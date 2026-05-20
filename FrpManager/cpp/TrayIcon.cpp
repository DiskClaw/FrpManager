#include "TrayIcon.h"
#include <shellapi.h>

TrayIcon::TrayIcon() = default;

TrayIcon::~TrayIcon() {
    Destroy();
}

// 创建系统托盘图标（图标、回调消息、Tooltip）
bool TrayIcon::Create(HWND parent, ITrayCallback* cb, const wchar_t* tooltip) {
    parent_ = parent;
    cb_ = cb;

    hIcon_ = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1));
    if (!hIcon_) hIcon_ = LoadIconW(nullptr, IDI_APPLICATION);

    nid_ = { sizeof(NOTIFYICONDATAW) };
    nid_.hWnd = parent;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.hIcon = hIcon_;
    nid_.uCallbackMessage = WM_TRAYICON;
    if (tooltip) wcscpy_s(nid_.szTip, _countof(nid_.szTip), tooltip);

    return Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

// 销毁托盘图标
void TrayIcon::Destroy() {
    CancelBalloon();
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    nid_ = {};
}

// 更新托盘 Tooltip 文本
void TrayIcon::SetTooltip(const wchar_t* tooltip) {
    if (!tooltip) return;
    wcscpy_s(nid_.szTip, _countof(nid_.szTip), tooltip);
    nid_.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

// 设置托盘右键菜单项文本（显示、语言切换、退出）
void TrayIcon::SetMenuStrings(const wchar_t* show, const wchar_t* lang, const wchar_t* hide, const wchar_t* exit) {
    if (show) showLabel_ = show;
    if (lang) langLabel_ = lang;
    if (hide) hideLabel_ = hide;
    if (exit) exitLabel_ = exit;
}

// 显示托盘气泡提示（默认2秒）
void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* msg) {
    ShowBalloon(title, msg, 2000);
}

// 显示托盘气泡提示（指定超时）
void TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* msg, UINT timeoutMs) {
    if (!parent_) return;

    // 覆盖模式：先取消当前气泡
    if (balloonActive_)
        CancelBalloon();

    nid_.uFlags = NIF_INFO;
    nid_.uTimeout = timeoutMs;
    wcscpy_s(nid_.szInfoTitle, _countof(nid_.szInfoTitle), title ? title : L"");
    wcscpy_s(nid_.szInfo, _countof(nid_.szInfo), msg ? msg : L"");
    nid_.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    balloonActive_ = true;
}

// 取消气泡提示
void TrayIcon::CancelBalloon() {
    if (!balloonActive_) return;
    nid_.uFlags = NIF_INFO;
    nid_.szInfo[0] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
    balloonActive_ = false;
}

// 处理托盘图标消息：右键菜单、左键双击、气泡点击
LRESULT TrayIcon::HandleMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_TRAYICON) return 0;

    switch (LOWORD(lp)) {
    case NIN_BALLOONTIMEOUT:
    case NIN_BALLOONUSERCLICK:
    case NIN_BALLOONHIDE:
        balloonActive_ = false;
        break;

    case WM_RBUTTONUP:
    case WM_LBUTTONUP: {
        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, ID_SHOW, showLabel_.c_str());
        AppendMenuW(hMenu, MF_STRING, ID_LANG, langLabel_.c_str());
        AppendMenuW(hMenu, MF_STRING, ID_HIDE_ICON, hideLabel_.c_str());
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, ID_EXIT, exitLabel_.c_str());
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(parent_);
        int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
            pt.x, pt.y, 0, parent_, nullptr);
        DestroyMenu(hMenu);
        if (cmd == ID_SHOW && cb_) cb_->OnTrayShow();
        else if (cmd == ID_LANG) PostMessageW(parent_, WM_TOGGLE_LANG, 0, 0);
        else if (cmd == ID_HIDE_ICON) PostMessageW(parent_, WM_HIDE_TRAY, 0, 0);
        else if (cmd == ID_EXIT && cb_) cb_->OnTrayExit();
        break;
    }
    case WM_LBUTTONDBLCLK:
        if (cb_) cb_->OnTrayShow();
        break;
    }
    return 0;
}