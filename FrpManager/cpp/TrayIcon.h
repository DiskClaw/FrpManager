// TrayIcon.h - 系统托盘（气泡覆盖模式）
#pragma once
#include <windows.h>

struct ITrayCallback {
    virtual void OnTrayShow() = 0;
    virtual void OnTrayExit() = 0;
    virtual ~ITrayCallback() {}
};

static const int WM_TRAYICON = WM_USER + 1;

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    bool Create(HWND parent, ITrayCallback* cb, const wchar_t* tooltip);
    void Destroy();

    void ShowBalloon(const wchar_t* title, const wchar_t* msg);          // 默认 2 秒
    void ShowBalloon(const wchar_t* title, const wchar_t* msg, UINT timeoutMs);

    LRESULT HandleMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    static const int ID_SHOW = 1;
    static const int ID_EXIT = 2;

    void CancelBalloon();   // 取消当前气泡

    HWND parent_ = nullptr;
    ITrayCallback* cb_ = nullptr;
    HICON hIcon_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    bool balloonActive_ = false;
};