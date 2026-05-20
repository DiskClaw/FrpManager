// Settings.h - 设置页（frp 路径 + 开机自启 + 清除日志）
#pragma once
#include <windows.h>
#include <string>

class MainWindow;   // 前置声明

class Settings {
public:
    struct DetectionInfo {
        std::wstring root;
        std::wstring frpcExe;
        std::wstring frpsExe;
        std::wstring frpcConfig;
        std::wstring frpsConfig;
        bool HasAnyExecutable() const;
    };

    Settings();
    ~Settings();

    std::wstring GetFrpRoot() const;
    void SetFrpRoot(const std::wstring& path);

    bool GetAutoStart() const;
    void SetAutoStart(bool enable);

    std::wstring GetFrpcExe() const;
    std::wstring GetFrpsExe() const;
    std::wstring GetFrpcConfig() const;
    std::wstring GetFrpsConfig() const;

    DetectionInfo Detect() const;
    static std::wstring AutoDetectFrpRoot();
    bool ShowDialog(HWND owner, bool zh);
    HWND GetDialogHwnd() const { return hwndDialog_; }

    // 初始化清理（供外部按需调用）
    void SetMainWindow(MainWindow* main) { m_pMain = main; }
    void InitCleanup();

private:
    void DeleteFrpServices();
    static const wchar_t* REG_KEY;
    static const wchar_t* VAL_FRP_ROOT;
    static const wchar_t* VAL_AUTO_START;
    HWND hwndDialog_ = nullptr;
    MainWindow* m_pMain = nullptr;
};