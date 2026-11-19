#include "NeneModel.hpp"
#include "Common.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <algorithm>
#include <utility>
#include <vector>

#include <GL/glew.h>

#include <CubismModelSettingJson.hpp>
#include <Id/CubismIdManager.hpp>
#include <Motion/CubismMotion.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>

#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

namespace
{
    // Finds the matching closing bracket for the bracket at openPos.
    size_t FindMatchingBracket(const std::string& text, size_t openPos)
    {
        if (openPos >= text.size())
        {
            return std::string::npos;
        }
        const char open = text[openPos];
        const char close = (open == '[') ? ']' : '}';
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (size_t i = openPos; i < text.size(); ++i)
        {
            const char c = text[i];
            if (inString)
            {
                if (escaped)
                {
                    escaped = false;
                }
                else if (c == '\\')
                {
                    escaped = true;
                }
                else if (c == '"')
                {
                    inString = false;
                }
                continue;
            }
            if (c == '"')
            {
                inString = true;
                continue;
            }
            if (c == open)
            {
                ++depth;
            }
            else if (c == close)
            {
                --depth;
                if (depth == 0)
                {
                    return i;
                }
            }
        }
        return std::string::npos;
    }

    std::string ReadJsonStringAfter(const std::string& text, size_t begin, size_t end,
                                    const char* key)
    {
        const std::string quotedKey = std::string("\"") + key + "\"";
        const size_t keyPos = text.find(quotedKey, begin);
        if (keyPos == std::string::npos || keyPos >= end)
        {
            return std::string();
        }
        const size_t colon = text.find(':', keyPos + quotedKey.size());
        if (colon == std::string::npos || colon >= end)
        {
            return std::string();
        }
        size_t valueBegin = colon + 1;
        while (valueBegin < end && std::isspace(static_cast<unsigned char>(text[valueBegin])))
        {
            ++valueBegin;
        }
        if (valueBegin >= end || text[valueBegin] != '"')
        {
            return std::string();
        }
        ++valueBegin;
        size_t valueEnd = valueBegin;
        bool escaped = false;
        while (valueEnd < end)
        {
            const char c = text[valueEnd];
            if (escaped)
            {
                escaped = false;
            }
            else if (c == '\\')
            {
                escaped = true;
            }
            else if (c == '"')
            {
                break;
            }
            ++valueEnd;
        }
        return text.substr(valueBegin, valueEnd - valueBegin);
    }

    // Removes every motion curve except ParamHair* and ParamBreath from the
    // "Curves" array. This keeps the authored hair animation while dropping
    // head/body/arm/mouth/eye curves.
    std::string FilterIdleMotionCurves(const std::string& json, int* totalCurves, int* keptCurves)
    {
        *totalCurves = 0;
        *keptCurves = 0;

        const std::string key = "\"Curves\"";
        const size_t keyPos = json.find(key);
        if (keyPos == std::string::npos)
        {
            return json;
        }
        const size_t arrayBegin = json.find('[', keyPos + key.size());
        if (arrayBegin == std::string::npos)
        {
            return json;
        }
        const size_t arrayEnd = FindMatchingBracket(json, arrayBegin);
        if (arrayEnd == std::string::npos)
        {
            return json;
        }

        std::vector<std::pair<size_t, size_t>> curves;
        size_t cursor = arrayBegin + 1;
        while (cursor < arrayEnd)
        {
            while (cursor < arrayEnd &&
                   (std::isspace(static_cast<unsigned char>(json[cursor])) || json[cursor] == ','))
            {
                ++cursor;
            }
            if (cursor >= arrayEnd)
            {
                break;
            }
            if (json[cursor] != '{')
            {
                ++cursor;
                continue;
            }
            const size_t objectEnd = FindMatchingBracket(json, cursor);
            if (objectEnd == std::string::npos || objectEnd > arrayEnd)
            {
                break;
            }
            curves.emplace_back(cursor, objectEnd);
            cursor = objectEnd + 1;
        }

        *totalCurves = static_cast<int>(curves.size());

        std::string filtered;
        filtered.reserve(json.size());
        filtered.append(json, 0, arrayBegin + 1);
        bool first = true;
        for (const auto& range : curves)
        {
            const std::string id = ReadJsonStringAfter(json, range.first, range.second, "Id");
            const bool keep = (id.rfind("ParamHair", 0) == 0) || (id == "ParamBreath");
            if (!keep)
            {
                continue;
            }
            if (!first)
            {
                filtered += ',';
            }
            filtered.append(json, range.first, range.second - range.first + 1);
            first = false;
            ++(*keptCurves);
        }
        filtered.append(json, arrayEnd, std::string::npos);
        return filtered;
    }

    // Reads a PNG from disk and uploads it as an RGBA8 OpenGL texture.
    //
    // The PNG is already in the display's gamma space (8-bit sRGB); the
    // Cubism shader multiplies it by the vertex color and writes straight to
    // the framebuffer, so we deliberately keep the *unsized* RGBA internal
    // format and never enable GL_FRAMEBUFFER_SRGB: switching the texture to
    // GL_SRGB8_ALPHA8 here would apply one decode without a matching encode
    // and shift every mid-tone.
    GLuint CreateTextureFromPng(const std::string& path, bool useMipmap,
                                int anisoLevel, float lodBias)
    {
        std::string bytes;
        if (!ReadAllBytes(path, bytes))
        {
            LogMsg("[texture] failed to read %s", path.c_str());
            return 0;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(bytes.data()),
            static_cast<int>(bytes.size()),
            &width, &height, &channels, 4);
        if (!pixels)
        {
            LogMsg("[texture] stbi decode failed: %s (%s)", path.c_str(), stbi_failure_reason());
            return 0;
        }

        // Mipmap generation needs the full chain; 8192x8192 RGBA8 is ~357 MB
        // including all levels.
        const bool canMipmap = useMipmap && width >= 2 && height >= 2;

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        canMipmap ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (canMipmap)
        {
            // A negative bias samples a sharper mip level (the model is
            // displayed at roughly 1/7 of the texture resolution, so the
            // trilinear choice is otherwise visibly soft on 4K/8K atlases).
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, lodBias);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (canMipmap)
        {
            glGenerateMipmap(GL_TEXTURE_2D);
        }

        GLfloat maxAniso = 1.0f;
        if (anisoLevel > 1 && glewIsSupported("GL_EXT_texture_filter_anisotropic"))
        {
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
            const GLfloat requested = std::min(static_cast<GLfloat>(anisoLevel), maxAniso);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, requested);
            LogMsg("[texture] anisotropic filtering %.0fx (max %.0fx)", requested, maxAniso);
        }
        else if (anisoLevel > 1)
        {
            LogMsg("[texture] GL_EXT_texture_filter_anisotropic unavailable");
        }

        glBindTexture(GL_TEXTURE_2D, 0);

        LogMsg("[texture] %s -> id=%u %dx%d mipmap=%d lod-bias=%.2f", path.c_str(), texture,
               width, height, canMipmap ? 1 : 0, lodBias);

        stbi_image_free(pixels);
        return texture;
    }
}

NeneModel::NeneModel()
    : _setting(nullptr)
    , _idleMotion(nullptr)
    , _breathOnly(false)
    , _defaultSmile(true)
    , _elapsedSeconds(0.0f)
    , _idParamBreath(nullptr)
    , _idParamMouthForm(nullptr)
    , _idParamMouthOpenY(nullptr)
    , _idParamEyeLSmile(nullptr)
    , _idParamEyeRSmile(nullptr)
    , _idParamAngleX(nullptr)
    , _idParamAngleY(nullptr)
    , _idParamAngleZ(nullptr)
    , _idParamEyeBallX(nullptr)
    , _idParamEyeBallY(nullptr)
    , _idParamBodyAngleX(nullptr)
    , _idParamBodyAngleY(nullptr)
    , _idParamBodyAngleZ(nullptr)
    , _trackingGain(1.0f)
    , _motionGain(1.0f)
    , _bodySwayEnabled(true)
    , _trackTargetX(0.0f)
    , _trackTargetY(0.0f)
    , _trackCurrentX(0.0f)
    , _trackCurrentY(0.0f)
{
}

NeneModel::~NeneModel()
{
    for (auto& entry : _expressions)
    {
        Csm::ACubismMotion::Delete(entry.second);
    }
    _expressions.clear();

    if (_idleMotion)
    {
        Csm::ACubismMotion::Delete(_idleMotion);
        _idleMotion = nullptr;
    }
    if (_headPatMotion)
    {
        Csm::ACubismMotion::Delete(_headPatMotion);
        _headPatMotion = nullptr;
    }
    if (_stareMotion)
    {
        Csm::ACubismMotion::Delete(_stareMotion);
        _stareMotion = nullptr;
    }
    if (_chestMotion)
    {
        Csm::ACubismMotion::Delete(_chestMotion);
        _chestMotion = nullptr;
    }

    delete _setting;
    _setting = nullptr;
}

bool NeneModel::Load(const std::string& modelDirUtf8, bool breathOnly)
{
    _modelDir = modelDirUtf8;
    _breathOnly = breathOnly;
    _elapsedSeconds = 0.0f;

    Csm::CubismIdManager* idManager = Csm::CubismFramework::GetIdManager();
    _idParamBreath = idManager->GetId("ParamBreath");
    _idParamMouthForm = idManager->GetId("ParamMouthForm");
    _idParamMouthOpenY = idManager->GetId("ParamMouthOpenY");
    // Shy expression switches (measured: these three are the visible ones).
    _idParamCheek = idManager->GetId("ParamCheek");
    _idParamCheek5 = idManager->GetId("ParamCheek5");
    _idParamCheek6 = idManager->GetId("ParamCheek6");
    _idParamEyeLSmile = idManager->GetId("ParamEyeLSmile");
    _idParamEyeRSmile = idManager->GetId("ParamEyeRSmile");
    _idParamAngleX = idManager->GetId("ParamAngleX");
    _idParamAngleY = idManager->GetId("ParamAngleY");
    _idParamAngleZ = idManager->GetId("ParamAngleZ");
    _idParamEyeBallX = idManager->GetId("ParamEyeBallX");
    _idParamEyeBallY = idManager->GetId("ParamEyeBallY");
    _idParamBodyAngleX = idManager->GetId("ParamBodyAngleX");
    _idParamBodyAngleY = idManager->GetId("ParamBodyAngleY");
    _idParamBodyAngleZ = idManager->GetId("ParamBodyAngleZ");
    _idParamWholeAngleZ = idManager->GetId("Param64");

    // 扫描目录自动定位 .model3.json（避免硬编码中文文件名导致的编码问题）
    std::string settingPath;
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::u8path(_modelDir), ec))
        {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().u8string();
            if (name.rfind("Live2DViewerEX", 0) == 0) continue;
            if (name.size() > 12 &&
                name.compare(name.size() - 12, 12, ".model3.json") == 0)
            {
                settingPath = entry.path().u8string().c_str() ? std::string(reinterpret_cast<const char*>(entry.path().u8string().c_str())) : std::string();
                break;
            }
        }
    }
    if (settingPath.empty())
    {
        LogMsg("[model] model3.json not found in %s", _modelDir.c_str());
        return false;
    }
    std::string settingBytes;
    if (!ReadAllBytes(settingPath, settingBytes))
    {
        LogMsg("[model] cannot read %s", settingPath.c_str());
        return false;
    }

    _setting = new Csm::CubismModelSettingJson(
        reinterpret_cast<const Csm::csmByte*>(settingBytes.data()),
        static_cast<Csm::csmSizeInt>(settingBytes.size()));
    LogMsg("[model] setting parsed");
    if (!_setting)
    {
        LogMsg("[model] CubismModelSettingJson failed");
        return false;
    }

    // --- moc3 -------------------------------------------------------------
    {
        const std::string mocPath = PathJoin(_modelDir, _setting->GetModelFileName());
        LogMsg("[model] reading moc3: %s", mocPath.c_str());
        std::string mocBytes;
        if (!ReadAllBytes(mocPath, mocBytes))
        {
            LogMsg("[model] cannot read moc3: %s", mocPath.c_str());
            return false;
        }
        LoadModel(reinterpret_cast<const Csm::csmByte*>(mocBytes.data()),
                  static_cast<Csm::csmSizeInt>(mocBytes.size()));
        if (!GetModel())
        {
            LogMsg("[model] LoadModel failed (moc version mismatch?)");
            return false;
        }
    }

    LogMsg("[model] canvas %.2f x %.2f", GetModel()->GetCanvasWidth(), GetModel()->GetCanvasHeight());

    // Model matrix (created by LoadModel) scales the model to height 2 and
    // centers it on the origin already; nothing else to do here.

    // --- renderer / textures ---------------------------------------------
    LogMsg("[model] creating renderer");
    CreateRenderer(640, 480);
    LogMsg("[model] renderer created, loading textures");
    SetupTextures();
    LogMsg("[model] textures done");
    GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>()->IsPremultipliedAlpha(false);

    // --- physics ----------------------------------------------------------
    if (strcmp(_setting->GetPhysicsFileName(), "") != 0)
    {
        const std::string physicsPath = PathJoin(_modelDir, _setting->GetPhysicsFileName());
        std::string physicsBytes;
        if (ReadAllBytes(physicsPath, physicsBytes))
        {
            LoadPhysics(reinterpret_cast<const Csm::csmByte*>(physicsBytes.data()),
                        static_cast<Csm::csmSizeInt>(physicsBytes.size()));
            LogMsg("[model] physics loaded (%s)", _physics ? "ok" : "failed");
        }
    }

    // --- eye blink ids ----------------------------------------------------
    for (Csm::csmInt32 i = 0; i < _setting->GetEyeBlinkParameterCount(); ++i)
    {
        _eyeBlinkIds.PushBack(_setting->GetEyeBlinkParameterId(i));
    }
    for (Csm::csmInt32 i = 0; i < _setting->GetLipSyncParameterCount(); ++i)
    {
        _lipSyncIds.PushBack(_setting->GetLipSyncParameterId(i));
    }

    _eyeBlink = Csm::CubismEyeBlink::Create(_setting);

    // --- idle motion (loop) ----------------------------------------------
    if (!_breathOnly)
    {
        // Scan the motion directory, list every motion with a 1-based index,
        // then pick the default (largest file) or the one requested with
        // --motion (index or file-name substring). No Chinese name is
        // hard-coded anywhere.
        struct MotionEntry { std::string name; std::string path; uintmax_t size; };
        std::vector<MotionEntry> motions;
        {
            std::error_code ec;
            for (const auto& entry :
                 fs::directory_iterator(fs::u8path(_modelDir) / "motion", ec))
            {
                if (!entry.is_regular_file(ec)) continue;
                const std::string name = entry.path().filename().u8string();
                if (name.size() <= 13 ||
                    name.compare(name.size() - 13, 13, ".motion3.json") != 0)
                    continue;
                motions.push_back({name, entry.path().u8string(), entry.file_size(ec)});
            }
            std::sort(motions.begin(), motions.end(),
                      [](const MotionEntry& a, const MotionEntry& b)
                      { return a.name < b.name; });
        }
        for (size_t i = 0; i < motions.size(); ++i)
        {
            LogMsg("[motion] %2d: %s (%llu bytes)", static_cast<int>(i + 1),
                   motions[i].name.c_str(),
                   static_cast<unsigned long long>(motions[i].size));
        }

        MotionEntry* selected = nullptr;
        if (!motions.empty())
        {
            if (_motionSpec.empty())
            {
                // Default: the largest file (the authored full idle animation).
                selected = &motions[0];
                for (auto& m : motions)
                {
                    if (m.size > selected->size) selected = &m;
                }
            }
            else
            {
                bool digits = !_motionSpec.empty();
                for (char ch : _motionSpec)
                {
                    if (ch < '0' || ch > '9') { digits = false; break; }
                }
                if (digits)
                {
                    const int index = atoi(_motionSpec.c_str());
                    if (index >= 1 && index <= static_cast<int>(motions.size()))
                    {
                        selected = &motions[index - 1];
                    }
                    else
                    {
                        LogMsg("[motion] index %d out of range 1..%d", index,
                               static_cast<int>(motions.size()));
                    }
                }
                else
                {
                    for (auto& m : motions)
                    {
                        if (m.name.find(_motionSpec) != std::string::npos)
                        {
                            selected = &m;
                            break;
                        }
                    }
                    if (!selected)
                    {
                        LogMsg("[motion] no motion matching '%s'", _motionSpec.c_str());
                    }
                }
            }
        }

        std::string motionPath = selected ? selected->path : std::string();        std::string motionBytes;
        if (!motionPath.empty() && ReadAllBytes(motionPath, motionBytes))
        {
            std::string payload = motionBytes;
            if (_filterMotion)
            {
                int totalCurves = 0;
                int keptCurves = 0;
                payload = FilterIdleMotionCurves(motionBytes, &totalCurves, &keptCurves);
                LogMsg("[model] idle motion FILTERED: kept %d / %d curves "
                       "(ParamHair*, ParamBreath)", keptCurves, totalCurves);
            }
            else
            {
                LogMsg("[model] idle motion: full curve set (no filtering)");
            }

            _idleMotion = LoadMotion(
                reinterpret_cast<const Csm::csmByte*>(payload.data()),
                static_cast<Csm::csmSizeInt>(payload.size()),
                "idle", nullptr, nullptr);
            if (_idleMotion)
            {
                Csm::CubismMotion* motion = static_cast<Csm::CubismMotion*>(_idleMotion);
                motion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
                motion->SetLoop(true);
                _motionManager->StartMotionPriority(_idleMotion, false, 1);
                LogMsg("[model] idle motion started: %s (duration %.2f s)",
                       selected->name.c_str(), motion->GetDuration());
            }
            else
            {
                LogMsg("[model] idle motion failed to load: %s", motionPath.c_str());
            }
        }
        else
        {
            LogMsg("[model] idle motion missing (no .motion3.json in motion/)");
        }

        // --- one-shot interaction motions ---------------------------------
        // Matched by a short name fragment (the file names are Chinese); no
        // full file name is hard-coded. Both are forced to play once with a
        // 0.4 s fade-in and 0.6 s fade-out over the looping idle motion.
        auto loadAction = [&](const std::string& path, const std::string& name)
            -> Csm::ACubismMotion*
        {
            std::string bytes;
            if (!ReadAllBytes(path, bytes))
            {
                LogMsg("[action] cannot read %s", path.c_str());
                return nullptr;
            }
            Csm::ACubismMotion* motion = LoadMotion(
                reinterpret_cast<const Csm::csmByte*>(bytes.data()),
                static_cast<Csm::csmSizeInt>(bytes.size()), name.c_str(), nullptr, nullptr);
            if (!motion)
            {
                LogMsg("[action] failed to load %s", name.c_str());
                return nullptr;
            }
            motion->SetLoop(false);
            motion->SetFadeInTime(0.4f);
            motion->SetFadeOutTime(0.6f);
            static_cast<Csm::CubismMotion*>(motion)->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
            LogMsg("[action] motion loaded: %s (duration %.2f s, once, fade 0.4/0.6)",
                   name.c_str(), static_cast<Csm::CubismMotion*>(motion)->GetDuration());
            return motion;
        };
        for (auto& m : motions)
        {
            if (_headPatMotion == nullptr && m.name.find("摸头") != std::string::npos)
            {
                _headPatMotion = loadAction(m.path, m.name);
            }
            if (_stareMotion == nullptr && m.name.find("盯") != std::string::npos)
            {
                _stareMotion = loadAction(m.path, m.name);
            }
            if (_chestMotion == nullptr && m.name.find("0721") != std::string::npos)
            {
                _chestMotion = loadAction(m.path, m.name);
            }
        }
        if (_headPatMotion == nullptr) LogMsg("[action] no 摸头 motion found");
        if (_stareMotion == nullptr) LogMsg("[action] no 盯 motion found");
        if (_chestMotion == nullptr) LogMsg("[action] no 0721 motion found");
    }
    else
    {
        LogMsg("[model] --idle-breath-only: idle motion disabled; using breath + hair physics");
    }

    // --- expressions ------------------------------------------------------
    {
        const std::string exprDir = PathJoin(_modelDir, "expression");
        std::error_code ec;
        if (fs::is_directory(fs::u8path(exprDir), ec))
        {
            for (const auto& entry : fs::directory_iterator(fs::u8path(exprDir), ec))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                const std::string fileName = entry.path().filename().u8string();
                if (fileName.size() < 10 ||
                    fileName.compare(fileName.size() - 10, 10, ".exp3.json") != 0)
                {
                    continue;
                }
                std::string bytes;
                if (!ReadAllBytes(entry.path().u8string(), bytes))
                {
                    continue;
                }
                const std::string name = entry.path().stem().u8string();
                Csm::ACubismMotion* expression = LoadExpression(
                    reinterpret_cast<const Csm::csmByte*>(bytes.data()),
                    static_cast<Csm::csmSizeInt>(bytes.size()),
                    name.c_str());
                if (expression)
                {
                    _expressions[name] = expression;
                    LogMsg("[model] expression loaded: %s", name.c_str());
                }
            }
        }
    }

    LogMsg("[model] loaded: %s", settingPath.c_str());
    return true;
}

void NeneModel::SetupTextures()
{
    Csm::Rendering::CubismRenderer_OpenGLES2* renderer =
        GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    if (!renderer)
    {
        return;
    }

    for (Csm::csmInt32 i = 0; i < _setting->GetTextureCount(); ++i)
    {
        const char* textureName = _setting->GetTextureFileName(i);
        if (!textureName || strcmp(textureName, "") == 0)
        {
            continue;
        }
        const GLuint texture = CreateTextureFromPng(PathJoin(_modelDir, textureName),
                                                    _useMipmap, _anisoLevel, _lodBias);
        if (texture != 0)
        {
            renderer->BindTexture(static_cast<Csm::csmUint32>(i), texture);
        }
    }
}

void NeneModel::Update(float deltaSeconds)
{
    Csm::CubismModel* model = GetModel();
    if (!model)
    {
        return;
    }

    _elapsedSeconds += deltaSeconds;

    model->LoadParameters();

    // --- filtered idle motion --------------------------------------------
    // Only ParamHair* / ParamBreath curves are kept by the loader. The hair
    // curves are superseded by the physics evaluation below; this is just the
    // authored idle animation source.
    if (!_breathOnly)
    {
        if (_motionManager->IsFinished())
        {
            if (_idleMotion)
            {
                _motionManager->StartMotionPriority(_idleMotion, false, 1);
            }
        }
        else
        {
            _motionManager->UpdateMotion(model, deltaSeconds);
        }
    }

    // --- one-shot interaction layer (head pat / stare) ---------------------
    // Runs on its own motion manager and blends over the idle result with the
    // motion's own fade weights (0.4 s in / 0.6 s out), so the idle motion
    // resumes seamlessly when it ends.
    if (_actionPlaying)
    {
        _actionMotionManager.UpdateMotion(model, deltaSeconds);
        _actionElapsed += deltaSeconds;
        if (_actionMotionManager.IsFinished())
        {
            _actionPlaying = false;
            LogMsg("[action] finished after %.2f s (back to idle)", _actionElapsed);
        }
    }

    // --- 1) input parameters (mouse tracking / breath / smile) ------------
    // The authored motion has already written its curves (head sway, shoulder
    // micro-motion, arms, eyes ...). We only *add* the mouse-tracking offset on
    // top of them, clamped to the model's parameter range, so the motion is
    // never thrown away and the tracking stays additive (no snapping).
    {
        const float alpha = 1.0f - expf(-deltaSeconds * 9.8f);
        _trackCurrentX += (_trackTargetX - _trackCurrentX) * alpha;
        _trackCurrentY += (_trackTargetY - _trackCurrentY) * alpha;

        const float eyeX = std::max(-1.0f, std::min(1.0f,
                                 _trackCurrentX * _trackingGain));
        const float eyeY = std::max(-1.0f, std::min(1.0f,
                                 -_trackCurrentY * _trackingGain));
        const float blend = _trackBlend * _motionGain;
        const float twoPi = 6.28318530718f;
        const float breathPhase = sinf(twoPi * 0.25f * _elapsedSeconds);

        auto addClamped = [&](const Csm::CubismIdHandle id, float delta, float lo, float hi)
        {
            const float value = model->GetParameterValue(id) + delta;
            model->SetParameterValue(id, std::max(lo, std::min(hi, value)));
        };

        addClamped(_idParamEyeBallX, 0.25f * eyeX * blend, -1.0f, 1.0f);
        addClamped(_idParamEyeBallY, 0.25f * eyeY * blend, -1.0f, 1.0f);

        // Head yaw / pitch: motion value + tracking offset (model range +-30).
        // The breathing adds a tiny neck nod (+-1.2 deg) so inhale/exhale is
        // readable and gently drives the hair physics. It is vertical only.
        addClamped(_idParamAngleX, 15.0f * eyeX * blend, -30.0f, 30.0f);
        addClamped(_idParamAngleY, 10.0f * eyeY * blend + 1.2f * breathPhase,
                   -30.0f, 30.0f);
        // Head roll: the authored idle curve sways the head +-15 deg left/right.
        // That auto-sway is disabled too (the head must only follow the mouse);
        // this also stops it from feeding the hair physics on its own.
        model->SetParameterValue(_idParamAngleZ, 0.0f);

        // Shoulder / upper-body follow (model range +-10). This is the *only*
        // body yaw: the authored idle motion sways the waist with
        // ParamBodyAngleZ up to ~19 deg, which reads as "the whole body keeps
        // swinging left/right", so that curve is suppressed below.
        float bodyYaw = 3.0f * eyeX * blend;
        if (_bodySwayEnabled)
        {
            const float twoPi = 6.28318530718f;
            bodyYaw += 0.75f * _motionGain * sinf(twoPi * 0.08f * _elapsedSeconds);
        }
        model->SetParameterValue(_idParamBodyAngleX,
                                 std::max(-10.0f, std::min(10.0f, bodyYaw)));
        // Body sway / lean from the motion is dropped entirely: the torso must
        // stay upright, only the head/arms/hair animate.
        model->SetParameterValue(_idParamBodyAngleY, 0.0f);
        model->SetParameterValue(_idParamBodyAngleZ, 0.0f);
        model->SetParameterValue(_idParamWholeAngleZ, 0.0f);
    }

    // --- programmatic breath (0 .. 0.9, 0.25 Hz) --------------------------
    // The authored idle motion has ParamBreath pinned to 0, and this rig's
    // breath deformation is subtle (the chest outline moves a few px), so the
    // full 0..1 range is used to keep inhale/exhale visible.
    {
        const float twoPi = 6.28318530718f;
        model->SetParameterValue(_idParamBreath,
                                 0.45f + 0.45f * sinf(twoPi * 0.25f * _elapsedSeconds));
    }
    // --- smile: mouth only ------------------------------------------------
    // The authored idle motion opens/closes the mouth (ParamMouthOpenY), which
    // reads as talking; the mouth must stay closed with a constant smile, so
    // the opening is pinned to 0 and only the mouth form stays at 0.7.
    model->SetParameterValue(_idParamMouthOpenY, 0.0f);
    if (_defaultSmile)
    {
        model->SetParameterValue(_idParamMouthForm, 0.7f);
    }
    // Never let any expression curl the eyes: keep normal open eyes.
    model->SetParameterValue(_idParamEyeLSmile, 0.0f);
    model->SetParameterValue(_idParamEyeRSmile, 0.0f);

    // --- shy face (chest) + blush (head-pat) ------------------------------
    // Both are written unconditionally every frame (0 when inactive) so the
    // authored motion / previous frame can never leave the face blushed:
    // the head-pat motion itself also touches ParamCheek, and a parameter
    // written by a finished motion would otherwise stick forever.
    float shyWeight = 0.0f;
    if (_shyTime >= 0.0f)
    {
        const float fadeIn = 0.25f;
        const float hold = 2.5f;
        const float fadeOut = 0.4f;
        _shyTime += deltaSeconds;
        if (_shyTime < fadeIn)
        {
            shyWeight = _shyTime / fadeIn;
        }
        else if (_shyTime < fadeIn + hold)
        {
            shyWeight = 1.0f;
        }
        else if (_shyTime < fadeIn + hold + fadeOut)
        {
            shyWeight = 1.0f - (_shyTime - fadeIn - hold) / fadeOut;
        }
        else
        {
            _shyTime = -1.0f;
            LogMsg("[action] shy finished (back to normal face) at t=%.2f s",
                   _elapsedSeconds);
        }
    }

    // Blush that rides along with whichever one-shot action is playing:
    // 0.3 s in, held for the motion, 0.4 s out before it ends.
    float blushWeight = 0.0f;
    if (_actionBlushTime >= 0.0f)
    {
        _actionBlushTime += deltaSeconds;
        blushWeight = _actionBlushTime < 0.3f ? _actionBlushTime / 0.3f : 1.0f;
        const float remaining = _actionBlushDuration - _actionBlushTime;
        if (remaining < 0.4f)
        {
            blushWeight = std::max(0.0f, remaining / 0.4f);
        }
        if (remaining <= 0.0f)
        {
            _actionBlushTime = -1.0f;
            blushWeight = 0.0f;
        }
    }

    // 脸红 (blush) is shared; 害羞 / 斜线 only for the chest-shy expression.
    model->SetParameterValue(_idParamCheek, std::max(shyWeight, blushWeight));
    model->SetParameterValue(_idParamCheek5, shyWeight);
    model->SetParameterValue(_idParamCheek6, shyWeight);

    // Debug overrides are applied before physics so they can be used to drive
    // the simulation as well as the pose.
    for (const auto& entry : _debugParameters)
    {
        const Csm::CubismIdHandle id =
            Csm::CubismFramework::GetIdManager()->GetId(entry.first.c_str());
        model->SetParameterValue(id, entry.second);
    }

    // --- 2) official physics ---------------------------------------------
    // physics3.json drives ParamHair* (head angle), the skirt chain
    // Param13/14/15/65 + Param16/17/18 and the arm/cloth helpers. It never
    // writes ParamAngle* / ParamBodyAngle* / Param64, so it cannot tilt the
    // body by itself (verified against the Output list of all 52 settings).
    if (_physicsEnabled && _physics)
    {
        _physics->Evaluate(model, deltaSeconds);
    }

    // --- hand-written idle hair breeze (ADDITIVE, after physics) ----------
    // Physics only reacts to motion, so a still character has perfectly static
    // hair. This adds a very light two-sine breeze on top of the physics
    // result (never overwriting it), clamped to each parameter's range.
    // Grouping: 0 = ParamHairFront* (bangs), 1 = ParamHairSide* (side),
    // 2 = ParamHairBack* / BackL* / BackR* (back chains). Each group and each
    // rope segment gets its own phase, and the tip amplitude grows with the
    // trailing segment number so roots stay quiet and tips travel more.
    if (_hairSwayGain > 0.0f)
    {
        const float twoPi = 6.28318530718f;
        const float t = _elapsedSeconds;
        const Csm::csmInt32 count = model->GetParameterCount();
        for (Csm::csmInt32 i = 0; i < count; ++i)
        {
            const char* id = model->GetParameterId(i)->GetString().GetRawString();
            if (strncmp(id, "ParamHair", 9) != 0)
            {
                continue;
            }
            const char* suffix = id + 9;
            int group = 2;
            if (strncmp(suffix, "Front", 5) == 0)
            {
                group = 0;
            }
            else if (strncmp(suffix, "Side", 4) == 0)
            {
                group = 1;
            }

            int segment = 0;
            const size_t length = strlen(id);
            size_t digitBegin = length;
            while (digitBegin > 9 && isdigit(static_cast<unsigned char>(id[digitBegin - 1])))
            {
                --digitBegin;
            }
            if (digitBegin < length)
            {
                segment = atoi(id + digitBegin);
            }

            const float amplitude = (1.00f + 0.30f * static_cast<float>(segment)) *
                                    _hairSwayGain;
            const float phase = static_cast<float>(group) * 2.0944f +
                                static_cast<float>(segment) * 0.30f;
            const float sway = 0.6f * sinf(twoPi * 0.6f * t + phase) +
                               0.4f * sinf(twoPi * 0.9f * t + phase * 0.5f + 0.5f);
            const float lo = model->GetParameterMinimumValue(i);
            const float hi = model->GetParameterMaximumValue(i);
            const float value = model->GetParameterValue(i) + amplitude * sway;
            model->SetParameterValue(i, std::max(lo, std::min(hi, value)));
        }
    }

    // Diagnostic: log a few physics-driven parameters once per second so the
    // effect of the tracking inputs on the simulation can be verified.
    if (_paramLogDivider > 0 && (++_paramLogCounter % _paramLogDivider) == 0)
    {
        static const char* ids[] = {"ParamAngleX", "ParamAngleY", "ParamAngleZ",
                                    "ParamAngleZ4", "ParamAngleZ5",
                                    "ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ",
                                    "Param64", "Param62", "Param63",
                                    "Param42", "Param40", "Param37",
                                    "ParamBreath", "ParamMouthForm", "ParamMouthOpenY",
                                    "ParamCheek", "ParamCheek5", "ParamCheek6",
                                    "ParamHairSideL", "ParamHairSideL3", "ParamHairBackL4",
                                    "ParamHairFront", "Param13", "Param15", "Param65",
                                    "Param16", "Param17", "Param18", "Param33"};
        char line[1024];
        int used = snprintf(line, sizeof(line), "t=%.2f ", _elapsedSeconds);
        for (const char* id : ids)
        {
            const Csm::CubismIdHandle handle =
                Csm::CubismFramework::GetIdManager()->GetId(id);
            used += snprintf(line + used, sizeof(line) - used, "%s=%.2f ", id,
                             model->GetParameterValue(handle));
        }
        LogMsg("[params] %s", line);
    }

    // --- 3) expression / eye blink / update -------------------------------
    _expressionManager->UpdateMotion(model, deltaSeconds);

    model->SaveParameters();

    if (_eyeBlink)
    {
        _eyeBlink->UpdateParameters(model, deltaSeconds);
    }

    model->Update();
}

void NeneModel::Draw(int width, int height, float centerX, float centerY, float halfHeight,
                     float halfWidth)
{
    Csm::Rendering::CubismRenderer_OpenGLES2* renderer =
        GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    if (!renderer || !GetModel())
    {
        return;
    }

    // A narrow window (e.g. 320x480) cannot show the whole model width at the
    // scale used for 640x480, because model space keeps a 1:1 pixel aspect.
    // The caller may therefore request an explicit horizontal half-width,
    // which enlarges the visible width without shrinking the character
    // vertically (a mild horizontal stretch, no cropping of the side hair).
    if (halfWidth <= 0.0f)
    {
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        halfWidth = halfHeight * aspect;
    }
    if (halfWidth <= 0.0001f)
    {
        halfWidth = 0.0001f;
    }

    // View matrix: maps model-matrix space (model height == 2, centered) to NDC.
    float viewArray[16] = {
        1.0f / halfWidth, 0.0f,             0.0f, 0.0f,
        0.0f,             1.0f / halfHeight, 0.0f, 0.0f,
        0.0f,             0.0f,             1.0f, 0.0f,
        -centerX / halfWidth, -centerY / halfHeight, 0.0f, 1.0f
    };

    Csm::CubismMatrix44 view;
    view.SetMatrix(viewArray);

    // mvp = view * modelMatrix (column-vector convention, as the shader uses
    // u_matrix * a_position and the array is column-major).
    Csm::CubismMatrix44 mvp;
    mvp.SetMatrix(GetModelMatrix()->GetArray());
    mvp.MultiplyByMatrix(&view);

    renderer->SetMvpMatrix(&mvp);
    renderer->DrawModel();
}

bool NeneModel::HasExpression(const std::string& name) const
{
    return _expressions.find(name) != _expressions.end();
}

bool NeneModel::ApplyExpression(const std::string& name)
{
    auto it = _expressions.find(name);
    if (it == _expressions.end())
    {
        return false;
    }
    _expressionManager->StartMotion(it->second, false);
    _defaultSmile = false;
    LogMsg("[model] expression applied: %s (default mouth-form smile disabled)", name.c_str());
    return true;
}

void NeneModel::SetTrackingGain(float gain)
{
    _trackingGain = std::max(0.0f, std::min(3.0f, gain));
}

void NeneModel::SetTrackingTarget(float x, float y)
{
    _trackTargetX = std::max(-1.0f, std::min(1.0f, x));
    _trackTargetY = std::max(-1.0f, std::min(1.0f, y));
}

void NeneModel::SetMotionGain(float gain)
{
    _motionGain = std::max(0.0f, std::min(3.0f, gain));
}

void NeneModel::SetBodySwayEnabled(bool enabled)
{
    _bodySwayEnabled = enabled;
}

void NeneModel::SetPhysicsEnabled(bool enabled)
{
    _physicsEnabled = enabled;
}

void NeneModel::SetMotionFiltering(bool filter)
{
    _filterMotion = filter;
}

void NeneModel::SetTrackBlend(float blend)
{
    _trackBlend = std::max(0.0f, std::min(1.0f, blend));
}

void NeneModel::SetMotionSelection(const std::string& spec)
{
    _motionSpec = spec;
}

void NeneModel::SetParamLogInterval(int frames)
{
    _paramLogDivider = frames > 0 ? frames : 0;
}

void NeneModel::SetHairSwayGain(float gain)
{
    _hairSwayGain = std::max(0.0f, std::min(3.0f, gain));
}

bool NeneModel::HitTestHead(float modelX, float modelY) const
{
    // Model-matrix space: the model is 2 units tall and centered. The face
    // center sits at y ~= 0.49, the chin at ~0.40 and the top hair at ~0.69
    // (measured from rendered frames + drawable bounds), so "head incl. upper
    // hair" is y > 0.44 within the head's horizontal extent.
    return modelY > 0.44f && modelY < 0.78f && std::fabs(modelX) < 0.28f;
}

bool NeneModel::HitTestChest(float modelX, float modelY) const
{
    // Below the head band and above the skirt: no overlap with HitTestHead.
    return modelY >= 0.10f && modelY <= 0.40f && std::fabs(modelX) < 0.20f;
}

// Shared "action blush": whichever one-shot motion is playing gets a blush
// that fades in over 0.3 s, holds, and fades out over its last 0.4 s.
void NeneModel::BeginActionBlush(Csm::ACubismMotion* motion)
{
    _actionBlushDuration = motion
        ? static_cast<Csm::CubismMotion*>(motion)->GetDuration()
        : 5.0f;
    if (_actionBlushDuration <= 0.0f)
    {
        _actionBlushDuration = 5.0f;
    }
    _actionBlushTime = 0.0f;
}

bool NeneModel::TriggerChestMotion()
{
    if (!_chestMotion)
    {
        LogMsg("[action] chest ignored (0721 motion not loaded)");
        return false;
    }
    if (IsActionPlaying())
    {
        LogMsg("[action] chest ignored (another action is playing)");
        return false;
    }
    _actionMotionManager.StopAllMotions();
    _chestMotion->SetLoop(false);
    _chestMotion->SetFadeInTime(0.4f);
    _chestMotion->SetFadeOutTime(0.6f);
    _actionMotionManager.StartMotionPriority(_chestMotion, false, 1);
    _actionPlaying = true;
    _actionElapsed = 0.0f;
    BeginActionBlush(_chestMotion);
    LogMsg("[action] TRIGGER chest-0721 at t=%.2f s", _elapsedSeconds);
    return true;
}

bool NeneModel::TriggerShy()
{
    _shyTime = 0.0f;
    LogMsg("[action] TRIGGER shy (blush) at t=%.2f s", _elapsedSeconds);
    return true;
}

bool NeneModel::TriggerHeadPat()
{
    if (!_headPatMotion)
    {
        LogMsg("[action] head-pat ignored (motion not loaded)");
        return false;
    }
    _actionMotionManager.StopAllMotions();
    _headPatMotion->SetLoop(false);
    _headPatMotion->SetFadeInTime(0.4f);
    _headPatMotion->SetFadeOutTime(0.6f);
    _actionMotionManager.StartMotionPriority(_headPatMotion, false, 1);
    _actionPlaying = true;
    _actionElapsed = 0.0f;
    BeginActionBlush(_headPatMotion);   // blush rides along with the head-pat
    LogMsg("[action] TRIGGER head-pat (摸头) at t=%.2f s", _elapsedSeconds);
    return true;
}

bool NeneModel::TriggerStare()
{
    if (!_stareMotion)
    {
        LogMsg("[action] stare ignored (motion not loaded)");
        return false;
    }
    _actionMotionManager.StopAllMotions();
    _stareMotion->SetLoop(false);
    _stareMotion->SetFadeInTime(0.4f);
    _stareMotion->SetFadeOutTime(0.6f);
    _actionMotionManager.StartMotionPriority(_stareMotion, false, 1);
    _actionPlaying = true;
    _actionElapsed = 0.0f;
    LogMsg("[action] TRIGGER stare (盯) at t=%.2f s", _elapsedSeconds);
    return true;
}

bool NeneModel::IsActionPlaying()
{
    return _actionPlaying && !_actionMotionManager.IsFinished();
}

void NeneModel::SetDebugParameter(const std::string& id, float value)
{
    _debugParameters[id] = value;
}

void NeneModel::SetTextureFiltering(bool mipmap, int anisoLevel, float lodBias)
{
    _useMipmap = mipmap;
    _anisoLevel = std::max(1, std::min(16, anisoLevel));
    _lodBias = std::max(-4.0f, std::min(4.0f, lodBias));
}

// Dumps every drawable's post-deformation vertices in model-matrix space
// (model height == 2, centered on the origin). Screen pixels are obtained by
// dividing by (2 * halfHeight / windowHeight).
void NeneModel::DumpDrawables(const std::string& pathUtf8)
{
    Csm::CubismModel* model = GetModel();
    if (!model)
    {
        return;
    }
    const std::wstring widePath = WideFromUtf8(pathUtf8);
    FILE* file = _wfopen(widePath.c_str(), L"wb");
    if (!file)
    {
        LogMsg("[mesh] cannot write %s", pathUtf8.c_str());
        return;
    }
    fprintf(file, "Drawable,Id,Index,X,Y\n");
    const Csm::csmInt32 drawableCount = model->GetDrawableCount();
    for (Csm::csmInt32 d = 0; d < drawableCount; ++d)
    {
        const Csm::csmInt32 vertexCount = model->GetDrawableVertexCount(d);
        const Csm::csmFloat32* vertices = model->GetDrawableVertices(d);
        const char* id = model->GetDrawableId(d)->GetString().GetRawString();
        for (Csm::csmInt32 v = 0; v < vertexCount; ++v)
        {
            fprintf(file, "%d,%s,%d,%.6f,%.6f\n", d, id, v,
                    vertices[v * 2], vertices[v * 2 + 1]);
        }
    }
    fclose(file);
    LogMsg("[mesh] %d drawables -> %s", drawableCount, pathUtf8.c_str());
}

std::vector<std::string> NeneModel::GetExpressionNames() const
{
    std::vector<std::string> names;
    for (const auto& entry : _expressions)
    {
        names.push_back(entry.first);
    }
    return names;
}

float NeneModel::GetCanvasWidth() const
{
    return GetModel() ? GetModel()->GetCanvasWidth() : 0.0f;
}

float NeneModel::GetCanvasHeight() const
{
    return GetModel() ? GetModel()->GetCanvasHeight() : 0.0f;
}







