#pragma once

#include <string>
#include <vector>
#include <map>

#include <CubismFramework.hpp>
#include <CubismModelSettingJson.hpp>
#include <Model/CubismUserModel.hpp>
#include <ICubismModelSetting.hpp>

// A minimal CubismUserModel that renders one model with the official
// OpenGL renderer of the Cubism Native Framework 5-r.5.
class NeneModel : public Csm::CubismUserModel
{
public:
    NeneModel();
    virtual ~NeneModel();

    // Loads model3.json + moc3 + textures + physics + motion + expressions.
    // breathOnly: do not play the idle motion; only programmatic breathing,
    // smile and hair physics are used.
    bool Load(const std::string& modelDirUtf8, bool breathOnly = false);

    // Advances motion / expression / physics / eye blink and the model.
    void Update(float deltaSeconds);

    // Sets up the camera and draws with the official Framework renderer.
    // centerX/centerY/halfHeight/halfWidth are in model-matrix space (model
    // height = 2). halfWidth <= 0 means "derive from the window aspect ratio"
    // (halfWidth = halfHeight * width / height).
    void Draw(int width, int height, float centerX, float centerY, float halfHeight,
              float halfWidth = 0.0f);

    bool ApplyExpression(const std::string& name);
    bool HasExpression(const std::string& name) const;
    std::vector<std::string> GetExpressionNames() const;

    // Mouse look: x/y are normalized window coordinates in [-1, 1]
    // (+x right, +y down). The model smooths toward this target.
    void SetTrackingGain(float gain);
    void SetTrackingTarget(float x, float y);
    // Global multiplier for head/body/hair motion amplitude.
    void SetMotionGain(float gain);
    // Enables/disables the very slow idle body sway (default off).
    void SetBodySwayEnabled(bool enabled);
    // Official physics (physics3.json) on/off. When off, ParamHair*/skirt keep
    // whatever the (filtered) motion and the rest pose leave in them.
    void SetPhysicsEnabled(bool enabled);
    // Keeps only ParamHair*/ParamBreath curves in the idle motion when true
    // (old behaviour); default false = play the complete authored motion.
    void SetMotionFiltering(bool filter);
    // 0..1 weight of the mouse-tracking offset added on top of the motion.
    void SetTrackBlend(float blend);
    // Amplitude multiplier of the hand-written idle hair sway (0 = off,
    // 1 = default, up to 3).
    void SetHairSwayGain(float gain);
    // Selects the idle motion: "" = default (largest file), a 1-based index,
    // or a substring of the file name.
    void SetMotionSelection(const std::string& spec);
    // Diagnostic: log a few physics-driven params every N Update() calls.
    void SetParamLogInterval(int frames);
    // Debug/testing: force a parameter value every frame (applied last).
    void SetDebugParameter(const std::string& id, float value);

    // --- interaction ------------------------------------------------------
    // Head hit test in model-matrix space (model height 2, centered).
    bool HitTestHead(float modelX, float modelY) const;
    // Chest / upper body hit region (below the head band, y 0.10..0.40).
    bool HitTestChest(float modelX, float modelY) const;
    // One-shot motions. They play once (even if the file says Loop=True),
    // fade in 0.4 s / out 0.6 s over the idle motion, then the idle resumes.
    bool TriggerHeadPat();
    bool TriggerStare();
    // Chest click: one-shot 0721 motion (ignored while another action plays).
    bool TriggerChestMotion();
    bool IsChestMotionLoaded() const { return _chestMotion != nullptr; }
    // Shy face: blush + shy + hatching, 0.25 s in / 2.5 s hold / 0.4 s out.
    bool TriggerShy();
    bool IsShyActive() const { return _shyTime >= 0.0f; }
    bool IsActionPlaying();
    bool HasHeadPatMotion() const { return _headPatMotion != nullptr; }
    bool HasStareMotion() const { return _stareMotion != nullptr; }

    // Texture sampling. Must be called before Load() (textures are uploaded
    // there). mipmap=true keeps GL_LINEAR_MIPMAP_LINEAR for minification;
    // anisoLevel asks the driver for anisotropic filtering (1 = off).
    void SetTextureFiltering(bool mipmap, int anisoLevel, float lodBias);

    // Writes every drawable vertex (post-deformation, model-matrix space) to a
    // CSV so the body deformation can be measured numerically.
    void DumpDrawables(const std::string& pathUtf8);

    float GetCanvasWidth() const;
    float GetCanvasHeight() const;

private:
    void SetupTextures();

    Csm::CubismModelSettingJson*               _setting;
    std::string                                _modelDir;
    Csm::csmVector<Csm::CubismIdHandle>        _eyeBlinkIds;
    Csm::csmVector<Csm::CubismIdHandle>        _lipSyncIds;
    Csm::ACubismMotion*                        _idleMotion;
    std::map<std::string, Csm::ACubismMotion*> _expressions;
    bool                                       _breathOnly;
    bool                                       _defaultSmile;
    float                                      _elapsedSeconds;

    Csm::CubismIdHandle _idParamBreath;
    Csm::CubismIdHandle _idParamMouthForm;
    Csm::CubismIdHandle _idParamMouthOpenY;
    Csm::CubismIdHandle _idParamEyeLSmile;
    Csm::CubismIdHandle _idParamEyeRSmile;
    Csm::CubismIdHandle _idParamAngleX;
    Csm::CubismIdHandle _idParamAngleY;
    Csm::CubismIdHandle _idParamAngleZ;
    Csm::CubismIdHandle _idParamEyeBallX;
    Csm::CubismIdHandle _idParamEyeBallY;
    Csm::CubismIdHandle _idParamBodyAngleX;
    Csm::CubismIdHandle _idParamBodyAngleY;
    Csm::CubismIdHandle _idParamBodyAngleZ;
    // "全体Z" master lean of the whole figure (pivot near the feet). Used as a
    // small addition on top of ParamBodyAngleZ, which is the real waist bend.
    Csm::CubismIdHandle _idParamWholeAngleZ;

    bool  _useMipmap = true;
    int   _anisoLevel = 16;
    float _lodBias = 0.0f;

    float _trackingGain;
    float _motionGain;
    bool  _bodySwayEnabled;
    bool  _physicsEnabled = true;
    float _hairSwayGain = 1.0f;
    // One-shot interaction motions layered over the looping idle motion.
    Csm::CubismMotionManager _actionMotionManager;
    Csm::ACubismMotion*      _headPatMotion = nullptr;
    Csm::ACubismMotion*      _stareMotion = nullptr;
    bool  _actionPlaying = false;
    float _actionElapsed = 0.0f;
    Csm::ACubismMotion* _chestMotion = nullptr;
    // Shy expression: elapsed time since the trigger (-1 = inactive).
    float _shyTime = -1.0f;
    // Blush riding along with whichever one-shot action is playing
    // (-1 = inactive); duration is that motion's length for the fade-out.
    float _actionBlushTime = -1.0f;
    float _actionBlushDuration = 0.0f;
    void BeginActionBlush(Csm::ACubismMotion* motion);
    Csm::CubismIdHandle _idParamCheek = nullptr;
    Csm::CubismIdHandle _idParamCheek5 = nullptr;
    Csm::CubismIdHandle _idParamCheek6 = nullptr;
    bool  _filterMotion = false;
    float _trackBlend = 1.0f;
    std::string _motionSpec;
    int   _paramLogDivider = 0;
    int   _paramLogCounter = 0;
    float _trackTargetX;
    float _trackTargetY;
    float _trackCurrentX;
    float _trackCurrentY;
    std::map<std::string, float> _debugParameters;
};
