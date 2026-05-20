// Window.h - 主窗口
#pragma once
#include <windows.h>
#include "FrpProcess.h"
#include "TrayIcon.h"       // 包含 WM_TRAYICON 定义
#include "TomlHelper.h"
#include "Settings.h"

// 自定义消息定义
static const int WM_OUTPUT = WM_USER + 2;
static const int WM_PROCESS_EXIT = WM_USER + 3;
static const int WM_TRAY_EXIT = WM_USER + 4;
static const int WM_REFRESH_LANG = WM_USER + 5;
static const int WM_TASKBARCREATED = WM_USER + 6;

// 主窗口类，实现进程回调与托盘回调接口
class MainWindow : public IFrpProcessCallback, public ITrayCallback {
public:
    MainWindow();
    ~MainWindow();

    bool RegisterClass();
    HWND CreateWindow_();
    void SetFrpRoot(const std::wstring& path);
    void SetHideMode(bool hide) { hideMode_ = hide; }

    // 停止所有进程并更新控件状态（供 Settings 调用）
    void StopAllProcesses();
    HWND GetHwnd() const { return hwnd_; }

    // 多语言
    enum Lang { LangZh, LangEn };
    Lang GetLang() const { return currentLang_; }
    void ToggleLang();
    void ApplyLang();  // 根据 currentLang_ 更新所有控件文字

private:
    void FlushPendingMessages();
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
    bool hideMode_ = false;
    bool trayHidden_ = false;

    // 配置文件时间戳缓存，用于检测外部修改
    FILETIME frpcLastWrite_ = {};
    FILETIME frpsLastWrite_ = {};

    // 可执行文件时间戳缓存，用于检测版本更新
    FILETIME frpcExeLastWrite_ = {};
    FILETIME frpsExeLastWrite_ = {};

    // 版本号缓存，避免频繁调用 frpc -v
    std::wstring cachedFrpVer_;
    bool versionDirty_ = true;

    // DetectionInfo 缓存，避免频繁磁盘 IO
    Settings::DetectionInfo cachedInfo_;
    bool infoDirty_ = true;

    // 单实例互斥体
    HANDLE hSingletonMutex_ = nullptr;

    // TaskbarCreated 消息注册 ID
    UINT wmTaskbarCreated_ = 0;

    // 窗口类背景刷（需手动释放）
    HBRUSH hBgBrush_ = nullptr;

    // 布局常量
    static const int TOOLBAR_H = 110;
    static const int STATUS_H = 24;
    static const int BTN_W = 80;
    static const int BTN_H = 30;

    // 托盘气泡防抖计时
    ULONGLONG lastBalloonTick_ = 0;

    // 多语言状态
    Lang currentLang_ = LangZh;

    // RefreshSummary 的 DetectionInfo 读取方法
    const Settings::DetectionInfo& GetCachedInfo();
};