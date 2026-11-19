#include "PanelRenderer.hpp"
#include "MonitorData.hpp"
#include "Common.hpp"

#include <windows.h>
#include <gdiplus.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace
{
    const int kPanelWidth = 160;
    const float kLabelSize = 20.0f;
    const float kValueSize = 27.0f;
    const float kBigValueSize = 34.0f;
    const float kSubSize = 20.0f;
    const float kCheckSize = 18.0f;
    const float kBarHeight = 24.0f;

    const Color kPink(255, 255, 102, 255);
    const Color kTrack(255, 255, 226, 255);
    const Color kSeparator(255, 252, 216, 252);

    void AddRoundedRect(GraphicsPath& path, const RectF& rect, float radius)
    {
        const float diameter = radius * 2.0f;
        if (rect.Width <= diameter || rect.Height <= diameter)
        {
            path.AddRectangle(rect);
            return;
        }
        path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                    diameter, diameter, 0.0f, 90.0f);
        path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();
    }

    void FillRounded(Graphics& graphics, const RectF& rect, float radius, Brush& brush)
    {
        GraphicsPath path;
        AddRoundedRect(path, rect, radius);
        graphics.FillPath(&brush, &path);
    }

    const wchar_t* ResolveFamily(const wchar_t* preferred, const wchar_t* fallback)
    {
        FontFamily family(preferred);
        return family.IsAvailable() ? preferred : fallback;
    }

    void FormatPercent(wchar_t* buffer, size_t count, bool valid, double value)
    {
        if (valid)
        {
            swprintf_s(buffer, count, L"%.0f%%", value);
        }
        else
        {
            wcscpy_s(buffer, count, L"--");
        }
    }

    void FormatSpeed(wchar_t* buffer, size_t count, double bytesPerSecond)
    {
        const double kb = bytesPerSecond / 1024.0;
        if (kb >= 1024.0 * 1024.0)
        {
            swprintf_s(buffer, count, L"%.1fGB/s", kb / (1024.0 * 1024.0));
        }
        else if (kb >= 1024.0)
        {
            const double mb = kb / 1024.0;
            swprintf_s(buffer, count, mb >= 10.0 ? L"%.0fMB/s" : L"%.1fMB/s", mb);
        }
        else if (kb >= 10.0)
        {
            // "↓43.0KB/s" would overflow the 132 px column at 27 px Consolas;
            // keep one decimal only for small rates.
            swprintf_s(buffer, count, L"%.0fKB/s", kb);
        }
        else
        {
            swprintf_s(buffer, count, L"%.1fKB/s", kb);
        }
    }

    // Draws one data row at (x, y): label, value, optional thick bar, optional
    // sub-value. The row advances 113 px, which fills a 480 px panel with four
    // rows and leaves the startup checkbox room at the bottom.
    void DrawRow(Graphics& graphics, Font& labelFont, Font& valueFont, Font& bigValueFont,
                 Font& subFont, SolidBrush& pink, SolidBrush& trackBrush, float x, float y,
                 float width, const wchar_t* label, const wchar_t* value, bool hasBar,
                 double percent, const wchar_t* sub, bool bigValue)
    {
        graphics.DrawString(label, -1, &labelFont, PointF(x, y), &pink);
        graphics.DrawString(value, -1, bigValue ? &bigValueFont : &valueFont,
                            PointF(x, y + 27.0f), &pink);

        if (hasBar)
        {
            const RectF bar(x, y + 66.0f, width, kBarHeight);
            FillRounded(graphics, bar, kBarHeight * 0.5f, trackBrush);
            const double clamped = std::max(0.0, std::min(100.0, percent));
            const float fillWidth = static_cast<float>(width * clamped / 100.0);
            if (fillWidth >= 2.0f)
            {
                const RectF fill(x, y + 66.0f,
                                 std::max(fillWidth, kBarHeight * 0.5f), kBarHeight);
                FillRounded(graphics, fill, kBarHeight * 0.5f, pink);
            }
        }

        if (sub != nullptr)
        {
            graphics.DrawString(sub, -1, &subFont, PointF(x, y + 96.0f), &pink);
        }
    }
}

struct PanelRenderer::Impl
{
    GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    bool gdiplusStarted = false;

    HBITMAP bitmap = nullptr;
    HDC memoryDc = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;
};

PanelRenderer::PanelRenderer()
{
}

PanelRenderer::~PanelRenderer()
{
    Shutdown();
}

bool PanelRenderer::Initialize(int width, int height, float scale)
{
    if (_impl != nullptr && _impl->width == width && _impl->height == height &&
        std::fabs(_scale - scale) < 0.001f)
    {
        return true;
    }
    Shutdown();

    if (width <= 0 || height <= 0 || scale <= 0.1f)
    {
        return false;
    }
    _scale = scale;

    _impl = new Impl();
    if (!_impl->gdiplusStarted)
    {
        const Status status = GdiplusStartup(&_impl->token, &_impl->startupInput, nullptr);
        if (status != Ok)
        {
            LogMsg("[panel] GdiplusStartup failed: %d", static_cast<int>(status));
            Shutdown();
            return false;
        }
        _impl->gdiplusStarted = true;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;      // top-down: row 0 is the top
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    _impl->bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &_impl->bits,
                                     nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!_impl->bitmap || !_impl->bits)
    {
        LogMsg("[panel] CreateDIBSection failed: %lu", GetLastError());
        Shutdown();
        return false;
    }
    _impl->memoryDc = CreateCompatibleDC(nullptr);
    if (!_impl->memoryDc)
    {
        Shutdown();
        return false;
    }
    _impl->previousBitmap = SelectObject(_impl->memoryDc, _impl->bitmap);
    _impl->width = width;
    _impl->height = height;
    _width = width;
    _height = height;
    LogMsg("[panel] surface %dx%d ready", width, height);
    return true;
}

void PanelRenderer::Shutdown()
{
    if (_impl == nullptr)
    {
        return;
    }
    if (_impl->memoryDc)
    {
        if (_impl->previousBitmap)
        {
            SelectObject(_impl->memoryDc, _impl->previousBitmap);
        }
        DeleteDC(_impl->memoryDc);
        _impl->memoryDc = nullptr;
    }
    if (_impl->bitmap)
    {
        DeleteObject(_impl->bitmap);
        _impl->bitmap = nullptr;
    }
    if (_impl->gdiplusStarted)
    {
        GdiplusShutdown(_impl->token);
        _impl->gdiplusStarted = false;
    }
    delete _impl;
    _impl = nullptr;
    _width = 0;
    _height = 0;
    _scale = 1.0f;
}

bool PanelRenderer::Ready() const
{
    return _impl != nullptr && _impl->bits != nullptr;
}

const void* PanelRenderer::Pixels() const
{
    return _impl ? _impl->bits : nullptr;
}

int PanelRenderer::Width() const
{
    return _width;
}

int PanelRenderer::Height() const
{
    return _height;
}

bool PanelRenderer::HitTestAutostart(int clientX, int clientY, int windowWidth,
                                     int windowHeight)
{
    const int left = windowWidth - kPanelWidth + 6;
    const int top = windowHeight - 50;
    return clientX >= left && clientX < windowWidth - 4 &&
           clientY >= top && clientY < windowHeight - 2;
}

void PanelRenderer::Update(const MonitorData& data, bool autostartChecked)
{
    if (!Ready())
    {
        return;
    }

    Graphics graphics(_impl->memoryDc);
    graphics.Clear(Color(255, 255, 255, 255));
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    // All coordinates below are the 640x480 logical layout; GDI+ scales the
    // surface up to the monitor's physical pixel size (e.g. 1.5x at 144 DPI).
    graphics.ScaleTransform(_scale, _scale);

    const int logicalWidth = static_cast<int>(lroundf(static_cast<float>(_width) / _scale));
    const int logicalHeight = static_cast<int>(lroundf(static_cast<float>(_height) / _scale));

    const wchar_t* labelFamily = ResolveFamily(L"Microsoft YaHei", L"Segoe UI");
    const wchar_t* valueFamily = ResolveFamily(L"Consolas", L"Segoe UI");

    Font labelFont(labelFamily, kLabelSize, FontStyleBold, UnitPixel);
    Font valueFont(valueFamily, kValueSize, FontStyleBold, UnitPixel);
    Font bigValueFont(valueFamily, kBigValueSize, FontStyleBold, UnitPixel);
    Font subFont(labelFamily, kSubSize, FontStyleRegular, UnitPixel);
    Font checkFont(labelFamily, kCheckSize, FontStyleBold, UnitPixel);
    SolidBrush pink(kPink);
    SolidBrush trackBrush(kTrack);
    Pen separatorPen(kSeparator, 1.0f);
    Pen checkPen(kPink, 2.0f);

    // Panel separators.
    graphics.DrawLine(&separatorPen, static_cast<float>(kPanelWidth), 0.0f,
                      static_cast<float>(kPanelWidth), static_cast<float>(logicalHeight));
    graphics.DrawLine(&separatorPen, static_cast<float>(logicalWidth - kPanelWidth), 0.0f,
                      static_cast<float>(logicalWidth - kPanelWidth),
                      static_cast<float>(logicalHeight));

    const float innerWidth = static_cast<float>(kPanelWidth - 28);
    const float leftX = 14.0f;
    const float rightX = static_cast<float>(logicalWidth - kPanelWidth + 14);

    // ---------------- left panel: CPU / GPU 3D / GPU CUDA / VRAM ----------
    wchar_t text[64];
    FormatPercent(text, 64, data.cpuValid, data.cpuPercent);
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            leftX, 10.0f, innerWidth, L"CPU", text, true, data.cpuPercent, nullptr, false);

    FormatPercent(text, 64, data.gpu3dValid, data.gpu3dPercent);
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            leftX, 123.0f, innerWidth, L"GPU 3D", text, true, data.gpu3dPercent, nullptr,
            false);

    FormatPercent(text, 64, data.gpuCudaValid, data.gpuCudaPercent);
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            leftX, 236.0f, innerWidth, L"GPU CUDA", text, true, data.gpuCudaPercent,
            nullptr, false);

    FormatPercent(text, 64, data.gpuMemValid, data.gpuMemPercent);
    wchar_t sub[64] = L"--";
    if (data.gpuMemValid)
    {
        swprintf_s(sub, 64, L"已用 %.1fG", data.gpuMemUsedGB);
    }
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            leftX, 349.0f, innerWidth, L"显存", text, true, data.gpuMemPercent, sub, false);

    // ---------------- right panel: temp / RAM / up / down -----------------
    if (data.gpuTempValid)
    {
        swprintf_s(text, 64, L"%.0f℃", data.gpuTempC);
    }
    else
    {
        wcscpy_s(text, 64, L"--");
    }
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            rightX, 10.0f, innerWidth, L"GPU 温度", text, false, 0.0, nullptr, true);

    FormatPercent(text, 64, data.ramValid, data.ramPercent);
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            rightX, 123.0f, innerWidth, L"内存", text, true, data.ramPercent, nullptr,
            false);

    wchar_t speed[64];
    FormatSpeed(speed, 64, data.netUpBytesPerSec);
    swprintf_s(text, 64, L"↑%s", data.netValid ? speed : L"--");
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            rightX, 236.0f, innerWidth, L"上传", text, false, 0.0, nullptr, false);

    FormatSpeed(speed, 64, data.netDownBytesPerSec);
    swprintf_s(text, 64, L"↓%s", data.netValid ? speed : L"--");
    DrawRow(graphics, labelFont, valueFont, bigValueFont, subFont, pink, trackBrush,
            rightX, 349.0f, innerWidth, L"下载", text, false, 0.0, nullptr, false);

    // ---------------- autostart checkbox (bottom right) --------------------
    const float checkX = static_cast<float>(logicalWidth - kPanelWidth + 12);
    const float checkY = static_cast<float>(logicalHeight - 40);
    const RectF box(checkX, checkY + 1.0f, 18.0f, 18.0f);
    GraphicsPath boxPath;
    AddRoundedRect(boxPath, box, 4.0f);
    graphics.DrawPath(&checkPen, &boxPath);
    if (autostartChecked)
    {
        const RectF inner(checkX + 5.0f, checkY + 6.0f, 8.0f, 8.0f);
        graphics.FillRectangle(&pink, inner);
    }
    graphics.DrawString(L"开机启动", -1, &checkFont,
                        PointF(checkX + 26.0f, checkY), &pink);
}
