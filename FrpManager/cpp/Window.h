// Window.h - 主窗口
#pragma once
#include <windows.h>
#include "FrpProcess.h"
#include "TrayIcon.h"       // 包含 WM_TRAYICON 定义
#include "TomlHelper.h"
#include "Settings.h"

// 自定义消息定义
enum {
    WM_OUTPUT = WM_USER + 10,  // 进程输出消息
    WM_TRAY_EXIT = WM_USER + 11,  // 托盘退出消息
    WM_PROCESS_EXIT = WM_USER + 12   // 进程退出消息
};

// 主窗口类，实现进程回调与托盘回调接口
class MainWindow : public IFrpProcessCallback, public ITrayCallback {
public:
    MainWindow();
    ~MainWindow();

    bool RegisterClass();
    HWND CreateWindow_();
    void SetFrpRoot(const std::wstring& path);

    // 停止所有进程并更新控件状态（供 Settings 调用）
    void StopAllProcesses();

private:
    // ---- IFrpProcessCallback 接口 ----
    void OnOutput(FrpMode mode, const char* line, int len) override;
    void OnExit(FrpMode mode, DWORD exitCode) override;

    // ---- ITrayCallback 接口 ----
    void OnTrayShow() override;
    void OnTrayExit() override;

    // Windows 消息处理
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMsg(UINT msg, WPARAM wp, LPARAM lp);

    // 布局与控件更新
    void OnSize(int w, int h);
    void OnCommand(int id);
    void UpdateProcessControls();
    void RefreshSummary();
    bool ShowSettingsDialog();
    void StartProcess(FrpMode mode);
    void StopProcess(FrpMode mode);
    void HandleProcessExitUi(FrpMode mode, DWORD exitCode);

    // 辅助
    FrpProcess& ProcessForMode(FrpMode mode);
    const wchar_t* ModeName(FrpMode mode) const;
    std::wstring ConfigPathForMode(FrpMode mode) const;
    std::wstring BuildSummaryText(FrpMode mode) const;
    void CheckConfigChanges();
    bool CanShowBalloon() const;   // 托盘气泡防抖

    // ---- 窗口句柄 ----
    HWND hwnd_ = nullptr;
    HWND hwndToolbar_ = nullptr;
    HWND hwndTitle_ = nullptr;            // 顶部粗体版本标题
    HWND hwndBtnSettings_ = nullptr;
    HWND hwndStatusBar_ = nullptr;
    HWND hwndFrpcLabel_ = nullptr;
    HWND hwndFrpsLabel_ = nullptr;
    HWND hwndFrpcStatus_ = nullptr;
    HWND hwndFrpsStatus_ = nullptr;
    HWND hwndBtnFrpcStart_ = nullptr;
    HWND hwndBtnFrpcStop_ = nullptr;
    HWND hwndBtnFrpsStart_ = nullptr;
    HWND hwndBtnFrpsStop_ = nullptr;
    HWND hwndFrpcCard_ = nullptr;         // frpc 配置摘要卡片
    HWND hwndFrpsCard_ = nullptr;         // frps 配置摘要卡片
    HWND hwndDivider_ = nullptr;
    HWND hwndChkAutoStart_ = nullptr;

    // ---- GDI 资源 ----
    HFONT hFont_ = nullptr;               // 普通控件字体 (14pt)
    HFONT hBoldFont_ = nullptr;           // 加粗字体 (16pt)，用于版本标题

    // ---- 业务对象 ----
    Settings settings_;
    FrpProcess frpc_{ FrpMode::Client };
    FrpProcess frps_{ FrpMode::Server };
    TrayIcon* tray_ = nullptr;

    // ---- 状态 ----
    bool exiting_ = false;

    // 配置文件时间戳缓存，用于检测外部修改
    FILETIME frpcLastWrite_ = {};
    FILETIME frpsLastWrite_ = {};

    // 可执行文件时间戳缓存，用于检测版本更新
    FILETIME frpcExeLastWrite_ = {};
    FILETIME frpsExeLastWrite_ = {};

    // 布局常量
    static const int TOOLBAR_H = 110;
    static const int STATUS_H = 24;
    static const int BTN_W = 80;
    static const int BTN_H = 30;

    // 托盘气泡防抖计时
    ULONGLONG lastBalloonTick_ = 0;
};