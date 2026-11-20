#pragma once

#include <windows.h>
#include <functional>
#include <string>

// Notification-area icon backed by a hidden message-only window.
// The window owns the Shell_NotifyIcon registration and shows the context
// menu on right click; all application actions are injected as callbacks so
// this module does not need to know about the renderer or the camera state.
class TrayIcon
{
public:
    TrayIcon();
    ~TrayIcon();

    bool Create(HINSTANCE instance, HWND mainWindow, HICON icon,
                const std::wstring& tooltip);
    void Destroy();
    void SetTooltip(const std::wstring& tooltip);
    bool IsCreated() const { return _added; }

    std::function<void()> onShowWindow;
    std::function<void()> onHideWindow;
    std::function<bool()> isWindowVisible;
    std::function<void()> onToggleLock;
    std::function<bool()> isLocked;
    std::function<void()> onToggleTopmost;
    std::function<bool()> isTopmost;
    std::function<void()> onToggleAutostart;
    std::function<bool()> isAutostart;
    std::function<void()> onExit;

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void ShowContextMenu();
    void HandleCommand(int commandId);

    HWND _hwnd = nullptr;
    HWND _mainWindow = nullptr;
    HICON _icon = nullptr;
    bool _added = false;
    bool _classRegistered = false;
    unsigned long long _lastLeftClick = 0;
    NOTIFYICONDATAW _nid = {};
};
