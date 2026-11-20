#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <malloc.h>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <random>

#include <GL/glew.h>

#include "Common.hpp"
#include "NeneModel.hpp"
#include "MonitorData.hpp"
#include "PanelRenderer.hpp"
#include "TrayIcon.hpp"

#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

std::string  g_exeDir;
std::wstring g_exeDirWide;

std::string Utf8FromWide(const std::wstring& w)
{
    if (w.empty())
    {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring WideFromUtf8(const std::string& s)
{
    if (s.empty())
    {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                         nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        result.data(), size);
    return result;
}

std::string PathJoin(const std::string& a, const std::string& b)
{
    if (a.empty())
    {
        return b;
    }
    if (b.empty())
    {
        return a;
    }
    const char last = a.back();
    if (last == '\\' || last == '/')
    {
        return a + b;
    }
    return a + "\\" + b;
}

bool ReadAllBytes(const std::string& pathUtf8, std::string& out)
{
    out.clear();
    HANDLE handle = CreateFileW(WideFromUtf8(pathUtf8).c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(handle, &fileSize) || fileSize.QuadPart < 0)
    {
        CloseHandle(handle);
        return false;
    }

    out.resize(static_cast<size_t>(fileSize.QuadPart));
    size_t offset = 0;
    while (offset < out.size())
    {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - offset, 1u << 24));
        DWORD read = 0;
        if (!ReadFile(handle, out.data() + offset, chunk, &read, nullptr) || read == 0)
        {
            CloseHandle(handle);
            out.clear();
            return false;
        }
        offset += read;
    }
    CloseHandle(handle);
    return true;
}

void LogMsg(const char* fmt, ...)
{
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[4224];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s",
             now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, buffer);

    OutputDebugStringA(line);
    OutputDebugStringA("\n");

    static HANDLE logFile = INVALID_HANDLE_VALUE;
    static bool logTried = false;
    if (!logTried)
    {
        logTried = true;
        const std::wstring logPath = WideFromUtf8(PathJoin(g_exeDir, "NeneCubism.log"));
        logFile = CreateFileW(logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (logFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(logFile, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        WriteFile(logFile, "\r\n", 2, &written, nullptr);
    }
}

namespace
{
    class AppAllocator : public Csm::ICubismAllocator
    {
    public:
        void* Allocate(const Csm::csmSizeType size) override
        {
            return malloc(size);
        }
        void Deallocate(void* memory) override
        {
            free(memory);
        }
        void* AllocateAligned(const Csm::csmSizeType size, const Csm::csmUint32 alignment) override
        {
            return _aligned_malloc(size, alignment);
        }
        void DeallocateAligned(void* alignedMemory) override
        {
            _aligned_free(alignedMemory);
        }
    };

    AppAllocator g_allocator;

    void CubismLogCallback(const Csm::csmChar* message)
    {
        LogMsg("[cubism] %s", message);
    }

    Csm::csmByte* LoadFileAsBytes(const std::string filePath, Csm::csmSizeInt* outSize)
    {
        std::string path = filePath;
        if (!path.empty() && path[0] != '\\' && path[0] != '/' &&
            !(path.size() > 1 && path[1] == ':'))
        {
            path = g_exeDir + path;
        }

        std::string bytes;
        if (!ReadAllBytes(path, bytes))
        {
            LogMsg("[file] cannot open %s", path.c_str());
            return nullptr;
        }

        Csm::csmByte* buffer = new Csm::csmByte[bytes.size()];
        memcpy(buffer, bytes.data(), bytes.size());
        *outSize = static_cast<Csm::csmSizeInt>(bytes.size());
        return buffer;
    }

    void ReleaseBytes(Csm::csmByte* byteData)
    {
        delete[] byteData;
    }

    // ---------------------------------------------------------------------
    // PNG snapshot helper
    // ---------------------------------------------------------------------
    struct PngWriteContext
    {
        FILE* file;
    };

    void PngWriteCallback(void* context, void* data, int size)
    {
        PngWriteContext* ctx = static_cast<PngWriteContext*>(context);
        if (ctx && ctx->file)
        {
            fwrite(data, 1, size, ctx->file);
        }
    }

    bool SaveFramebufferPng(const std::string& pathUtf8, int width, int height)
    {
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        // Flip vertically for PNG.
        std::vector<unsigned char> flipped(static_cast<size_t>(width) * height * 4);
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        for (int y = 0; y < height; ++y)
        {
            memcpy(flipped.data() + static_cast<size_t>(y) * rowBytes,
                   pixels.data() + static_cast<size_t>(height - 1 - y) * rowBytes,
                   rowBytes);
        }

        const std::wstring widePath = WideFromUtf8(pathUtf8);
        FILE* file = _wfopen(widePath.c_str(), L"wb");
        if (!file)
        {
            LogMsg("[snapshot] cannot write %s", pathUtf8.c_str());
            return false;
        }

        PngWriteContext context{file};
        const int ok = stbi_write_png_to_func(PngWriteCallback, &context, width, height, 4,
                                              flipped.data(), width * 4);
        fclose(file);
        LogMsg("[snapshot] %s (%dx%d) %s", pathUtf8.c_str(), width, height,
               ok ? "written" : "FAILED");
        return ok != 0;
    }

    struct Options
    {
        int x = 0;
        int y = 0;
        int width = 640;
        int height = 480;
        bool xSet = false;
        bool ySet = false;
        // Side panels (160 px left + 160 px right around a 320 px Live2D
        // viewport). Enabled by default; --no-panels restores the old
        // full-window Live2D-only mode.
        bool panels = true;
        // Window width/height are the full window; modelWidth/modelHeight are
        // what the Cubism camera is framed for (the middle 320x480 region).
        int modelWidth = 320;
        int modelHeight = 480;
        bool installAutostart = false;
        bool removeAutostart = false;
        // Monitor DPI scale for the 640x480 logical layout (1.5 at 144 DPI).
        // 0 = auto: use the target monitor's effective DPI, so the window fills
        // a high-DPI mini panel while keeping the specified 160/320/160 layout.
        float uiScale = 0.0f;
        int windowPixelWidth = 640;
        int windowPixelHeight = 480;
        float centerX = 0.0f;
        float centerY = 0.30f;
        float halfHeight = 0.42f;
        // Explicit horizontal half-width in model space; <= 0 means "use the
        // window aspect ratio" (the 640x480 default).
        float halfWidth = 0.0f;
        bool topmost = false;
        // True when --topmost was given explicitly; the command line always
        // wins over the saved state in model.cfg.
        bool topmostSet = false;
        // Topmost flag loaded from model.cfg (4th, optional field).
        bool configTopmost = false;
        bool configTopmostValid = false;
        bool noFocus = false;
        // True when the process was started without any CLI argument (e.g. a
        // double click on the packaged build). In that case a matching mini
        // screen implies topmost + no-focus so it behaves like a panel gadget.
        bool noArguments = false;
        bool placedOnMiniScreen = false;
        // Tray icon (default on). The window uses WS_EX_TOOLWINDOW so it does
        // not appear in the taskbar / Alt+Tab; --no-tray restores a normal
        // taskbar window for debugging.
        bool tray = true;
        bool halfHeightSet = false;
        bool centerXSet = false;
        bool halfWidthSet = false;
        COLORREF background = RGB(0xFF, 0xFF, 0xFF);
        // Empty = resolve to <exeDir>\model when that folder exists, otherwise
        // the development model path (keeps both the package and this tree
        // working without touching the command line).
        std::string modelDir;
        std::string expression;
        bool idleBreathOnly = false;
        float trackGain = 1.8f;   // 默认追踪强度（用户实测 1.8 最合适）
        float motionGain = 1.0f;
        bool bodySway = false;
        std::vector<std::pair<std::string, float>> debugParams;
        std::string snapshotPath;
        int snapshotFrame = 3;
        bool exitAfterSnapshot = false;
        // Diagnostics: dump every model parameter (id, min, max, default) as
        // CSV and optionally quit right away.
        std::string dumpParamsPath;
        // Diagnostics: dump every drawable vertex (post-deformation) as CSV.
        std::string dumpMeshPath;
        // Texture sampling overrides (see NeneModel::SetTextureFiltering).
        bool textureMipmap = true;
        int  textureAniso = 16;
        float textureLodBias = -0.5f;
        // Supersampling factor for the model pass (1 = off). The Cubism
        // renderer hard-codes GL_LINEAR_MIPMAP_LINEAR, so rendering above the
        // window resolution and resolving down is what actually restores
        // detail and smooths the silhouette edges.
        float ssaaFactor = 2.0f;
        bool hasTrackOverride = false;
        float trackOverrideX = 0.0f;
        float trackOverrideY = 0.0f;
        // Official physics on/off.
        bool physicsEnabled = true;
        // Motion: false = play all authored curves (default), true = keep only
        // ParamHair*/ParamBreath (old behaviour).
        bool filterMotion = false;
        // Weight of the tracking offset added on top of the motion.
        float trackBlend = 1.0f;
        // Amplitude of the hand-written idle hair breeze (0 = off).
        float hairSway = 1.0f;
        // "" = default idle (largest file), "2" = 1-based index, or a file-name
        // substring.
        std::string motionSpec;
        // Diagnostic: log physics-driven parameters every N frames.
        int paramLogInterval = 0;
        // Interaction: click head -> head-pat motion; chest -> 0721 motion.
        bool clickMotion = true;
        bool clickChest = true;
        bool chestShy = false;
        bool idleStare = false;
        float stareIntervalMin = 120.0f;
        float stareIntervalMax = 300.0f;
        int debugClickFrame = -1;
        int debugStareFrame = -1;
        int debugShyFrame = -1;
        int debugChestFrame = -1;
        std::string hitTestPixel;
        // Scripted mouse targets: frame -> normalized (x, y). Lets one process
        // capture a "flick" with real inertia (inertia is time dependent).
        struct TrackKey { int frame; float x; float y; };
        std::vector<TrackKey> trackSeq;
        // Scripted snapshots: frame -> file.
        struct SeqShot { int frame; std::string path; bool done = false; };
        std::vector<SeqShot> snapshotSeq;
    };

    // Framing measured from rendered snapshots: with halfHeight = 0.42 and
    // centerY = 0.30 the view shows the head top with a small gap and the
    // skirt hem close to the bottom edge (only the legs are cut below the
    // hem). The same numbers work for the 320x480 portrait window: the view
    // keeps 1:1 pixels, so the side hair still fits inside 320 px.
    // `--hw` stays available for manual tuning of the horizontal extent.
    void ResolveFraming(Options& options)
    {
        if (options.halfHeight <= 0.01f)
        {
            options.halfHeight = 0.01f;
        }
        const float aspect = static_cast<float>(options.modelWidth) /
                             static_cast<float>(options.modelHeight);
        if (options.halfWidth <= 0.0f)
        {
            options.halfWidth = options.halfHeight * aspect;
        }
        if (options.halfWidth <= 0.0001f)
        {
            options.halfWidth = 0.0001f;
        }
        // A portrait window (e.g. 320x480 on the mini screen) has very little
        // room left and right of the long side hair. Without an explicit
        // --hh, zoom out by 4% so the hair tips keep a safety margin instead
        // of touching the window edge while the hair physics swings.
        if (!options.halfHeightSet && aspect < 1.2f)
        {
            options.halfHeight *= 1.04f;
            options.halfWidth = options.halfHeight * aspect;
            // The artwork sits ~0.015 model units left of the canvas center
            // (asymmetric side hair); recenter it inside the narrow window.
            if (!options.centerXSet)
            {
                options.centerX = -0.015f;
            }
        }
    }

    bool ParseColor(const std::string& text, COLORREF& out)
    {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf_s(text.c_str(), "%2x%2x%2x", &r, &g, &b) == 3 &&
            text.size() >= 6)
        {
            out = RGB(r, g, b);
            return true;
        }
        return false;
    }

    bool LoadModelConfig(Options& options);
    float g_baseHalfHeight = 0.42f;

    Options ParseCommandLine()
    {
        Options options;
        options.x = 0;
        options.y = 0;
        // Target layout: one 640x480 mini-screen window (left 160 px panel,
        // middle 320 px Live2D, right 160 px panel). --size / --width /
        // --height still override it; the old fullscreen default would make
        // the side panels huge on a normal monitor.
        options.width = 640;
        options.height = 480;

        int argc = 0;
        LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argvW)
        {
            return options;
        }

        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i)
        {
            args.push_back(Utf8FromWide(argvW[i]));
        }
        LocalFree(argvW);
        options.noArguments = args.empty();

        // Packaged layout: <exeDir>\model next to the exe. Development layout
        // falls back to the original absolute model directory. No dependency
        // on the current working directory either way.
        if (options.modelDir.empty())
        {
            std::error_code ec;
            const std::wstring packaged = g_exeDirWide + L"model";
            if (fs::is_directory(fs::path(packaged), ec))
            {
                options.modelDir = Utf8FromWide(packaged);
                LogMsg("[args] using packaged model directory: %s",
                       options.modelDir.c_str());
            }
            else
            {
                options.modelDir = "E:\\L2D宁宁\\AYACHI NENE\\綾地寧々";
                LogMsg("[args] no <exeDir>\\model, using development model directory");
            }
        }

        int positional = 0;
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string& arg = args[i];
            auto next = [&](const char* name) -> std::string
            {
                if (i + 1 < args.size())
                {
                    return args[++i];
                }
                LogMsg("[args] missing value for %s", name);
                return std::string();
            };

            if (arg == "--model")
            {
                options.modelDir = next("--model");
            }
            else if (arg == "--expression")
            {
                options.expression = next("--expression");
            }
            else if (arg == "--idle-breath-only")
            {
                options.idleBreathOnly = true;
            }
            else if (arg == "--track-gain")
            {
                options.trackGain = static_cast<float>(atof(next("--track-gain").c_str()));
            }
            else if (arg == "--debug-param")
            {
                const std::string text = next("--debug-param");
                const size_t equals = text.find('=');
                if (equals != std::string::npos)
                {
                    options.debugParams.emplace_back(
                        text.substr(0, equals),
                        static_cast<float>(atof(text.c_str() + equals + 1)));
                }
                else
                {
                    LogMsg("[args] invalid --debug-param, expected Id=Value");
                }
            }
            else if (arg == "--motion-gain")
            {
                options.motionGain = static_cast<float>(atof(next("--motion-gain").c_str()));
            }
            else if (arg == "--body-sway")
            {
                options.bodySway = atoi(next("--body-sway").c_str()) != 0;
            }
            else if (arg == "--physics")
            {
                const std::string text = next("--physics");
                options.physicsEnabled = !(text == "off" || text == "0" || text == "false");
            }
            else if (arg == "--log-params")
            {
                options.paramLogInterval = atoi(next("--log-params").c_str());
            }
            else if (arg == "--filter-motion")
            {
                options.filterMotion = atoi(next("--filter-motion").c_str()) != 0;
            }
            else if (arg == "--track-blend")
            {
                options.trackBlend = static_cast<float>(atof(next("--track-blend").c_str()));
            }
            else if (arg == "--hair-sway")
            {
                options.hairSway = static_cast<float>(atof(next("--hair-sway").c_str()));
            }
            else if (arg == "--no-click-motion")
            {
                options.clickMotion = false;
            }
            else if (arg == "--no-click-shy" || arg == "--no-click-chest")
            {
                options.clickChest = false;
            }
            else if (arg == "--chest-shy")
            {
                options.chestShy = true;
            }
            else if (arg == "--idle-stare")
            {
                options.idleStare = true;
            }
            else if (arg == "--no-idle-stare")
            {
                options.idleStare = false;
            }
            else if (arg == "--stare-interval-min")
            {
                options.stareIntervalMin =
                    static_cast<float>(atof(next("--stare-interval-min").c_str()));
            }
            else if (arg == "--stare-interval-max")
            {
                options.stareIntervalMax =
                    static_cast<float>(atof(next("--stare-interval-max").c_str()));
            }
            else if (arg == "--debug-click")
            {
                options.debugClickFrame = atoi(next("--debug-click").c_str());
            }
            else if (arg == "--debug-stare")
            {
                options.debugStareFrame = atoi(next("--debug-stare").c_str());
            }
            else if (arg == "--debug-shy")
            {
                options.debugShyFrame = atoi(next("--debug-shy").c_str());
            }
            else if (arg == "--debug-chest")
            {
                options.debugChestFrame = atoi(next("--debug-chest").c_str());
            }
            else if (arg == "--hit-test")
            {
                options.hitTestPixel = next("--hit-test");
            }
            else if (arg == "--motion")
            {
                options.motionSpec = next("--motion");
            }
            else if (arg == "--track-seq")
            {
                // "frame:x,y;frame:x,y;..." — scripted tracking targets.
                const std::string text = next("--track-seq");
                size_t pos = 0;
                while (pos < text.size())
                {
                    const size_t semi = text.find(';', pos);
                    const std::string item = text.substr(
                        pos, semi == std::string::npos ? std::string::npos : semi - pos);
                    pos = (semi == std::string::npos) ? text.size() : semi + 1;
                    const size_t colon = item.find(':');
                    const size_t comma = item.find(',');
                    if (colon == std::string::npos || comma == std::string::npos ||
                        colon >= comma)
                    {
                        LogMsg("[args] bad --track-seq item '%s'", item.c_str());
                        continue;
                    }
                    Options::TrackKey key;
                    key.frame = atoi(item.substr(0, colon).c_str());
                    key.x = static_cast<float>(atof(item.substr(colon + 1,
                                                               comma - colon - 1).c_str()));
                    key.y = static_cast<float>(atof(item.substr(comma + 1).c_str()));
                    options.trackSeq.push_back(key);
                }
                std::sort(options.trackSeq.begin(), options.trackSeq.end(),
                          [](const Options::TrackKey& a, const Options::TrackKey& b)
                          { return a.frame < b.frame; });
            }
            else if (arg == "--snapshot-seq")
            {
                // "frame:path;frame:path;..." — snapshots at exact frames.
                const std::string text = next("--snapshot-seq");
                size_t pos = 0;
                while (pos < text.size())
                {
                    const size_t semi = text.find(';', pos);
                    const std::string item = text.substr(
                        pos, semi == std::string::npos ? std::string::npos : semi - pos);
                    pos = (semi == std::string::npos) ? text.size() : semi + 1;
                    const size_t colon = item.find(':');
                    if (colon == std::string::npos || colon + 1 >= item.size())
                    {
                        LogMsg("[args] bad --snapshot-seq item '%s'", item.c_str());
                        continue;
                    }
                    Options::SeqShot shot;
                    shot.frame = atoi(item.substr(0, colon).c_str());
                    shot.path = item.substr(colon + 1);
                    options.snapshotSeq.push_back(shot);
                }
                std::sort(options.snapshotSeq.begin(), options.snapshotSeq.end(),
                          [](const Options::SeqShot& a, const Options::SeqShot& b)
                          { return a.frame < b.frame; });
            }
            else if (arg == "--snapshot")
            {
                options.snapshotPath = next("--snapshot");
            }
            else if (arg == "--dump-params")
            {
                options.dumpParamsPath = next("--dump-params");
            }
            else if (arg == "--dump-mesh")
            {
                options.dumpMeshPath = next("--dump-mesh");
            }
            else if (arg == "--aniso")
            {
                options.textureAniso = atoi(next("--aniso").c_str());
            }
            else if (arg == "--lod-bias")
            {
                options.textureLodBias = static_cast<float>(atof(next("--lod-bias").c_str()));
            }
            else if (arg == "--no-mipmap")
            {
                options.textureMipmap = false;
            }
            else if (arg == "--ssaa")
            {
                options.ssaaFactor = static_cast<float>(atof(next("--ssaa").c_str()));
            }
            else if (arg == "--track")
            {
                const std::string text = next("--track");
                const size_t comma = text.find(',');
                if (comma != std::string::npos)
                {
                    options.trackOverrideX = static_cast<float>(atof(text.substr(0, comma).c_str()));
                    options.trackOverrideY = static_cast<float>(atof(text.c_str() + comma + 1));
                    options.hasTrackOverride = true;
                }
                else
                {
                    LogMsg("[args] invalid --track, expected X,Y");
                }
            }
            else if (arg == "--frames")
            {
                options.snapshotFrame = atoi(next("--frames").c_str());
            }
            else if (arg == "--exit")
            {
                options.exitAfterSnapshot = true;
            }
            else if (arg == "--cx")
            {
                options.centerX = static_cast<float>(atof(next("--cx").c_str()));
                options.centerXSet = true;
            }
            else if (arg == "--cy")
            {
                options.centerY = static_cast<float>(atof(next("--cy").c_str()));
            }
            else if (arg == "--hh")
            {
                options.halfHeight = static_cast<float>(atof(next("--hh").c_str()));
                options.halfHeightSet = true;
            }
            else if (arg == "--hw")
            {
                options.halfWidth = static_cast<float>(atof(next("--hw").c_str()));
                options.halfWidthSet = true;
            }
            else if (arg == "--width")
            {
                options.width = atoi(next("--width").c_str());
            }
            else if (arg == "--height")
            {
                options.height = atoi(next("--height").c_str());
            }
            else if (arg == "--bg")
            {
                if (!ParseColor(next("--bg"), options.background))
                {
                    LogMsg("[args] invalid --bg value, expected RRGGBB");
                }
            }
            else if (arg == "--topmost")
            {
                options.topmost = true;
                options.topmostSet = true;
            }
            else if (arg == "--nofocus")
            {
                options.noFocus = true;
            }
            else if (arg == "--size")
            {
                const std::string text = next("--size");
                const size_t cross = text.find_first_of("xX*");
                if (cross == std::string::npos)
                {
                    // Single value = width with the usual 4:3 aspect
                    // (--size 640 -> 640x480, --size 900 -> 900x675).
                    options.width = atoi(text.c_str());
                    options.height = options.width * 3 / 4;
                }
                else
                {
                    options.width = atoi(text.substr(0, cross).c_str());
                    options.height = atoi(text.c_str() + cross + 1);
                }
            }
            if (!arg.empty() && arg[0] != '-')
            {
                if (positional == 0)
                {
                    options.x = atoi(arg.c_str());
                    options.xSet = true;
                }
                else if (positional == 1)
                {
                    options.y = atoi(arg.c_str());
                    options.ySet = true;
                }
                ++positional;
            }
            else if (arg == "--panels")
            {
                options.panels = true;
            }
            else if (arg == "--no-panels")
            {
                options.panels = false;
            }
            else if (arg == "--no-tray")
            {
                options.tray = false;
            }
            else if (arg == "--tray")
            {
                options.tray = true;
            }
            else if (arg == "--install-autostart")
            {
                options.installAutostart = true;
            }
            else if (arg == "--remove-autostart")
            {
                options.removeAutostart = true;
            }
            else if (arg == "--ui-scale")
            {
                options.uiScale = static_cast<float>(atof(next("--ui-scale").c_str()));
                if (options.uiScale < 0.5f || options.uiScale > 3.0f)
                {
                    LogMsg("[args] --ui-scale %.2f out of range; using auto", options.uiScale);
                    options.uiScale = 0.0f;
                }
            }
        }

        if (options.width < 16 || options.width > 8192)
        {
            LogMsg("[args] invalid --width %d, using 640", options.width);
            options.width = 640;
        }
        if (options.height < 16 || options.height > 8192)
        {
            LogMsg("[args] invalid --height %d, using 480", options.height);
            options.height = 480;
        }

        // Panels need 2 x 160 px, so the middle Live2D viewport is whatever is
        // left. Below 352 px total width the panels would eat the character;
        // fall back to the Live2D-only layout.
        if (options.panels)
        {
            const int modelWidth = options.width - 2 * 160;
            if (modelWidth < 64)
            {
                LogMsg("[args] window %dx%d too narrow for panels; --no-panels",
                       options.width, options.height);
                options.panels = false;
            }
        }
        options.modelWidth = options.panels ? options.width - 2 * 160 : options.width;
        options.modelHeight = options.height;

        ResolveFraming(options);
        g_baseHalfHeight = options.halfHeight;
        options.ssaaFactor = std::max(1.0f, std::min(2.0f, options.ssaaFactor));

        const bool snapshotMode =
            std::find(args.begin(), args.end(), "--snapshot") != args.end() ||
            !options.snapshotSeq.empty();
        if (!snapshotMode && LoadModelConfig(options))
        {
            // The saved camera overrides the default framing; keep pixels
            // square by deriving the horizontal extent from the aspect ratio.
            const float aspect = static_cast<float>(options.modelWidth) /
                                 static_cast<float>(options.modelHeight);
            options.halfWidth = options.halfHeight * aspect;
            // --topmost is explicit and wins; otherwise restore the saved flag.
            if (!options.topmostSet)
            {
                options.topmost = options.configTopmost;
                LogMsg("[config] restored topmost=%d from model.cfg",
                       options.topmost ? 1 : 0);
            }
            else
            {
                LogMsg("[config] topmost=%d from --topmost (model.cfg topmost=%d ignored)",
                       options.topmost ? 1 : 0, options.configTopmost ? 1 : 0);
            }
        }
        return options;
    }

    // Camera state used for interactive dragging. Holding the left mouse
    // button on the window moves the model with the cursor.
    struct ViewState
    {
        float centerX = 0.0f;
        float centerY = 0.0f;
        float halfHeight = 0.42f;
        float halfWidth = 0.56f;
        int width = 640;
        int height = 480;
        bool dragging = false;
        POINT dragStart = {0, 0};
        float dragCenterX = 0.0f;
        float dragCenterY = 0.0f;
    };

    ViewState g_view;
    bool g_editMode = false;       // model edit lock is OFF by default
    bool g_persistModel = true;
    // Current topmost state of the window, mirrored into model.cfg.
    bool g_topmost = false;
    HDC g_fontDc = nullptr;
    GLuint g_fontListBase = 0;
    bool g_fontReady = false;

    // Window-level state for the panel layout. The model is rendered into the
    // middle rect; client coordinates are converted to model-local ones before
    // they reach the camera / hit-testing code.
    int  g_modelX = 0;
    int  g_modelY = 0;
    int  g_windowWidth = 640;
    int  g_windowHeight = 480;
    int  g_logicalWindowWidth = 640;
    int  g_logicalWindowHeight = 480;
    float g_uiScale = 1.0f;
    bool g_panelsEnabled = false;
    PanelRenderer* g_panelRenderer = nullptr;
    bool g_autostartChecked = false;
    bool g_panelsDirty = true;

    POINT ClientToModelLocal(int clientX, int clientY)
    {
        // Client coordinates are physical pixels; the camera works in the
        // 320x480 logical layout.
        POINT local = {
            static_cast<LONG>(lroundf((clientX - g_modelX) / g_uiScale)),
            static_cast<LONG>(lroundf((clientY - g_modelY) / g_uiScale))
        };
        return local;
    }

    bool IsInsideModel(int clientX, int clientY)
    {
        const int modelPixelWidth =
            static_cast<int>(lroundf(g_view.width * g_uiScale));
        const int modelPixelHeight =
            static_cast<int>(lroundf(g_view.height * g_uiScale));
        return clientX >= g_modelX && clientX < g_modelX + modelPixelWidth &&
               clientY >= g_modelY && clientY < g_modelY + modelPixelHeight;
    }

    bool LoadModelConfig(Options& options)
    {
        const std::wstring path = g_exeDirWide + L"model.cfg";
        FILE* file = _wfopen(path.c_str(), L"r");
        if (!file)
        {
            return false;
        }
        float centerX = options.centerX;
        float centerY = options.centerY;
        float halfHeight = options.halfHeight;
        int topmostValue = 0;
        // The 4th field is optional so files written by older builds
        // ("cx,cy,hh") still load and simply mean "not topmost".
        const int count = fscanf(file, "%f,%f,%f,%d", &centerX, &centerY, &halfHeight,
                                 &topmostValue);
        fclose(file);
        if (count < 3 || halfHeight <= 0.0f)
        {
            LogMsg("[config] model.cfg ignored (invalid content)");
            return false;
        }
        const float minHalfHeight = g_baseHalfHeight / 3.0f;   // 300%
        const float maxHalfHeight = g_baseHalfHeight / 0.5f;   // 50%
        halfHeight = std::max(minHalfHeight, std::min(maxHalfHeight, halfHeight));
        options.centerX = centerX;
        options.centerY = centerY;
        options.halfHeight = halfHeight;
        options.configTopmostValid = true;
        options.configTopmost = (count >= 4) && topmostValue != 0;
        LogMsg("[config] model.cfg cx=%.4f cy=%.4f hh=%.4f (%d%%) topmost=%d (fields=%d)",
               centerX, centerY, halfHeight,
               static_cast<int>(lroundf(g_baseHalfHeight / halfHeight * 100.0f)),
               options.configTopmost ? 1 : 0, count);
        return true;
    }

    void SaveModelConfig()
    {
        if (!g_persistModel)
        {
            return;
        }
        const std::wstring path = g_exeDirWide + L"model.cfg";
        FILE* file = _wfopen(path.c_str(), L"w");
        if (!file)
        {
            LogMsg("[config] cannot write model.cfg");
            return;
        }
        fprintf(file, "%.6f,%.6f,%.6f,%d\n",
                g_view.centerX, g_view.centerY, g_view.halfHeight,
                g_topmost ? 1 : 0);
        fclose(file);
    }

    int ZoomPercent()
    {
        if (g_view.halfHeight <= 0.0001f)
        {
            return 100;
        }
        return static_cast<int>(lroundf(g_baseHalfHeight / g_view.halfHeight * 100.0f));
    }

    void ZoomAtCursor(int clientX, int clientY, float factor)
    {
        const float minHalfHeight = g_baseHalfHeight / 3.0f;   // 300%
        const float maxHalfHeight = g_baseHalfHeight / 0.5f;   // 50%
        float newHalfHeight = g_view.halfHeight / factor;
        newHalfHeight = std::max(minHalfHeight, std::min(maxHalfHeight, newHalfHeight));
        if (fabsf(newHalfHeight - g_view.halfHeight) < 1.0e-6f)
        {
            return;
        }

        const float width = static_cast<float>(g_view.width);
        const float height = static_cast<float>(g_view.height);
        const float aspect = width / height;
        const float oldHalfWidth = g_view.halfHeight * aspect;
        const float newHalfWidth = newHalfHeight * aspect;
        const float dx = static_cast<float>(clientX) - width * 0.5f;
        const float dy = static_cast<float>(clientY) - height * 0.5f;

        // Model point under the cursor must stay under the cursor.
        const float modelX = g_view.centerX + dx * (2.0f * oldHalfWidth / width);
        const float modelY = g_view.centerY - dy * (2.0f * g_view.halfHeight / height);
        g_view.centerX = modelX - dx * (2.0f * newHalfWidth / width);
        g_view.centerY = modelY + dy * (2.0f * newHalfHeight / height);
        g_view.halfHeight = newHalfHeight;
        g_view.halfWidth = newHalfWidth;
        LogMsg("[ui] zoom %d%% hh=%.4f center=(%.3f,%.3f)",
               ZoomPercent(), g_view.halfHeight, g_view.centerX, g_view.centerY);
        SaveModelConfig();
    }

    void DrawRoundedRect(float x, float y, float width, float height, float radius)
    {
        const int cornerSteps = 6;
        glBegin(GL_TRIANGLE_FAN);
        glVertex2f(x + width * 0.5f, y + height * 0.5f);

        // bottom-right -> top-right -> top-left -> bottom-left
        const float centersX[4] = {x + width - radius, x + width - radius,
                                   x + radius, x + radius};
        const float centersY[4] = {y + radius, y + height - radius,
                                   y + height - radius, y + radius};
        const float startAngles[4] = {-90.0f, 0.0f, 90.0f, 180.0f};
        for (int corner = 0; corner < 4; ++corner)
        {
            for (int step = 0; step <= cornerSteps; ++step)
            {
                const float angle = (startAngles[corner] +
                                     90.0f * static_cast<float>(step) / cornerSteps) *
                                    3.14159265358979f / 180.0f;
                glVertex2f(centersX[corner] + cosf(angle) * radius,
                           centersY[corner] + sinf(angle) * radius);
            }
        }
        glEnd();
    }

    void DrawText2D(const char* text, int x, int y)
    {
        if (!g_fontReady && g_fontDc)
        {
            HFONT font = CreateFontW(-static_cast<int>(lroundf(13.0f * g_uiScale)), 0, 0, 0,
                                     FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HGDIOBJ oldFont = SelectObject(g_fontDc, font);
            g_fontListBase = glGenLists(96);
            wglUseFontBitmapsW(g_fontDc, 32, 96, g_fontListBase);
            SelectObject(g_fontDc, oldFont);
            DeleteObject(font);
            g_fontReady = true;
        }
        if (g_fontListBase != 0)
        {
            glRasterPos2i(x, y);
            glListBase(g_fontListBase - 32);
            glCallLists(static_cast<GLsizei>(strlen(text)), GL_UNSIGNED_BYTE, text);
        }
    }

    void DrawEditButton(int width, int height, bool editMode)
    {
        const float size = 28.0f;
        const float margin = 8.0f;
        const float x = static_cast<float>(width) - margin - size;
        // GL's origin is bottom-left, so a small y puts the button at the top
        // of the window in client coordinates (which is what IsPointInButton
        // tests: y in [margin, margin + size]).
        const float y = static_cast<float>(height) - margin - size;

        if (editMode)
        {
            glColor4f(0.16f, 0.68f, 0.36f, 0.96f);
        }
        else
        {
            glColor4f(0.42f, 0.45f, 0.49f, 0.92f);
        }
        DrawRoundedRect(x, y, size, size, 7.0f);

        glColor4f(1.0f, 1.0f, 1.0f, 0.95f);
        if (editMode)
        {
            // Pencil icon.
            glLineWidth(3.0f);
            glBegin(GL_LINES);
            glVertex2f(x + 8.0f, y + 8.0f);
            glVertex2f(x + 20.0f, y + 20.0f);
            glEnd();
            glBegin(GL_TRIANGLES);
            glVertex2f(x + 6.0f, y + 6.0f);
            glVertex2f(x + 10.0f, y + 7.5f);
            glVertex2f(x + 7.5f, y + 10.0f);
            glEnd();

            // Zoom percentage pill left of the button.
            char label[16];
            snprintf(label, sizeof(label), "%d%%", ZoomPercent());
            const float pillWidth = 54.0f;
            const float pillHeight = 22.0f;
            const float pillX = x - 6.0f - pillWidth;
            const float pillY = y + 3.0f;
            glColor4f(0.08f, 0.42f, 0.22f, 0.88f);
            DrawRoundedRect(pillX, pillY, pillWidth, pillHeight, 6.0f);
            glColor4f(1.0f, 1.0f, 1.0f, 0.97f);
            DrawText2D(label, static_cast<int>(pillX + 9.0f),
                       static_cast<int>(pillY + 6.0f));
        }
        else
        {
            // Lock icon: shackle + body.
            glLineWidth(2.5f);
            glBegin(GL_LINE_STRIP);
            for (int i = 0; i <= 12; ++i)
            {
                const float angle = 3.14159265f * static_cast<float>(i) / 12.0f;
                glVertex2f(x + 14.0f + cosf(angle) * 5.0f,
                           y + 16.0f + sinf(angle) * 5.0f);
            }
            glEnd();
            DrawRoundedRect(x + 9.0f, y + 7.0f, 10.0f, 9.0f, 2.0f);
        }
    }

    void DrawUi(int viewportX, int viewportY, int viewportWidth, int viewportHeight,
                int logicalWidth, int logicalHeight, bool editMode)
    {
        if (!glUseProgram)
        {
            return;
        }
        glUseProgram(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glViewport(viewportX, viewportY, viewportWidth, viewportHeight);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(logicalWidth), 0.0,
                static_cast<double>(logicalHeight), -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        DrawEditButton(logicalWidth, logicalHeight, editMode);
        glLineWidth(1.0f);
    }

    // ---------------------------------------------------------------------
    // Supersampled model render target
    // ---------------------------------------------------------------------
    // The official renderer always samples the atlas with
    // GL_LINEAR_MIPMAP_LINEAR (CubismShader_OpenGLES2::SetupTexture), which on
    // an 8192x8192 atlas displayed at ~1/7 scale picks a fairly soft mip and
    // leaves stair-stepped silhouettes. Rendering the whole model pass into a
    // larger offscreen target and resolving it down with a single bilinear
    // step (a 2x2 box at exactly 2x) restores texture detail and anti-aliases
    // every mesh/clipping edge, without touching the SDK.
    struct SsaaTarget
    {
        GLuint fbo = 0;
        GLuint color = 0;
        GLuint depthStencil = 0;
        int width = 0;
        int height = 0;
    };

    SsaaTarget g_ssaa;

    void ReleaseSsaaTarget()
    {
        if (g_ssaa.depthStencil != 0)
        {
            glDeleteRenderbuffers(1, &g_ssaa.depthStencil);
        }
        if (g_ssaa.color != 0)
        {
            glDeleteTextures(1, &g_ssaa.color);
        }
        if (g_ssaa.fbo != 0)
        {
            glDeleteFramebuffers(1, &g_ssaa.fbo);
        }
        g_ssaa = SsaaTarget();
    }

    bool EnsureSsaaTarget(int width, int height)
    {
        if (g_ssaa.fbo != 0 && g_ssaa.width == width && g_ssaa.height == height)
        {
            return true;
        }
        ReleaseSsaaTarget();

        glGenTextures(1, &g_ssaa.color);
        glBindTexture(GL_TEXTURE_2D, g_ssaa.color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenRenderbuffers(1, &g_ssaa.depthStencil);
        glBindRenderbuffer(GL_RENDERBUFFER, g_ssaa.depthStencil);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glGenFramebuffers(1, &g_ssaa.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_ssaa.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               g_ssaa.color, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, g_ssaa.depthStencil);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            LogMsg("[ssaa] framebuffer incomplete (0x%X) at %dx%d; falling back to direct rendering",
                   static_cast<unsigned int>(status), width, height);
            ReleaseSsaaTarget();
            return false;
        }

        g_ssaa.width = width;
        g_ssaa.height = height;
        LogMsg("[ssaa] render target %dx%d", width, height);
        return true;
    }

    // Resolves the supersampled colour buffer onto the given window rectangle.
    void ResolveSsaaToWindow(int x, int y, int width, int height)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(x, y, width, height);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glUseProgram(0);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, 1.0, 0.0, 1.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_ssaa.color);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, 1.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 1.0f);
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }

    // Draws the GDI+-rendered panel surface over the whole window. The DIB is
    // top-down, hence the flipped T coordinates.
    void DrawPanelTexture(GLuint texture, int width, int height)
    {
        if (texture == 0)
        {
            return;
        }
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glUseProgram(0);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(width), 0.0, static_cast<double>(height),
                -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(static_cast<float>(width), 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(static_cast<float>(width),
                                              static_cast<float>(height));
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, static_cast<float>(height));
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }

    bool IsPointInButton(const POINT& client, int width, int height)    {
        const int size = 28;
        const int margin = 8;
        return client.x >= width - margin - size && client.x < width - margin &&
               client.y >= margin && client.y < margin + size;
    }

    // Click-to-head-pat state (the model itself lives in wWinMain).
    NeneModel* g_model = nullptr;
    bool  g_clickMotionEnabled = true;
    bool  g_clickChestEnabled = true;
    bool  g_chestShy = false;
    bool  g_clickPending = false;
    POINT g_clickStart = {0, 0};

    // Converts a client pixel to model-matrix space (model height 2, centered)
    // using the current camera, so the hit test follows zoom/pan.
    void ClientToModel(int px, int py, float* modelX, float* modelY)
    {
        const float width = static_cast<float>(g_view.width);
        const float height = static_cast<float>(g_view.height);
        const float ndcX = (2.0f * static_cast<float>(px) / width) - 1.0f;
        const float ndcY = 1.0f - (2.0f * static_cast<float>(py) / height);
        *modelX = g_view.centerX + ndcX * g_view.halfWidth;
        *modelY = g_view.centerY + ndcY * g_view.halfHeight;
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_NCHITTEST:
            // Fixed window: never move or resize it.
            return HTCLIENT;
        case WM_LBUTTONDOWN:
        {
            POINT clientPoint = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // Bottom-right "开机启动" checkbox lives in the right panel.
            if (g_panelsEnabled &&
                PanelRenderer::HitTestAutostart(
                    static_cast<int>(lroundf(clientPoint.x / g_uiScale)),
                    static_cast<int>(lroundf(clientPoint.y / g_uiScale)),
                    g_logicalWindowWidth, g_logicalWindowHeight))
            {
                const bool newState = !g_autostartChecked;
                wchar_t modulePath[MAX_PATH] = {};
                GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
                if (SetAutostart(newState, BuildAutostartCommand(modulePath)))
                {
                    g_autostartChecked = newState;
                }
                g_panelsDirty = true;
                g_view.dragging = false;
                g_clickPending = false;
                return 0;
            }
            if (!IsInsideModel(clientPoint.x, clientPoint.y))
            {
                return 0;
            }
            const POINT local = ClientToModelLocal(clientPoint.x, clientPoint.y);
            if (IsPointInButton(local, g_view.width, g_view.height))
            {
                g_editMode = !g_editMode;
                g_view.dragging = false;
                g_clickPending = false;
                LogMsg("[ui] edit mode = %d", g_editMode ? 1 : 0);
                return 0;
            }
            // Remember the press position; on release a movement < 6 px is a
            // click (head pat), anything larger stays a drag/pan.
            g_clickStart = local;
            g_clickPending = true;
            if (!g_editMode)
            {
                return 0;
            }

            SetCapture(hwnd);
            g_view.dragging = true;
            g_view.dragStart = local;
            g_view.dragCenterX = g_view.centerX;
            g_view.dragCenterY = g_view.centerY;
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if (g_view.dragging && g_editMode)
            {
                const int x = static_cast<int>(
                    lroundf((GET_X_LPARAM(lParam) - g_modelX) / g_uiScale));
                const int y = static_cast<int>(
                    lroundf((GET_Y_LPARAM(lParam) - g_modelY) / g_uiScale));
                const float scaleX = 2.0f * g_view.halfWidth /
                                     static_cast<float>(g_view.width);
                const float scaleY = 2.0f * g_view.halfHeight /
                                     static_cast<float>(g_view.height);
                g_view.centerX = g_view.dragCenterX -
                                 static_cast<float>(x - g_view.dragStart.x) * scaleX;
                g_view.centerY = g_view.dragCenterY +
                                 static_cast<float>(y - g_view.dragStart.y) * scaleY;
                g_view.centerX = std::max(-1.8f, std::min(1.8f, g_view.centerX));
                g_view.centerY = std::max(-1.2f, std::min(1.8f, g_view.centerY));
            }
            return 0;
        }
        case WM_LBUTTONUP:
        {
            const POINT clientPoint = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const POINT upPoint = ClientToModelLocal(clientPoint.x, clientPoint.y);
            const bool wasClick =
                g_clickPending &&
                std::abs(upPoint.x - g_clickStart.x) < 6 &&
                std::abs(upPoint.y - g_clickStart.y) < 6;
            g_clickPending = false;
            if (g_view.dragging)
            {
                g_view.dragging = false;
                ReleaseCapture();
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                SaveModelConfig();
            }
            if (wasClick && g_model != nullptr &&
                IsInsideModel(clientPoint.x, clientPoint.y))
            {
                float modelX = 0.0f;
                float modelY = 0.0f;
                ClientToModel(upPoint.x, upPoint.y, &modelX, &modelY);
                const bool hitHead = g_model->HitTestHead(modelX, modelY);
                const bool hitChest = !hitHead && g_model->HitTestChest(modelX, modelY);
                LogMsg("[click] pixel(%d,%d) -> model(%.3f,%.3f) head=%d chest=%d",
                       upPoint.x, upPoint.y, modelX, modelY, hitHead ? 1 : 0,
                       hitChest ? 1 : 0);
                if (hitHead && g_clickMotionEnabled)
                {
                    g_model->TriggerHeadPat();
                }
                else if (hitChest && g_clickChestEnabled)
                {
                    if (g_chestShy)
                    {
                        g_model->TriggerShy();
                    }
                    else
                    {
                        g_model->TriggerChestMotion();
                    }
                }
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
        {
            g_view.dragging = false;
            g_clickPending = false;
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            if (!g_editMode)
            {
                return 0;
            }
            POINT cursor = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &cursor);
            if (!IsInsideModel(cursor.x, cursor.y))
            {
                return 0;
            }
            const POINT local = ClientToModelLocal(cursor.x, cursor.y);
            const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const float factor = powf(1.15f, static_cast<float>(delta) / 120.0f);
            ZoomAtCursor(local.x, local.y, factor);
            return 0;
        }
        case WM_SETCURSOR:
        {
            if (LOWORD(lParam) == HTCLIENT)
            {
                POINT cursor;
                GetCursorPos(&cursor);
                ScreenToClient(hwnd, &cursor);
                const POINT local = ClientToModelLocal(cursor.x, cursor.y);
                if (IsPointInButton(local, g_view.width, g_view.height))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                }
                else if (g_view.dragging)
                {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                }
                else
                {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
                return TRUE;
            }
            break;
        }
        case WM_SIZE:
        {
            const int width = LOWORD(lParam);
            const int height = HIWORD(lParam);
            if (width > 0 && height > 0)
            {
                g_windowWidth = width;
                g_windowHeight = height;
                g_modelX = static_cast<int>(lroundf((g_panelsEnabled ? 160.0f : 0.0f) *
                                                    g_uiScale));
                g_view.width = static_cast<int>(lroundf(width / g_uiScale)) -
                               (g_panelsEnabled ? 320 : 0);
                g_view.height = static_cast<int>(lroundf(height / g_uiScale));
                g_logicalWindowWidth = g_view.width + (g_panelsEnabled ? 320 : 0);
                g_logicalWindowHeight = g_view.height;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                SaveModelConfig();
                PostQuitMessage(0);
            }
            return 0;
        case WM_CLOSE:
            SaveModelConfig();
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            SaveModelConfig();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    // When no explicit window position is given, put the window on the mini
    // screen: a non-primary monitor whose *logical* geometry matches the
    // 640x480 layout (physical size / DPI scale). Otherwise centre it on the
    // primary monitor, which also makes local testing easy.
    struct MonitorMatch
    {
        int logicalWidth = 0;
        int logicalHeight = 0;
        float forcedScale = 0.0f;
        int x = 0;
        int y = 0;
        float scale = 1.0f;
        bool matched = false;
        int smallX = 0;
        int smallY = 0;
        float smallScale = 1.0f;
        bool smallMatched = false;
    };

    float ScaleForMonitor(HMONITOR monitor, float forcedScale)
    {
        if (forcedScale > 0.0f)
        {
            return forcedScale;
        }
        UINT dpiX = 96;
        UINT dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) &&
            dpiX > 0)
        {
            return static_cast<float>(dpiX) / 96.0f;
        }
        return 1.0f;
    }

    BOOL CALLBACK FindMatchingMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
    {
        MonitorMatch* match = reinterpret_cast<MonitorMatch*>(data);
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
        {
            return TRUE;
        }
        const bool primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        const int width = info.rcMonitor.right - info.rcMonitor.left;
        const int height = info.rcMonitor.bottom - info.rcMonitor.top;
        const float scale = ScaleForMonitor(monitor, match->forcedScale);
        const int logicalWidth = static_cast<int>(lroundf(width / scale));
        const int logicalHeight = static_cast<int>(lroundf(height / scale));
        LogMsg("[win] monitor %s: (%ld,%ld)-(%ld,%ld) %dx%d phys, %dx%d logical, "
               "scale=%.2f primary=%d",
               Utf8FromWide(info.szDevice).c_str(),
               info.rcMonitor.left, info.rcMonitor.top,
               info.rcMonitor.right, info.rcMonitor.bottom,
               width, height, logicalWidth, logicalHeight, scale, primary ? 1 : 0);
        if (primary)
        {
            return TRUE;
        }
        // Preferred: a secondary screen whose logical geometry is the window.
        if (!match->matched && logicalWidth == match->logicalWidth &&
            logicalHeight == match->logicalHeight)
        {
            match->x = info.rcMonitor.left;
            match->y = info.rcMonitor.top;
            match->scale = scale;
            match->matched = true;
            return FALSE;
        }
        // The USB mini panel is a small secondary screen. Windows may report a
        // slightly different logical size, so any small secondary display is a
        // good candidate.
        if (!match->smallMatched && logicalWidth <= 1024 && logicalHeight <= 768)
        {
            match->smallX = info.rcMonitor.left;
            match->smallY = info.rcMonitor.top;
            match->smallScale = scale;
            match->smallMatched = true;
        }
        return TRUE;
    }

    void ResolveWindowPlacement(Options& options)
    {
        if (options.xSet || options.ySet)
        {
            // Explicit position: derive the scale from the monitor under it.
            const POINT point = {options.x + 1, options.y + 1};
            const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
            options.uiScale = ScaleForMonitor(monitor, options.uiScale);
            return;
        }

        MonitorMatch match;
        match.logicalWidth = options.width;
        match.logicalHeight = options.height;
        match.forcedScale = options.uiScale;
        EnumDisplayMonitors(nullptr, nullptr, FindMatchingMonitor,
                            reinterpret_cast<LPARAM>(&match));
        if (match.matched)
        {
            options.x = match.x;
            options.y = match.y;
            options.uiScale = match.scale;
            options.placedOnMiniScreen = true;
            LogMsg("[win] auto-placed on matching monitor at (%d,%d) scale=%.2f",
                   match.x, match.y, match.scale);
            return;
        }
        if (match.smallMatched)
        {
            options.x = match.smallX;
            options.y = match.smallY;
            options.uiScale = match.smallScale;
            options.placedOnMiniScreen = true;
            LogMsg("[win] auto-placed on small secondary monitor at (%d,%d) scale=%.2f",
                   match.smallX, match.smallY, match.smallScale);
            return;
        }

        const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(primary, &info);
        const float scale = ScaleForMonitor(primary, options.uiScale);
        const int pixelWidth = static_cast<int>(lroundf(options.width * scale));
        const int pixelHeight = static_cast<int>(lroundf(options.height * scale));
        const int monitorWidth = info.rcMonitor.right - info.rcMonitor.left;
        const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
        options.x = info.rcMonitor.left + std::max(0, (monitorWidth - pixelWidth) / 2);
        options.y = info.rcMonitor.top + std::max(0, (monitorHeight - pixelHeight) / 2);
        options.uiScale = scale;
        LogMsg("[win] no matching monitor; centred on primary at (%d,%d) scale=%.2f",
               options.x, options.y, scale);
    }

    void ApplyTopmost(HWND hwnd, bool topmost)
    {
        SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_topmost = topmost;
        LogMsg("[win] topmost=%d exstyle=0x%08lX",
               topmost ? 1 : 0,
               static_cast<unsigned long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
    }

    bool IsWindowTopmost(HWND hwnd)
    {
        return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    }

    bool CreateGlWindow(HINSTANCE instance, const Options& options, HWND& outWindow,
                        HDC& outDc, HGLRC& outGlrc)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(options.background);
        // Resource id 1 is the application icon embedded from res/app.rc.
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"NeneCubismWindowClass";
        if (!RegisterClassExW(&wc))
        {
            LogMsg("[win] RegisterClassEx failed: %lu", GetLastError());
            return false;
        }

        DWORD extendedStyle = 0;
        if (options.topmost)
        {
            extendedStyle |= WS_EX_TOPMOST;
        }
        if (options.tray)
        {
            // Tool window: no taskbar button and no Alt+Tab entry.
            extendedStyle |= WS_EX_TOOLWINDOW;
        }
        else
        {
            extendedStyle |= WS_EX_APPWINDOW;
        }
        HWND hwnd = CreateWindowExW(extendedStyle,
                                    wc.lpszClassName, L"Nene Cubism",
                                    WS_POPUP, options.x, options.y,
                                    options.windowPixelWidth, options.windowPixelHeight,
                                    nullptr, nullptr, instance, nullptr);
        if (!hwnd)
        {
            LogMsg("[win] CreateWindowEx failed: %lu", GetLastError());
            return false;
        }

        HDC dc = GetDC(hwnd);

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cAlphaBits = 8;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;

        const int pixelFormat = ChoosePixelFormat(dc, &pfd);
        if (pixelFormat == 0)
        {
            LogMsg("[win] ChoosePixelFormat failed: %lu", GetLastError());
            return false;
        }
        if (!SetPixelFormat(dc, pixelFormat, &pfd))
        {
            LogMsg("[win] SetPixelFormat failed: %lu", GetLastError());
            return false;
        }

        HGLRC glrc = wglCreateContext(dc);
        if (!glrc)
        {
            LogMsg("[win] wglCreateContext failed: %lu", GetLastError());
            return false;
        }
        if (!wglMakeCurrent(dc, glrc))
        {
            LogMsg("[win] wglMakeCurrent failed: %lu", GetLastError());
            return false;
        }

        outWindow = hwnd;
        outDc = dc;
        outGlrc = glrc;
        return true;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    // Resolve the executable directory first (logs, shaders and files use it).
    {
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        std::wstring full(modulePath);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            g_exeDirWide = full.substr(0, slash + 1);
        }
        g_exeDir = Utf8FromWide(g_exeDirWide);
    }

    // Per-monitor DPI awareness (V2 when available): GetMonitorInfo and
    // GetDpiForMonitor then report real physical pixels and DPI, which is what
    // the 640x480 -> 960x720 scaling below is based on.
    {
        typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(HANDLE);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        SetProcessDpiAwarenessContextFn setContext = user32
            ? reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                  GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
            : nullptr;
        bool dpiReady = false;
        if (setContext)
        {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            dpiReady = setContext(reinterpret_cast<HANDLE>(-4)) != FALSE;
        }
        if (!dpiReady)
        {
            dpiReady = SUCCEEDED(SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE));
        }
        if (!dpiReady)
        {
            SetProcessDPIAware();
        }
    }

    Options options = ParseCommandLine();

    // --install-autostart / --remove-autostart: update HKCU\...\Run and exit
    // without opening a window.
    if (options.installAutostart || options.removeAutostart)
    {
        wchar_t modulePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        const bool enable = options.installAutostart && !options.removeAutostart;
        const bool ok = SetAutostart(enable, BuildAutostartCommand(modulePath));
        return ok ? 0 : 1;
    }

    ResolveWindowPlacement(options);
    // Double-click / shortcut launch (no arguments): behave like a mini-screen
    // gadget. Use --no-focus on the panel. The topmost flag is whatever the
    // command line (highest priority) or model.cfg says; with neither it stays
    // off so normal applications can cover the window.
    if (options.noArguments && options.placedOnMiniScreen)
    {
        options.noFocus = true;
        LogMsg("[win] no-argument launch on mini screen: topmost=%d (%s) nofocus=1",
               options.topmost ? 1 : 0,
               options.topmostSet ? "command line"
                                  : (options.configTopmostValid ? "model.cfg" : "default"));
    }
    g_uiScale = options.uiScale > 0.0f ? options.uiScale : 1.0f;
    options.windowPixelWidth =
        static_cast<int>(lroundf(static_cast<float>(options.width) * g_uiScale));
    options.windowPixelHeight =
        static_cast<int>(lroundf(static_cast<float>(options.height) * g_uiScale));

    g_editMode = false;  // model interaction starts locked
    g_panelsEnabled = options.panels;
    g_modelX = static_cast<int>(lroundf((options.panels ? 160.0f : 0.0f) * g_uiScale));
    g_modelY = 0;
    g_windowWidth = options.windowPixelWidth;
    g_windowHeight = options.windowPixelHeight;
    g_logicalWindowWidth = options.width;
    g_logicalWindowHeight = options.height;
    // Automated snapshot runs must not overwrite the saved model transform.
    g_persistModel = options.snapshotPath.empty();
    g_topmost = options.topmost;
    g_view.centerX = options.centerX;
    g_view.centerY = options.centerY;
    g_view.halfHeight = options.halfHeight;
    g_view.halfWidth = options.halfWidth;
    g_view.width = options.modelWidth;
    g_view.height = options.modelHeight;
    g_autostartChecked = IsAutostartEnabled();
    LogMsg("[panels] enabled=%d model viewport %dx%d at x=%d window=%dx%d (%dx%d px, scale %.2f)",
           options.panels ? 1 : 0, options.modelWidth, options.modelHeight,
           g_modelX, options.width, options.height, options.windowPixelWidth,
           options.windowPixelHeight, g_uiScale);
    LogMsg("=== NeneCubism ===");
    LogMsg("[args] model=%s view(cx=%.3f cy=%.3f hh=%.3f hw=%.3f) expr=%s breathOnly=%d snapshot=%s frame=%d",
           options.modelDir.c_str(), options.centerX, options.centerY, options.halfHeight,
           options.halfWidth, options.expression.c_str(), options.idleBreathOnly ? 1 : 0,
           options.snapshotPath.c_str(), options.snapshotFrame);
    LogMsg("[args] window=%dx%d logical (%dx%d px) at (%d,%d) scale=%.2f topmost=%d nofocus=%d bg=%02X%02X%02X edit=0",
           options.width, options.height, options.windowPixelWidth,
           options.windowPixelHeight, options.x, options.y, g_uiScale,
           options.topmost ? 1 : 0, options.noFocus ? 1 : 0,
           GetRValue(options.background), GetGValue(options.background),
           GetBValue(options.background));
    LogMsg("[args] click-motion=%d click-chest=%d chest-mode=%s idle-stare=%d stare=%.0f..%.0f s",
           options.clickMotion ? 1 : 0, options.clickChest ? 1 : 0,
           options.chestShy ? "shy" : "0721",
           options.idleStare ? 1 : 0,
           options.stareIntervalMin, options.stareIntervalMax);

    // Offline hit-test probe: --hit-test px,py  (prints model coords + result)
    if (!options.hitTestPixel.empty())
    {
        const size_t comma = options.hitTestPixel.find(',');
        if (comma == std::string::npos)
        {
            LogMsg("[hit] bad --hit-test, expected px,py");
            return 1;
        }
        const int px = atoi(options.hitTestPixel.substr(0, comma).c_str());
        const int py = atoi(options.hitTestPixel.c_str() + comma + 1);
        float mx = 0.0f;
        float my = 0.0f;
        ClientToModel(px, py, &mx, &my);
        const bool hit = (my > 0.44f && my < 0.78f && std::fabs(mx) < 0.28f);
        const bool chest = (my >= 0.10f && my <= 0.40f && std::fabs(mx) < 0.20f);
        LogMsg("[hit] pixel(%d,%d) -> model(%.3f,%.3f) head=%d chest=%d (window %dx%d, cx=%.3f cy=%.3f hh=%.3f hw=%.3f)",
               px, py, mx, my, hit ? 1 : 0, chest ? 1 : 0, g_view.width, g_view.height,
               g_view.centerX, g_view.centerY, g_view.halfHeight, g_view.halfWidth);
        return 0;
    }

    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC glrc = nullptr;
    if (!CreateGlWindow(instance, options, window, dc, glrc))
    {
        return 1;
    }
    g_fontDc = dc;

    // Place the window without ever activating it when --nofocus is given:
    // SetWindowPos with SWP_NOACTIVATE and SW_SHOWNOACTIVATE keep the current
    // foreground window untouched (SetForegroundWindow is never called).
    const UINT showFlags = options.noFocus ? SWP_NOACTIVATE : 0;
    SetWindowPos(window, options.topmost ? HWND_TOPMOST : HWND_TOP,
                 options.x, options.y, options.windowPixelWidth, options.windowPixelHeight,
                 SWP_SHOWWINDOW | showFlags);
    ShowWindow(window, options.noFocus ? SW_SHOWNOACTIVATE : SW_SHOW);
    UpdateWindow(window);
    LogMsg("[win] window shown, topmost=%d exstyle=0x%08lX",
           g_topmost ? 1 : 0,
           static_cast<unsigned long>(GetWindowLongPtrW(window, GWL_EXSTYLE)));

    const GLenum glewResult = glewInit();
    LogMsg("[gl] glewInit=%d vendor=%s renderer=%s version=%s",
           static_cast<int>(glewResult),
           reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
           reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
           reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    if (glewResult != GLEW_OK)
    {
        LogMsg("[gl] GLEW error: %s", reinterpret_cast<const char*>(glewGetErrorString(glewResult)));
    }

    GLint maxTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    LogMsg("[gl] max texture size = %d", maxTextureSize);

    // --- performance panels -------------------------------------------------
    PerformanceMonitor monitor;
    PanelRenderer panelRenderer;
    GLuint panelTexture = 0;
    if (g_panelsEnabled)
    {
        if (panelRenderer.Initialize(g_windowWidth, g_windowHeight, g_uiScale))
        {
            MonitorData initial;
            panelRenderer.Update(initial, g_autostartChecked);
            glGenTextures(1, &panelTexture);
            glBindTexture(GL_TEXTURE_2D, panelTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_windowWidth, g_windowHeight,
                         0, GL_BGRA, GL_UNSIGNED_BYTE, panelRenderer.Pixels());
            glBindTexture(GL_TEXTURE_2D, 0);
            g_panelRenderer = &panelRenderer;
            g_panelsDirty = false;
            LogMsg("[panel] OpenGL texture %u created", panelTexture);
        }
        else
        {
            LogMsg("[panel] renderer init failed; continuing without panels");
            g_panelsEnabled = false;
            g_modelX = 0;
            g_view.width = options.width;
            g_view.halfWidth = g_view.halfHeight *
                               (static_cast<float>(options.width) /
                                static_cast<float>(options.height));
        }
    }

    Csm::CubismFramework::Option cubismOption;
    cubismOption.LogFunction = CubismLogCallback;
    cubismOption.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Warning;
    cubismOption.LoadFileFunction = LoadFileAsBytes;
    cubismOption.ReleaseBytesFunction = ReleaseBytes;
    Csm::CubismFramework::StartUp(&g_allocator, &cubismOption);
    LogMsg("[cubism] StartUp done");
    Csm::CubismFramework::Initialize();
    LogMsg("[cubism] Initialize done");

    {
    LogMsg("[app] loading model...");
    NeneModel model;
    model.SetTextureFiltering(options.textureMipmap, options.textureAniso,
                              options.textureLodBias);
    // Motion selection/filtering is consumed by Load().
    model.SetMotionFiltering(options.filterMotion);
    model.SetMotionSelection(options.motionSpec);
    if (!model.Load(options.modelDir, options.idleBreathOnly))
    {
        LogMsg("[app] model load failed");
        MessageBoxW(nullptr, L"Live2D model load failed. See NeneCubism.log.",
                    L"NeneCubism", MB_ICONERROR | MB_OK);
        Csm::CubismFramework::Dispose();
        Csm::CubismFramework::CleanUp();
        return 1;
    }

    if (!options.expression.empty() && !model.ApplyExpression(options.expression))
    {
        LogMsg("[app] expression not found: %s", options.expression.c_str());
    }

    model.SetTrackingGain(options.trackGain);
    model.SetMotionGain(options.motionGain);
    model.SetBodySwayEnabled(options.bodySway);
    model.SetPhysicsEnabled(options.physicsEnabled);
    model.SetTrackBlend(options.trackBlend);
    model.SetHairSwayGain(options.hairSway);
    model.SetParamLogInterval(options.paramLogInterval);
    g_model = &model;
    g_clickMotionEnabled = options.clickMotion;
    g_clickChestEnabled = options.clickChest;
    g_chestShy = options.chestShy;
    for (const auto& entry : options.debugParams)
    {
        model.SetDebugParameter(entry.first, entry.second);
        LogMsg("[debug] %s = %.3f", entry.first.c_str(), entry.second);
    }
    LogMsg("[app] mouse tracking gain=%.2f motion gain=%.2f body sway=%d physics=%d "
           "motion-filter=%d track-blend=%.2f hair-sway=%.2f motion='%s'",
           options.trackGain, options.motionGain, options.bodySway ? 1 : 0,
           options.physicsEnabled ? 1 : 0, options.filterMotion ? 1 : 0,
           options.trackBlend, options.hairSway, options.motionSpec.c_str());
    LogMsg("[app] texture mipmap=%d aniso=%d lod-bias=%.2f ssaa=%.2f",
           options.textureMipmap ? 1 : 0, options.textureAniso, options.textureLodBias,
           options.ssaaFactor);

    if (!options.dumpParamsPath.empty())
    {
        std::string dumpPath = options.dumpParamsPath;
        if (!dumpPath.empty() && dumpPath[0] != '\\' && dumpPath[0] != '/' &&
            !(dumpPath.size() > 1 && dumpPath[1] == ':'))
        {
            dumpPath = PathJoin(g_exeDir, dumpPath);
        }
        FILE* dumpFile = _wfopen(WideFromUtf8(dumpPath).c_str(), L"wb");
        Csm::CubismModel* cubismModel = model.GetModel();
        if (dumpFile && cubismModel)
        {
            fprintf(dumpFile, "Id,Min,Max,Default,Current\n");
            const Csm::csmInt32 count = cubismModel->GetParameterCount();
            for (Csm::csmInt32 i = 0; i < count; ++i)
            {
                fprintf(dumpFile, "%s,%.6f,%.6f,%.6f,%.6f\n",
                        cubismModel->GetParameterId(i)->GetString().GetRawString(),
                        cubismModel->GetParameterMinimumValue(i),
                        cubismModel->GetParameterMaximumValue(i),
                        cubismModel->GetParameterDefaultValue(i),
                        cubismModel->GetParameterValue(i));
            }
            fclose(dumpFile);
            LogMsg("[dump] %d parameters -> %s", count, dumpPath.c_str());
        }
        else
        {
            LogMsg("[dump] cannot write %s", dumpPath.c_str());
        }
        if (options.exitAfterSnapshot)
        {
            Csm::Rendering::CubismOffscreenManager_OpenGLES2::ReleaseInstance();
            Csm::CubismFramework::Dispose();
            Csm::CubismFramework::CleanUp();
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(glrc);
            ReleaseDC(window, dc);
            DestroyWindow(window);
            return 0;
        }
    }

    LogMsg("[app] loaded, entering main loop");

    // --- system tray -------------------------------------------------------
    TrayIcon tray;
    if (options.tray)
    {
        tray.onShowWindow = [&]()
        {
            ShowWindow(window, SW_SHOWNOACTIVATE);
            SetWindowPos(window, g_topmost ? HWND_TOPMOST : HWND_TOP,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        };
        tray.onHideWindow = [&]()
        {
            ShowWindow(window, SW_HIDE);
        };
        tray.isWindowVisible = [&]()
        {
            return IsWindowVisible(window) != FALSE;
        };
        tray.onToggleLock = [&]()
        {
            g_editMode = !g_editMode;
            LogMsg("[ui] edit mode = %d (tray)", g_editMode ? 1 : 0);
        };
        tray.isLocked = [&]()
        {
            return !g_editMode;
        };
        tray.onToggleTopmost = [&]()
        {
            const bool newState = !IsWindowTopmost(window);
            ApplyTopmost(window, newState);
            LogMsg("[tray] topmost toggled -> %d", g_topmost ? 1 : 0);
            SaveModelConfig();
        };
        tray.isTopmost = [&]()
        {
            return IsWindowTopmost(window);
        };
        tray.onToggleAutostart = [&]()
        {
            const bool newState = !g_autostartChecked;
            wchar_t modulePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            if (SetAutostart(newState, BuildAutostartCommand(modulePath)))
            {
                g_autostartChecked = newState;
            }
            g_panelsDirty = true;
        };
        tray.isAutostart = []()
        {
            return IsAutostartEnabled();
        };
        tray.onExit = []()
        {
            SaveModelConfig();
            PostQuitMessage(0);
        };
        const HICON trayIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        if (tray.Create(instance, window, trayIcon, L"宁宁迷你屏"))
        {
            LogMsg("[tray] ready (tooltip will update every second)");
        }
        else
        {
            LogMsg("[tray] creation failed; window stays a tool window");
        }
    }
    else
    {
        LogMsg("[tray] disabled by --no-tray");
    }

    glViewport(0, 0, g_view.width, g_view.height);
    glClearColor(static_cast<GLfloat>(GetRValue(options.background)) / 255.0f,
                 static_cast<GLfloat>(GetGValue(options.background)) / 255.0f,
                 static_cast<GLfloat>(GetBValue(options.background)) / 255.0f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    LARGE_INTEGER frequency;
    LARGE_INTEGER previous;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previous);

    int appliedWidth = 0;
    int appliedHeight = 0;
    int appliedRenderWidth = 0;
    int appliedRenderHeight = 0;
    std::vector<bool> seqShotDone(options.snapshotSeq.size(), false);

    // Idle stare: random 120..300 s between triggers (--stare-interval-*).
    std::mt19937 rng(static_cast<unsigned int>(GetTickCount()));
    auto randRange = [&](float lo, float hi)
    {
        if (hi < lo) std::swap(lo, hi);
        std::uniform_real_distribution<float> dist(lo, hi);
        return dist(rng);
    };
    float stareTimer = 0.0f;
    float nextStare = 1.0e9f;
    if (options.idleStare)
    {
        nextStare = randRange(options.stareIntervalMin, options.stareIntervalMax);
        LogMsg("[stare] next automatic stare in %.1f s", nextStare);
    }
    bool running = true;
    int frame = 0;
    bool snapshotDone = false;
    float panelElapsed = 1.0f;      // first sample right away

    while (running)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running)
        {
            break;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float deltaSeconds = static_cast<float>(now.QuadPart - previous.QuadPart) /
                             static_cast<float>(frequency.QuadPart);
        previous = now;
        deltaSeconds = std::min(std::max(deltaSeconds, 0.0f), 0.1f);
        if (frame < 2)
        {
            deltaSeconds = 1.0f / 60.0f;
        }

        // Pick up the client size (WM_SIZE may already have updated it). The
        // Cubism camera always works in model-region pixels, not window pixels.
        RECT clientRect;
        if (GetClientRect(window, &clientRect))
        {
            const int clientWidth = clientRect.right - clientRect.left;
            const int clientHeight = clientRect.bottom - clientRect.top;
            if (clientWidth > 0 && clientHeight > 0 &&
                (clientWidth != appliedWidth || clientHeight != appliedHeight))
            {
                appliedWidth = clientWidth;
                appliedHeight = clientHeight;
                g_windowWidth = clientWidth;
                g_windowHeight = clientHeight;
                g_modelX = g_panelsEnabled ? 160 : 0;
                g_view.width = clientWidth - (g_panelsEnabled ? 320 : 0);
                g_view.height = clientHeight;
                glViewport(0, 0, clientWidth, clientHeight);
                if (!options.halfWidthSet)
                {
                    // Recompute horizontal framing for the new aspect ratio so
                    // the character is never stretched.
                    g_view.halfWidth = g_view.halfHeight *
                                       (static_cast<float>(g_view.width) /
                                        static_cast<float>(g_view.height));
                }
                LogMsg("[ui] client resized to %dx%d (model %dx%d at x=%d) hw=%.4f",
                       clientWidth, clientHeight, g_view.width, g_view.height,
                       g_modelX, g_view.halfWidth);
            }
        }

        // --- performance data: panels and/or tray tooltip ---------------------
        const bool panelsActive = g_panelsEnabled && g_panelRenderer &&
                                  g_panelRenderer->Ready() && panelTexture != 0;
        if (panelsActive || (options.tray && tray.IsCreated()))
        {
            panelElapsed += deltaSeconds;
            if (panelElapsed >= 1.0f || g_panelsDirty)
            {
                panelElapsed = 0.0f;
                MonitorData data;
                monitor.Poll(data);
                if (panelsActive)
                {
                    g_panelRenderer->Update(data, g_autostartChecked);
                    glBindTexture(GL_TEXTURE_2D, panelTexture);
                    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_windowWidth, g_windowHeight,
                                    GL_BGRA, GL_UNSIGNED_BYTE, g_panelRenderer->Pixels());
                    glBindTexture(GL_TEXTURE_2D, 0);
                    g_panelsDirty = false;
                    LogMsg("[monitor] cpu=%s%.1f gpu3d=%s%.0f cuda=%s%.0f vram=%s%.1f%%/%s%.1fG "
                           "temp=%s%.0f ram=%.1f up=%.1fKB/s down=%.1fKB/s",
                           data.cpuValid ? "" : "?", data.cpuPercent,
                           data.gpu3dValid ? "" : "?", data.gpu3dPercent,
                           data.gpuCudaValid ? "" : "?", data.gpuCudaPercent,
                           data.gpuMemValid ? "" : "?", data.gpuMemPercent,
                           data.gpuMemValid ? "" : "?", data.gpuMemUsedGB,
                           data.gpuTempValid ? "" : "?", data.gpuTempC,
                           data.ramPercent,
                           data.netUpBytesPerSec / 1024.0,
                           data.netDownBytesPerSec / 1024.0);
                }
                if (options.tray && tray.IsCreated())
                {
                    wchar_t cpuText[16] = L"--";
                    wchar_t gpuText[16] = L"--";
                    wchar_t tempText[16] = L"--";
                    if (data.cpuValid)
                    {
                        swprintf_s(cpuText, 16, L"%.0f%%", data.cpuPercent);
                    }
                    if (data.gpu3dValid)
                    {
                        swprintf_s(gpuText, 16, L"%.0f%%", data.gpu3dPercent);
                    }
                    if (data.gpuTempValid)
                    {
                        swprintf_s(tempText, 16, L"%.0f℃", data.gpuTempC);
                    }
                    wchar_t tooltip[128] = {};
                    swprintf_s(tooltip, 128, L"宁宁迷你屏\nCPU %s   GPU %s   %s",
                               cpuText, gpuText, tempText);
                    tray.SetTooltip(tooltip);
                }
            }
        }

        // --- interaction triggers ---------------------------------------------
        // Scripted hooks for automated screenshots.
        if (options.debugClickFrame >= 0 && frame == options.debugClickFrame)
        {
            model.TriggerHeadPat();
        }
        if (options.debugStareFrame >= 0 && frame == options.debugStareFrame)
        {
            model.TriggerStare();
        }
        if (options.debugShyFrame >= 0 && frame == options.debugShyFrame)
        {
            model.TriggerShy();
        }
        if (options.debugChestFrame >= 0 && frame == options.debugChestFrame)
        {
            model.TriggerChestMotion();
        }
        // Random idle stare; postpone (retry in ~2 s) while a head-pat plays.
        if (options.idleStare)
        {
            stareTimer += deltaSeconds;
            if (stareTimer >= nextStare)
            {
                if (model.IsActionPlaying())
                {
                    stareTimer = nextStare - 2.0f;
                    LogMsg("[stare] postponed (action playing)");
                }
                else
                {
                    model.TriggerStare();
                    stareTimer = 0.0f;
                    nextStare = randRange(options.stareIntervalMin,
                                          options.stareIntervalMax);
                    LogMsg("[stare] next automatic stare in %.1f s", nextStare);
                }
            }
        }

        // Global mouse position -> normalized window-relative target. The
        // cursor may be outside the window; the normalized value is clamped
        // to the edge so the model keeps looking toward it.
        // Scripted tracking targets take precedence over the real cursor and
        // over --track; the last key at or before the current frame is held.
        bool scriptedTrack = false;
        if (!options.trackSeq.empty())
        {
            const Options::TrackKey* active = nullptr;
            for (const auto& key : options.trackSeq)
            {
                if (key.frame <= frame)
                {
                    active = &key;
                }
            }
            if (active)
            {
                model.SetTrackingTarget(active->x, active->y);
                scriptedTrack = true;
            }
        }
        if (!scriptedTrack && options.hasTrackOverride)
        {
            model.SetTrackingTarget(options.trackOverrideX, options.trackOverrideY);
        }
        else if (!scriptedTrack && g_view.width > 0 && g_view.height > 0)
        {
            POINT cursor;
            if (GetCursorPos(&cursor))
            {
                ScreenToClient(window, &cursor);
                // Tracking is relative to the Live2D viewport, not the window:
                // the side panels must not pull the gaze sideways.
                const float localX = static_cast<float>(cursor.x - g_modelX) / g_uiScale;
                const float localY = static_cast<float>(cursor.y - g_modelY) / g_uiScale;
                const float halfWidth = static_cast<float>(g_view.width) * 0.5f;
                const float halfHeight = static_cast<float>(g_view.height) * 0.5f;
                const float targetX = (localX - halfWidth) / halfWidth;
                const float targetY = (localY - halfHeight) / halfHeight;
                model.SetTrackingTarget(targetX, targetY);
            }
        }

        model.Update(deltaSeconds);

        // --- full window: background + GDI+ side panels --------------------
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_windowWidth, g_windowHeight);
        glClearColor(static_cast<GLfloat>(GetRValue(options.background)) / 255.0f,
                     static_cast<GLfloat>(GetGValue(options.background)) / 255.0f,
                     static_cast<GLfloat>(GetBValue(options.background)) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if (g_panelsEnabled && panelTexture != 0)
        {
            DrawPanelTexture(panelTexture, g_windowWidth, g_windowHeight);
        }

        // --- supersampled model pass -------------------------------------
        const int modelPixelWidth =
            static_cast<int>(lroundf(static_cast<float>(g_view.width) * g_uiScale));
        const int modelPixelHeight =
            static_cast<int>(lroundf(static_cast<float>(g_view.height) * g_uiScale));
        int renderWidth = modelPixelWidth;
        int renderHeight = modelPixelHeight;
        bool useSsaa = false;
        if (options.ssaaFactor > 1.01f)
        {
            renderWidth = static_cast<int>(lroundf(static_cast<float>(modelPixelWidth) *
                                                   options.ssaaFactor));
            renderHeight = static_cast<int>(lroundf(static_cast<float>(modelPixelHeight) *
                                                    options.ssaaFactor));
            useSsaa = EnsureSsaaTarget(renderWidth, renderHeight);
            if (!useSsaa)
            {
                renderWidth = modelPixelWidth;
                renderHeight = modelPixelHeight;
            }
        }
        if (renderWidth != appliedRenderWidth || renderHeight != appliedRenderHeight)
        {
            appliedRenderWidth = renderWidth;
            appliedRenderHeight = renderHeight;
            model.SetRenderTargetSize(static_cast<Csm::csmUint32>(renderWidth),
                                      static_cast<Csm::csmUint32>(renderHeight));
        }

        if (useSsaa)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, g_ssaa.fbo);
            glViewport(0, 0, renderWidth, renderHeight);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
        else
        {
            // Direct path: keep the panels intact by clearing only the model
            // rectangle of the default framebuffer.
            glViewport(g_modelX, g_modelY, modelPixelWidth, modelPixelHeight);
            glEnable(GL_SCISSOR_TEST);
            glScissor(g_modelX, g_modelY, modelPixelWidth, modelPixelHeight);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }

        Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->BeginFrameProcess();
        model.Draw(modelPixelWidth, modelPixelHeight, g_view.centerX, g_view.centerY,
                   g_view.halfHeight, g_view.halfWidth);
        Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->EndFrameProcess();
        Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->ReleaseStaleRenderTextures();

        if (useSsaa)
        {
            ResolveSsaaToWindow(g_modelX, g_modelY, modelPixelWidth, modelPixelHeight);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        else
        {
            glDisable(GL_SCISSOR_TEST);
        }

        DrawUi(g_modelX, g_modelY, modelPixelWidth, modelPixelHeight,
               g_view.width, g_view.height, g_editMode);

        ++frame;
        if (!options.snapshotPath.empty() && !snapshotDone && frame >= options.snapshotFrame)
        {
            glFinish();
            std::string snapshotPath = options.snapshotPath;
            if (!snapshotPath.empty() && snapshotPath[0] != '\\' && snapshotPath[0] != '/' &&
                !(snapshotPath.size() > 1 && snapshotPath[1] == ':'))
            {
                snapshotPath = PathJoin(g_exeDir, snapshotPath);
            }
            SaveFramebufferPng(snapshotPath, g_windowWidth, g_windowHeight);
            if (!options.dumpMeshPath.empty())
            {
                std::string meshPath = options.dumpMeshPath;
                if (!meshPath.empty() && meshPath[0] != '\\' && meshPath[0] != '/' &&
                    !(meshPath.size() > 1 && meshPath[1] == ':'))
                {
                    meshPath = PathJoin(g_exeDir, meshPath);
                }
                model.DumpDrawables(meshPath);
            }
            snapshotDone = true;
            if (options.exitAfterSnapshot)
            {
                running = false;
            }
        }

        // Scripted multi-frame snapshots (used for the flick/inertia series).
        if (!options.snapshotSeq.empty())
        {
            bool allDone = true;
            for (size_t i = 0; i < options.snapshotSeq.size(); ++i)
            {
                const Options::SeqShot& shot = options.snapshotSeq[i];
                if (!seqShotDone[i] && frame >= shot.frame)
                {
                    glFinish();
                    std::string path = shot.path;
                    if (!path.empty() && path[0] != '\\' && path[0] != '/' &&
                        !(path.size() > 1 && path[1] == ':'))
                    {
                        path = PathJoin(g_exeDir, path);
                    }
                    SaveFramebufferPng(path, g_windowWidth, g_windowHeight);
                    seqShotDone[i] = true;
                }
                if (!seqShotDone[i])
                {
                    allDone = false;
                }
            }
            if (allDone && options.exitAfterSnapshot)
            {
                running = false;
            }
        }

        SwapBuffers(dc);

        if (frame % 120 == 0)
        {
            LogMsg("[app] frame %d", frame);
        }

        Sleep(1);
    }

    LogMsg("[app] shutting down after %d frames", frame);
    tray.Destroy();
    SaveModelConfig();

    } // NeneModel destroyed here, while the GL context is still current.

    if (panelTexture != 0)
    {
        glDeleteTextures(1, &panelTexture);
        panelTexture = 0;
    }
    panelRenderer.Shutdown();
    g_panelRenderer = nullptr;

    ReleaseSsaaTarget();

    // Make sure the renderer and all framework resources are released while
    // the GL context is still current.
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::ReleaseInstance();
    Csm::CubismFramework::Dispose();
    Csm::CubismFramework::CleanUp();

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(glrc);
    ReleaseDC(window, dc);
    DestroyWindow(window);

    return 0;
}

