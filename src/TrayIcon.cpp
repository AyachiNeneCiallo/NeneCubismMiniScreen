#include "TrayIcon.hpp"
#include "Common.hpp"

#include <cstring>

namespace
{
    const wchar_t* kTrayWindowClass = L"NeneCubismTrayWindow";
    const UINT kTrayCallbackMessage = WM_APP + 1;
    const UINT kTrayIconId = 1;

    enum MenuCommand
    {
        kCmdToggleWindow = 40001,
        kCmdToggleLock = 40002,
        kCmdToggleAutostart = 40003,
        kCmdExit = 40004
    };

    void CopyTip(wchar_t (&dest)[128], const std::wstring& text)
    {
        wcsncpy_s(dest, text.c_str(), _TRUNCATE);
    }
}

TrayIcon::TrayIcon()
{
}

TrayIcon::~TrayIcon()
{
    Destroy();
}

bool TrayIcon::Create(HINSTANCE instance, HWND mainWindow, HICON icon,
                      const std::wstring& tooltip)
{
    if (_added)
    {
        return true;
    }
    _mainWindow = mainWindow;
    _icon = icon;

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = TrayIcon::WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kTrayWindowClass;
    if (RegisterClassExW(&windowClass) != 0)
    {
        _classRegistered = true;
    }
    else if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        LogMsg("[tray] RegisterClassEx failed: %lu", GetLastError());
        return false;
    }

    _hwnd = CreateWindowExW(0, kTrayWindowClass, L"", 0, 0, 0, 0, 0,
                            HWND_MESSAGE, nullptr, instance, this);
    if (!_hwnd)
    {
        LogMsg("[tray] message window creation failed: %lu", GetLastError());
        return false;
    }

    _nid.cbSize = sizeof(_nid);
    _nid.hWnd = _hwnd;
    _nid.uID = kTrayIconId;
    _nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    _nid.uCallbackMessage = kTrayCallbackMessage;
    _nid.hIcon = icon ? icon : LoadIconW(instance, MAKEINTRESOURCEW(1));
    CopyTip(_nid.szTip, tooltip);

    // Explorer may still be starting when this app auto-starts with Windows.
    bool ok = false;
    for (int attempt = 0; attempt < 5 && !ok; ++attempt)
    {
        ok = Shell_NotifyIconW(NIM_ADD, &_nid) != FALSE;
        if (!ok)
        {
            Sleep(200);
        }
    }
    if (!ok)
    {
        LogMsg("[tray] Shell_NotifyIcon(NIM_ADD) failed");
        return false;
    }
    _added = true;
    LogMsg("[tray] icon added (hwnd=%p)", _hwnd);
    return true;
}

void TrayIcon::Destroy()
{
    if (_added)
    {
        Shell_NotifyIconW(NIM_DELETE, &_nid);
        _added = false;
        LogMsg("[tray] icon removed");
    }
    if (_hwnd)
    {
        DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
    if (_classRegistered)
    {
        if (GetModuleHandleW(nullptr))
        {
            UnregisterClassW(kTrayWindowClass, GetModuleHandleW(nullptr));
        }
        _classRegistered = false;
    }
}

void TrayIcon::SetTooltip(const std::wstring& tooltip)
{
    if (!_added)
    {
        return;
    }
    CopyTip(_nid.szTip, tooltip);
    _nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &_nid);
}

LRESULT CALLBACK TrayIcon::WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                      LPARAM lParam)
{
    TrayIcon* tray = reinterpret_cast<TrayIcon*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && tray == nullptr)
    {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        tray = reinterpret_cast<TrayIcon*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tray));
    }
    if (tray != nullptr)
    {
        return tray->HandleMessage(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT TrayIcon::HandleMessage(HWND hwnd, UINT message, WPARAM wParam,
                                LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        HandleCommand(LOWORD(wParam));
        return 0;
    case kTrayCallbackMessage:
    {
        const UINT event = static_cast<UINT>(LOWORD(lParam));
        if (event == WM_LBUTTONUP)
        {
            // Single click toggles, but a double click must end with the
            // window shown. Suppress the hide that would otherwise follow the
            // double-click's second button-up.
            const unsigned long long now = GetTickCount64();
            const bool visible = isWindowVisible && isWindowVisible();
            if (!visible)
            {
                if (onShowWindow) onShowWindow();
            }
            else if (now - _lastLeftClick > 400)
            {
                if (onHideWindow) onHideWindow();
            }
            _lastLeftClick = now;
        }
        else if (event == WM_LBUTTONDBLCLK)
        {
            if (onShowWindow) onShowWindow();
        }
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
        {
            ShowContextMenu();
        }
        return 0;
    }
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

void TrayIcon::ShowContextMenu()
{
    const bool visible = isWindowVisible && isWindowVisible();
    const bool locked = isLocked && isLocked();
    const bool autostart = isAutostart && isAutostart();

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }
    AppendMenuW(menu, MF_STRING | (visible ? MF_CHECKED : 0), kCmdToggleWindow,
                visible ? L"隐藏窗口" : L"显示窗口");
    AppendMenuW(menu, MF_STRING | (locked ? MF_CHECKED : 0), kCmdToggleLock,
                L"锁定模型");
    AppendMenuW(menu, MF_STRING | (autostart ? MF_CHECKED : 0), kCmdToggleAutostart,
                L"开机启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出");

    POINT cursor = {};
    GetCursorPos(&cursor);
    // The menu owner is the message-only window; make the main window
    // foreground first so clicking elsewhere closes the popup as usual.
    if (_mainWindow)
    {
        SetForegroundWindow(_mainWindow);
    }
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, cursor.x, cursor.y,
                   0, _hwnd, nullptr);
    PostMessageW(_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void TrayIcon::HandleCommand(int commandId)
{
    switch (commandId)
    {
    case kCmdToggleWindow:
    {
        const bool visible = isWindowVisible && isWindowVisible();
        if (visible)
        {
            if (onHideWindow) onHideWindow();
        }
        else if (onShowWindow)
        {
            onShowWindow();
        }
        break;
    }
    case kCmdToggleLock:
        if (onToggleLock) onToggleLock();
        break;
    case kCmdToggleAutostart:
        if (onToggleAutostart) onToggleAutostart();
        break;
    case kCmdExit:
        if (onExit) onExit();
        break;
    default:
        break;
    }
}
