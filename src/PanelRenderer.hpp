#pragma once

#include <windows.h>

struct MonitorData;

// Renders the two 160 px side panels into a 32-bit top-down DIB with GDI+.
// The caller uploads Pixels() to an OpenGL texture and draws it as the window
// background, so the panels never touch the Cubism rendering path.
class PanelRenderer
{
public:
    PanelRenderer();
    ~PanelRenderer();

    bool Initialize(int width, int height, float scale);
    void Shutdown();

    // Redraws the whole 640x480 surface (call about once per second).
    void Update(const MonitorData& data, bool autostartChecked);

    const void* Pixels() const;
    int Width() const;
    int Height() const;
    bool Ready() const;

    // "开机启动" checkbox, in client (window) pixels.
    static bool HitTestAutostart(int clientX, int clientY, int windowWidth, int windowHeight);

private:
    struct Impl;
    Impl* _impl = nullptr;
    int _width = 0;
    int _height = 0;
    float _scale = 1.0f;
};
