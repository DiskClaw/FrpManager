// TrayIcon.h - 系统托盘（气泡覆盖模式）
#pragma once
#include <windows.h>
#include <string>

struct ITrayCallback {
    virtual void OnTrayShow() = 0;
    virtual void OnTrayExit() = 0;
    virtual ~ITrayCallback() {}
};

static const int WM_TRAYICON = WM_USER + 1;
static const int WM_TOGGLE_LANG = WM_USER + 13;
static const int WM_HIDE_TRAY = WM_USER + 7;

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    bool Create(HWND parent, ITrayCallback* cb, const wchar_t* tooltip);
    bool IsCreated() const { return nid_.uFlags != 0; }
    void Destroy();

    void ShowBalloon(const wchar_t* title, const wchar_t* msg);          // 默认 2 秒
    void ShowBalloon(const wchar_t* title, const wchar_t* msg, UINT timeoutMs);

    void SetTooltip(const wchar_t* tooltip);   // 更新托盘提示文字
    void SetMenuStrings(const wchar_t* show, const wchar_t* lang, const wchar_t* hide, const wchar_t* exit);

    LRESULT HandleMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    static const int ID_SHOW = 1;
    static const int ID_EXIT = 2;
    static const int ID_LANG = 3;
    static const int ID_HIDE_ICON = 4;

    void CancelBalloon();

    HWND parent_ = nullptr;
    ITrayCallback* cb_ = nullptr;
    HICON hIcon_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    bool balloonActive_ = false;
    std::wstring showLabel_ = L"显示窗口";
    std::wstring langLabel_ = L"切换到 English";
    std::wstring hideLabel_ = L"隐藏图标";
    std::wstring exitLabel_ = L"退出";
};