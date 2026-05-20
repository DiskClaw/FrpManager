/**
 * Window.cpp - 主窗口实现
 *
 * 窗口创建、进程生命周期管理、配置摘要、文件监控、版本检测
 */
#include "Window.h"
#include <shellapi.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <TlHelp32.h>
#include <algorithm>
#include <vector>
#include <string>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {
    // 窗口类名
    constexpr const wchar_t CLASS_NAME[] = L"FrpManagerWindow";

    // 异步事件数据结构
    struct OutputEvent { FrpMode mode; std::string text; };
    struct ExitEvent { FrpMode mode; DWORD exitCode; };

    // 创建静态文本控件
    HWND CreateText(HWND parent, const wchar_t* text, DWORD style, int id = 0) {
        return CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, parent,
            (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
    }

    // 将可选数值转换为宽字符串，失败显示"未设置"或"Not Set"
    std::wstring ToWString(const std::optional<int>& val, bool zh) {
        return val.has_value() ? std::to_wstring(*val) : (zh ? L"未设置" : L"Not Set");
    }

    // 限制编辑框行数，超出则从顶部删除
    void LimitEditLines(HWND hEdit, int maxLines) {
        int lineCount = static_cast<int>(SendMessageW(hEdit, EM_GETLINECOUNT, 0, 0));
        if (lineCount <= maxLines) return;
        int removeLines = lineCount - maxLines;
        int idx = static_cast<int>(SendMessageW(hEdit, EM_LINEINDEX, removeLines, 0));
        SendMessageW(hEdit, EM_SETSEL, 0, idx);
        SendMessageW(hEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
        SendMessageW(hEdit, EM_SETSEL, -1, -1);
    }

    // 检查当前进程是否以管理员权限运行
    bool IsUserAdmin() {
        BOOL isElevated = FALSE;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION elevation{};
            DWORD size = sizeof(elevation);
            if (GetTokenInformation(token, TokenElevation, &elevation, size, &size))
                isElevated = elevation.TokenIsElevated;
            CloseHandle(token);
        }
        return isElevated != FALSE;
    }

    // 通过 PID 获取进程完整镜像路径
    std::wstring GetProcessImagePath(DWORD pid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return L"";
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (!QueryFullProcessImageNameW(hProc, 0, path, &size)) {
            CloseHandle(hProc);
            return L"";
        }
        CloseHandle(hProc);
        return std::wstring(path);
    }

    // 判断指定路径的可执行文件是否正在运行
    bool IsExactExeRunning(const std::wstring& exePath) {
        if (exePath.empty()) return false;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"frpc.exe") == 0 ||
                    _wcsicmp(pe.szExeFile, L"frps.exe") == 0) {
                    std::wstring procPath = GetProcessImagePath(pe.th32ProcessID);
                    if (_wcsicmp(procPath.c_str(), exePath.c_str()) == 0) {
                        CloseHandle(hSnap);
                        return true;
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return false;
    }

    // 终止所有与指定路径匹配的 frpc/frps 进程，返回终止数目
    int KillSamePathExe(const std::wstring& exePath) {
        if (exePath.empty()) return 0;
        int count = 0;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return 0;
        PROCESSENTRY32W pe{ sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"frpc.exe") == 0 ||
                    _wcsicmp(pe.szExeFile, L"frps.exe") == 0) {
                    std::wstring procPath = GetProcessImagePath(pe.th32ProcessID);
                    if (_wcsicmp(procPath.c_str(), exePath.c_str()) == 0) {
                        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                        if (hProc) {
                            TerminateProcess(hProc, 1);
                            CloseHandle(hProc);
                            ++count;
                        }
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
        return count;
    }

    // 通过执行 "exe -v" 获取版本字符串
    std::wstring GetExeVersionByCommand(const std::wstring& exePath) {
        if (exePath.empty() || !PathFileExistsW(exePath.c_str()))
            return L"";

        // 构建命令行
        std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" -v";
        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        // 创建匿名管道捕获输出
        HANDLE hRead = nullptr, hWrite = nullptr;
        SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        if (!CreatePipe(&hRead, &hWrite, &sa, 0))
            return L"";

        STARTUPINFOW si{ sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(hWrite);
        if (!ok) {
            CloseHandle(hRead);
            return L"";
        }

        // 读取输出（最长 4KB）
        std::string output;
        char buf[1024];
        DWORD read = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
            buf[read] = '\0';
            output += buf;
            if (output.size() > 4096) break;
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hRead);

        // 去除换行符
        size_t pos = output.find('\n');
        if (pos != std::string::npos) output.resize(pos);
        pos = output.find('\r');
        if (pos != std::string::npos) output.resize(pos);

        // 转换为 Unicode
        int len = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
        if (len <= 0) return L"";
        std::wstring wver(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, &wver[0], len);
        wver.resize(len - 1);
        return wver;
    }

    // 获取文件最后修改时间
    bool GetFileLastWrite(const std::wstring& path, FILETIME& ft) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        GetFileTime(hFile, nullptr, nullptr, &ft);
        CloseHandle(hFile);
        return true;
    }

    // 应用现代 DWM 风格（圆角 + 深色标题栏），兼容 Win8–Win11
    void ApplyModernStyle(HWND hwnd) {
        HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
        if (!hDwm) return;
        using DwmSetAttr = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto pfn = (DwmSetAttr)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (pfn) {
            DWORD c = 2; pfn(hwnd, 33, &c, sizeof(c)); // 圆角（Win11）
        }
        FreeLibrary(hDwm);
    }
    // 自动检测系统语言：中文区返回 true，否则 false
    bool IsChineseLocale() {
        LANGID langId = GetSystemDefaultUILanguage();
        WORD primLang = PRIMARYLANGID(langId);
        return primLang == LANG_CHINESE;
    }

    // 多语言字符串索引
    enum StrId {
        S_ADMIN, S_NON_ADMIN, S_STOPPED, S_MISS_FRPC, S_MISS_FRPS,
        S_RUNNING, S_READY, S_NO_CONFIG, S_LABEL_AUTO_START,
        S_EXIT, S_SHOW, S_SWITCH_TO_EN, S_SWITCH_TO_ZH,
        S_BTN_SETTINGS, S_BTN_START, S_BTN_STOP,
        S_TRAY_TOOLTIP,
        S_HDR_SERVER_ADDR, S_HDR_LOG_FILE, S_HDR_LOG_LEVEL, S_HDR_LOG_DAYS,
        S_HDR_LOG_PORT, S_HDR_HTTP_PORT, S_HDR_HTTPS_PORT,
        S_HDR_DASH_ADDR, S_HDR_DASH_USER, S_HDR_DASH_PWD,
        S_LOCAL, S_REMOTE,
        S_BALLOON_NOTICE, S_BALLOON_CLEANED, S_BALLOON_EXIT_ERROR,
        S_MSG_START_FAILED, S_MSG_ERROR, S_MSG_FRPC_RUNNING, S_MSG_FRPS_RUNNING,
        S_MSG_FRPC_STARTED, S_MSG_FRPS_STARTED, S_MSG_FRPC_STOPPED, S_MSG_FRPS_STOPPED,
        S_MSG_FRPC_EXIT, S_MSG_FRPS_EXIT, S_MSG_CLEANUP,
        S_MSG_FRPC_ALREADY_RUNNING, S_MSG_FRPS_ALREADY_RUNNING,
        S_MSG_FRPC_STILL_RUNNING, S_MSG_FRPS_STILL_RUNNING,
        S_MSG_SET_ROOT_FIRST, S_AUTO_START_ENABLED, S_AUTO_START_DISABLED,
        S_SETTINGS_UPDATED,
        S_COUNT
    };

    // 中英文字符串表（STR[id][0]=中文，STR[id][1]=英文）
    const wchar_t* STR[S_COUNT][2] = {
        /* S_ADMIN */           { L" [管理员]",           L" [Admin]" },
        /* S_NON_ADMIN */       { L" [非管理员]",         L" [Non-Admin]" },
        /* S_STOPPED */         { L"已停止",              L"Stopped" },
        /* S_MISS_FRPC */       { L"缺少 frpc.exe",       L"frpc.exe missing" },
        /* S_MISS_FRPS */       { L"缺少 frps.exe",       L"frps.exe missing" },
        /* S_RUNNING */         { L"运行中",              L"Running" },
        /* S_READY */           { L"就绪",                L"Ready" },
        /* S_NO_CONFIG */       { L"未找到配置文件。",     L"Config file not found." },
        /* S_LABEL_AUTO_START */{ L"开机启动",            L"Auto-Start" },
        /* S_EXIT */            { L"退出",                L"Exit" },
        /* S_SHOW */            { L"显示窗口",             L"Show Window" },
        /* S_SWITCH_TO_EN */    { L"EN",                  L"EN" },
        /* S_SWITCH_TO_ZH */    { L"中文",                L"中文" },
        /* S_BTN_SETTINGS */    { L"设置",                L"Settings" },
        /* S_BTN_START */       { L"启动",                L"Start" },
        /* S_BTN_STOP */        { L"停止",                L"Stop" },
        /* S_TRAY_TOOLTIP */    { L"FRP 管理器",          L"FRP Manager" },
        /* S_HDR_SERVER_ADDR */ { L"服务端地址",           L"Server" },
        /* S_HDR_LOG_FILE */   { L"日志文件",             L"Log" },
        /* S_HDR_LOG_LEVEL */   { L"日志等级",             L"Log Level" },
        /* S_HDR_LOG_DAYS */   { L"日志保留",             L"Retention" },
        /* S_HDR_LOG_PORT */   { L"监听端口",             L"Bind Port" },
        /* S_HDR_HTTP_PORT */   { L"HTTP 端口",           L"HTTP Port" },
        /* S_HDR_HTTPS_PORT */ { L"HTTPS 端口",           L"HTTPS Port" },
        /* S_HDR_DASH_ADDR */  { L"管理地址",             L"Dash" },
        /* S_HDR_DASH_USER */  { L"账户名",               L"User" },
        /* S_HDR_DASH_PWD */   { L"管理密码",             L"Password" },
        /* S_LOCAL */           { L"本地:",                L"Local:" },
        /* S_REMOTE */          { L" → 远程:",             L" -> Remote:" },
        /* S_BALLOON_NOTICE */  { L"提示",                L"Notice" },
        /* S_BALLOON_CLEANED */ { L"清理完成",            L"Cleaned" },
        /* S_BALLOON_EXIT_ERROR */  { L"异常退出",         L"Error" },
        /* S_MSG_START_FAILED */ { L"请先在设置中配置 frp 目录", L"Please configure FRP root in Settings first" },
        /* S_MSG_ERROR */       { L"错误",                L"Error" },
        /* S_MSG_FRPC_RUNNING */ { L"frpc 正在运行。",    L"frpc is already running." },
        /* S_MSG_FRPS_RUNNING */ { L"frps 正在运行。",    L"frps is already running." },
        /* S_MSG_FRPC_STARTED */ { L"frpc 已启动",       L"frpc started" },
        /* S_MSG_FRPS_STARTED */ { L"frps 已启动",       L"frps started" },
        /* S_MSG_FRPC_STOPPED */ { L"frpc 已停止",        L"frpc stopped" },
        /* S_MSG_FRPS_STOPPED */ { L"frps 已停止",        L"frps stopped" },
        /* S_MSG_FRPC_EXIT */   { L"frpc 已退出，代码 ", L"frpc exited, code " },
        /* S_MSG_FRPS_EXIT */   { L"frps 已退出，代码 ", L"frps exited, code " },
        /* S_MSG_CLEANUP */     { L"，清理了 ",           L", cleaned " },
        /* S_MSG_FRPC_ALREADY_RUNNING */ { L"frpc 已在运行中，请先停止后再启动。", L"frpc is already running. Stop it first." },
        /* S_MSG_FRPS_ALREADY_RUNNING */ { L"frps 已在运行中，请先停止后再启动。", L"frps is already running. Stop it first." },
        /* S_MSG_FRPC_STILL_RUNNING */ { L"frpc 仍在运行，无法启动。", L"frpc is still running." },
        /* S_MSG_FRPS_STILL_RUNNING */ { L"frps 仍在运行，无法启动。", L"frps is still running." },
        /* S_MSG_SET_ROOT_FIRST */ { L"请先在设置中配置 frp 目录。", L"Please set FRP root in Settings first." },
        /* S_AUTO_START_ENABLED */ { L"已开启开机自启",    L"Auto-start enabled" },
        /* S_AUTO_START_DISABLED */ { L"已关闭开机自启",   L"Auto-start disabled" },
        /* S_SETTINGS_UPDATED */ { L"设置已更新",         L"Settings updated" },
    };

    inline const wchar_t* Ls(StrId id, bool zh) { return STR[id][zh ? 0 : 1]; }
} // anonymous namespace

MainWindow::MainWindow() {
    frpc_.SetCallback(this);
    frps_.SetCallback(this);
    settings_.SetMainWindow(this);
    currentLang_ = IsChineseLocale() ? LangZh : LangEn;
}

MainWindow::~MainWindow() {
    frpc_.Stop();
    frps_.Stop();
    delete tray_;
    if (hFont_)     DeleteObject(hFont_);
    if (hBoldFont_) DeleteObject(hBoldFont_);
    if (hBgBrush_)  DeleteObject(hBgBrush_);
    if (hSingletonMutex_) { ReleaseMutex(hSingletonMutex_); CloseHandle(hSingletonMutex_); }
}

// 注册窗口类（图标、光标、背景刷、窗口过程）
bool MainWindow::RegisterClass() {
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = MainWindow::WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (hBgBrush_ = CreateSolidBrush(RGB(240, 240, 240)));
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassExW(&wc) != 0;
}

// 创建窗口、初始化所有子控件、设置字体、启动托盘图标
HWND MainWindow::CreateWindow_() {
    // 单实例检查：如果已有实例运行则激活它并退出
    hSingletonMutex_ = CreateMutexW(nullptr, TRUE, L"FrpManager_Singleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 查找已有窗口并前置
        HWND existing = FindWindowW(CLASS_NAME, nullptr);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (hSingletonMutex_) { CloseHandle(hSingletonMutex_); hSingletonMutex_ = nullptr; }
        return nullptr;
    }

    // 注册 TaskbarCreated 消息，用于 explorer 崩溃后重建托盘图标
    wmTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int winW = 420, winH = 520;

    std::wstring titleBar = L"FRP 管理器";
    titleBar += IsUserAdmin() ? L" [管理员]" : L" [非管理员]";

    hwnd_ = CreateWindowExW(0, CLASS_NAME, titleBar.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        (screenW - winW) / 2, (screenH - winH) / 2,
        winW, winH,
        nullptr, nullptr, GetModuleHandle(nullptr), this);
    if (!hwnd_) return nullptr;
    ApplyModernStyle(hwnd_);

    // 创建字体
    hFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_CHARSET, L"Microsoft YaHei UI");
    hBoldFont_ = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_CHARSET, L"Microsoft YaHei UI");

    // 初始化根目录并获取版本号用于标题
    {
        std::wstring root = settings_.GetFrpRoot();
        if (root.empty()) {
            root = Settings::AutoDetectFrpRoot();
            if (!root.empty()) settings_.SetFrpRoot(root);
        }
    }
    infoDirty_ = true;
    cachedInfo_ = GetCachedInfo();
    std::wstring exePath = cachedInfo_.frpcExe.empty() ? cachedInfo_.frpsExe : cachedInfo_.frpcExe;
    cachedFrpVer_ = GetExeVersionByCommand(exePath);
    versionDirty_ = false;
    if (cachedFrpVer_.empty()) cachedFrpVer_ = L"未知";
    std::wstring versionTitle = (currentLang_ == LangZh ? L"FRP 版本：" : L"FRP Version: ") + cachedFrpVer_;

    // 顶部区域
    hwndTitle_ = CreateText(hwnd_, versionTitle.c_str(), SS_LEFT);
    hwndBtnSettings_ = CreateWindowExW(0, L"BUTTON", L"设置",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, BTN_W, BTN_H,
        hwnd_, (HMENU)103, GetModuleHandle(nullptr), nullptr);

    // frpc 控制行
    hwndFrpcLabel_ = CreateText(hwnd_, L"frpc", SS_LEFT);
    hwndFrpcStatus_ = CreateText(hwnd_, L"未启动", SS_LEFT);
    hwndBtnFrpcStart_ = CreateWindowExW(0, L"BUTTON", L"启动",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, BTN_W, BTN_H,
        hwnd_, (HMENU)101, GetModuleHandle(nullptr), nullptr);
    hwndBtnFrpcStop_ = CreateWindowExW(0, L"BUTTON", L"停止",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, BTN_W, BTN_H,
        hwnd_, (HMENU)102, GetModuleHandle(nullptr), nullptr);

    // frps 控制行
    hwndFrpsLabel_ = CreateText(hwnd_, L"frps", SS_LEFT);
    hwndFrpsStatus_ = CreateText(hwnd_, L"未启动", SS_LEFT);
    hwndBtnFrpsStart_ = CreateWindowExW(0, L"BUTTON", L"启动",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, BTN_W, BTN_H,
        hwnd_, (HMENU)104, GetModuleHandle(nullptr), nullptr);
    hwndBtnFrpsStop_ = CreateWindowExW(0, L"BUTTON", L"停止",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, BTN_W, BTN_H,
        hwnd_, (HMENU)105, GetModuleHandle(nullptr), nullptr);

    // 摘要卡片（只读编辑框）
    hwndFrpcCard_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
        0, 0, 0, 0, hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hwndFrpsCard_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
        0, 0, 0, 0, hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);

    // 分隔线、状态栏、自启勾选框
    hwndDivider_ = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_SUNKEN, 0, 0, 2, 0,
        hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    hwndStatusBar_ = CreateText(hwnd_, L"就绪", SS_LEFT);
    hwndChkAutoStart_ = CreateWindowExW(0, L"BUTTON", L"开机启动",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 95, 20,
        hwnd_, (HMENU)106, GetModuleHandle(nullptr), nullptr);
    if (settings_.GetAutoStart())
        SendMessageW(hwndChkAutoStart_, BM_SETCHECK, BST_CHECKED, 0);

    // 设置字体（标题用加粗，其余用常规）
    for (HWND child : { hwndBtnSettings_,
        hwndFrpcLabel_, hwndFrpcStatus_, hwndBtnFrpcStart_, hwndBtnFrpcStop_,
        hwndFrpsLabel_, hwndFrpsStatus_, hwndBtnFrpsStart_, hwndBtnFrpsStop_,
        hwndFrpcCard_, hwndFrpsCard_, hwndDivider_, hwndStatusBar_,
        hwndChkAutoStart_ })
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(hFont_), TRUE);
    SendMessageW(hwndTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(hBoldFont_), TRUE);

    // 确保根目录已设置，必要时弹出设置对话框
    std::wstring root = settings_.GetFrpRoot();
    if (root.empty()) {
        root = Settings::AutoDetectFrpRoot();
        if (!root.empty()) settings_.SetFrpRoot(root);
    }
    if (root.empty()) ShowSettingsDialog(); else { SetFrpRoot(root); infoDirty_ = true; }

    // 初始化文件时间戳缓存
    GetFileLastWrite(cachedInfo_.frpcExe, frpcExeLastWrite_);
    GetFileLastWrite(cachedInfo_.frpsExe, frpsExeLastWrite_);

    for (auto mode : { FrpMode::Client, FrpMode::Server }) {
        std::wstring cfg = ConfigPathForMode(mode);
        FILETIME& ft = (mode == FrpMode::Client) ? frpcLastWrite_ : frpsLastWrite_;
        if (!cfg.empty()) {
            HANDLE hFile = CreateFileW(cfg.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                GetFileTime(hFile, nullptr, nullptr, &ft);
                CloseHandle(hFile);
            }
        }
    }

    // 托盘与定时器
    tray_ = new TrayIcon();
    tray_->Create(hwnd_, this, L"FRP 管理器");
    SetTimer(hwnd_, 1, 2000, nullptr);  // 2 秒周期检查文件变化

    // 应用系统语言并刷新界面
    ApplyLang();

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    UpdateProcessControls();
    RefreshSummary();

    return hwnd_;
}

// 设置 FRP 根目录，刷新控件状态和摘要文本，更新版本标题
void MainWindow::SetFrpRoot(const std::wstring& path) {
    settings_.SetFrpRoot(path);
    infoDirty_ = true;
    cachedInfo_ = GetCachedInfo();
    UpdateProcessControls();
    RefreshSummary();

    // 更新版本号缓存
    std::wstring exePath = cachedInfo_.frpcExe.empty() ? cachedInfo_.frpsExe : cachedInfo_.frpcExe;
    cachedFrpVer_ = GetExeVersionByCommand(exePath);
    versionDirty_ = false;
    if (cachedFrpVer_.empty()) cachedFrpVer_ = currentLang_ == LangZh ? L"未知" : L"Unknown";
    SetWindowTextW(hwndTitle_, ((currentLang_ == LangZh ? L"FRP 版本：" : L"FRP Version: ") + cachedFrpVer_).c_str());

    // 更新可执行文件时间戳缓存
    GetFileLastWrite(cachedInfo_.frpcExe, frpcExeLastWrite_);
    GetFileLastWrite(cachedInfo_.frpsExe, frpsExeLastWrite_);
}

// 刷新 frpc/frps 摘要卡片文本，根据内容行数自动显示或隐藏滚动条
void MainWindow::RefreshSummary() {
    for (auto mode : { FrpMode::Client, FrpMode::Server }) {
        std::wstring cfg = ConfigPathForMode(mode);
        HWND hEdit = (mode == FrpMode::Client) ? hwndFrpcCard_ : hwndFrpsCard_;

        // 首行：配置文件名
        std::wstring text;
        if (cfg.empty()) {
            text = Ls(S_NO_CONFIG, currentLang_ == LangZh);
        }
        else {
            const wchar_t* name = wcsrchr(cfg.c_str(), L'\\');
            text = name ? std::wstring(name + 1) : cfg;
        }
        text += std::wstring(L"\r\n") + BuildSummaryText(mode);
        SetWindowTextW(hEdit, text.c_str());

        // 根据内容行数决定是否显示垂直滚动条
        int lines = 1;
        for (wchar_t c : text) if (c == L'\n') ++lines;
        LONG style = GetWindowLongW(hEdit, GWL_STYLE);
        if (lines > 5) {
            SetWindowLongW(hEdit, GWL_STYLE, style | WS_VSCROLL);
        }
        else {
            SetWindowLongW(hEdit, GWL_STYLE, style & ~WS_VSCROLL);
        }
        SetWindowPos(hEdit, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

// ---- 定时检查配置文件及 exe 版本变化 ----
void MainWindow::CheckConfigChanges() {
    bool changed = false;
    if (infoDirty_) { cachedInfo_ = GetCachedInfo(); infoDirty_ = false; }
    auto& info = cachedInfo_;

    // 检查可执行文件修改：只在时间戳变化时才重新跑 frpc -v
    auto checkExeVersion = [&](const std::wstring& exePath, FILETIME& cachedTime) {
        FILETIME currentTime{};
        bool fileExists = GetFileLastWrite(exePath, currentTime);

        bool exeChanged = false;
        if (fileExists) {
            if (CompareFileTime(&currentTime, &cachedTime) != 0)
                exeChanged = true;
        }
        else {
            FILETIME zero{};
            if (CompareFileTime(&cachedTime, &zero) != 0) {
                exeChanged = true;
                currentTime = {};
            }
        }

        if (exeChanged) {
            cachedTime = currentTime;
            versionDirty_ = true;
        }
        };

    checkExeVersion(info.frpcExe, frpcExeLastWrite_);
    checkExeVersion(info.frpsExe, frpsExeLastWrite_);

    // 只在标记脏时才跑一次版本查询
    if (versionDirty_) {
        std::wstring exePath = info.frpcExe.empty() ? info.frpsExe : info.frpcExe;
        cachedFrpVer_ = GetExeVersionByCommand(exePath);
        if (cachedFrpVer_.empty()) cachedFrpVer_ = currentLang_ == LangZh ? L"未知" : L"Unknown";
        bool zh = (currentLang_ == LangZh);
        SetWindowTextW(hwndTitle_, ((zh ? L"FRP 版本：" : L"FRP Version: ") + cachedFrpVer_).c_str());
        versionDirty_ = false;
    }

    // 检查配置文件变化
    for (auto mode : { FrpMode::Client, FrpMode::Server }) {
        std::wstring cfg = ConfigPathForMode(mode);
        FILETIME& last = (mode == FrpMode::Client) ? frpcLastWrite_ : frpsLastWrite_;
        if (cfg.empty() || !PathFileExistsW(cfg.c_str())) {
            ZeroMemory(&last, sizeof(last));
            changed = true;
            continue;
        }
        HANDLE hFile = CreateFileW(cfg.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;
        FILETIME cfgFt{};
        GetFileTime(hFile, nullptr, nullptr, &cfgFt);
        CloseHandle(hFile);
        if (CompareFileTime(&cfgFt, &last) != 0) {
            last = cfgFt;
            changed = true;
        }
    }

    if (changed) RefreshSummary();
}

// ---- 根据实际文件状态更新进程控制按钮 ----
void MainWindow::UpdateProcessControls() {
    if (infoDirty_) { cachedInfo_ = GetCachedInfo(); infoDirty_ = false; }
    auto& info = cachedInfo_;
    bool frpcExeExists = !info.frpcExe.empty() && PathFileExistsW(info.frpcExe.c_str());
    bool frpsExeExists = !info.frpsExe.empty() && PathFileExistsW(info.frpsExe.c_str());
    bool frpcCfgExists = !info.frpcConfig.empty() && PathFileExistsW(info.frpcConfig.c_str());
    bool frpsCfgExists = !info.frpsConfig.empty() && PathFileExistsW(info.frpsConfig.c_str());

    bool frpcRunning = frpc_.IsRunning() || (frpcExeExists && IsExactExeRunning(info.frpcExe));
    bool frpsRunning = frps_.IsRunning() || (frpsExeExists && IsExactExeRunning(info.frpsExe));

    bool zh = (currentLang_ == LangZh);
    SetWindowTextW(hwndFrpcStatus_, frpcRunning ? Ls(S_RUNNING, zh) :
        (frpcExeExists ? Ls(S_STOPPED, zh) : Ls(S_MISS_FRPC, zh)));
    SetWindowTextW(hwndFrpsStatus_, frpsRunning ? Ls(S_RUNNING, zh) :
        (frpsExeExists ? Ls(S_STOPPED, zh) : Ls(S_MISS_FRPS, zh)));
    EnableWindow(hwndBtnFrpcStart_, !frpcRunning && frpcExeExists && frpcCfgExists);
    EnableWindow(hwndBtnFrpcStop_, frpcRunning);
    EnableWindow(hwndBtnFrpsStart_, !frpsRunning && frpsExeExists && frpsCfgExists);
    EnableWindow(hwndBtnFrpsStop_, frpsRunning);

    if (settings_.GetFrpRoot().empty()) {
        SetWindowTextW(hwndStatusBar_, L"请先在设置中选择 frp 根目录");
    }
}

// 响应 WM_SIZE，重新计算并布局所有子控件位置
void MainWindow::OnSize(int w, int h) {
    constexpr int PAD = 20;
    const int contentWidth = (w > 380) ? 380 : (w - PAD * 2);
    const int left = (w - contentWidth) / 2;
    const int gx = left + 20;
    const int rightEdge = gx + contentWidth - 40;

    SetWindowPos(hwndToolbar_, nullptr, left, PAD, contentWidth, TOOLBAR_H, SWP_NOZORDER);

    const int row1Y = PAD + 6;
    const int row2Y = PAD + 40;
    const int row3Y = PAD + 74;
    const int gap = 8;
    const int stopBtnX = rightEdge - BTN_W;
    const int startBtnX = stopBtnX - BTN_W - gap;
    const int labelW = startBtnX - gx;

    // 标题行（高度加大以适应粗体）
    SetWindowPos(hwndTitle_, nullptr, gx, row1Y, stopBtnX - gx - gap, 28, SWP_NOZORDER);
    SetWindowPos(hwndBtnSettings_, nullptr, stopBtnX, row1Y, BTN_W, BTN_H, SWP_NOZORDER);

    // frpc 行
    SetWindowPos(hwndFrpcLabel_, nullptr, gx, row2Y + 6, 40, 20, SWP_NOZORDER);
    SetWindowPos(hwndFrpcStatus_, nullptr, gx + 48, row2Y + 6, labelW - 48, 20, SWP_NOZORDER);
    SetWindowPos(hwndBtnFrpcStart_, nullptr, startBtnX, row2Y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(hwndBtnFrpcStop_, nullptr, stopBtnX, row2Y, BTN_W, BTN_H, SWP_NOZORDER);

    // frps 行
    SetWindowPos(hwndFrpsLabel_, nullptr, gx, row3Y + 6, 40, 20, SWP_NOZORDER);
    SetWindowPos(hwndFrpsStatus_, nullptr, gx + 48, row3Y + 6, labelW - 48, 20, SWP_NOZORDER);
    SetWindowPos(hwndBtnFrpsStart_, nullptr, startBtnX, row3Y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(hwndBtnFrpsStop_, nullptr, stopBtnX, row3Y, BTN_W, BTN_H, SWP_NOZORDER);

    // 摘要卡片
    const int cardTop = PAD + TOOLBAR_H + 12;
    const int totalCardH = h - cardTop - PAD - STATUS_H - 8;
    const int cardH = (totalCardH - 8) / 2;
    SetWindowPos(hwndFrpcCard_, nullptr, left + 20, cardTop, contentWidth - 40, cardH, SWP_NOZORDER);
    SetWindowPos(hwndFrpsCard_, nullptr, left + 20, cardTop + cardH + 8, contentWidth - 40, cardH, SWP_NOZORDER);

    // 状态栏与自启复选框
    ShowWindow(hwndDivider_, SW_HIDE);
    const int autoX = rightEdge - 95 - 6;
    SetWindowPos(hwndStatusBar_, nullptr, gx, h - PAD - STATUS_H, autoX - gx - 4, STATUS_H, SWP_NOZORDER);
    SetWindowPos(hwndChkAutoStart_, nullptr, autoX, h - PAD - STATUS_H, 95, STATUS_H, SWP_NOZORDER);
}

// ---- 命令处理（按钮点击） ----
void MainWindow::OnCommand(int id) {
    switch (id) {
    case 101: StartProcess(FrpMode::Client);  break;
    case 102: StopProcess(FrpMode::Client);   break;
    case 103: ShowSettingsDialog();           break;
    case 104: StartProcess(FrpMode::Server);  break;
    case 105: StopProcess(FrpMode::Server);   break;
    case 106: {
        bool zh = (currentLang_ == LangZh);
        settings_.SetAutoStart(
            SendMessageW(hwndChkAutoStart_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        SetWindowTextW(hwndStatusBar_,
            settings_.GetAutoStart() ? Ls(S_AUTO_START_ENABLED, zh) : Ls(S_AUTO_START_DISABLED, zh));
        break;
    }
    }
}

// ---- 显示设置对话框 ----
bool MainWindow::ShowSettingsDialog() {
    bool zh = (currentLang_ == LangZh);
    bool ok = settings_.ShowDialog(hwnd_, zh);
    if (ok) {
        UpdateProcessControls();
        RefreshSummary();
        SetWindowTextW(hwndStatusBar_, Ls(S_SETTINGS_UPDATED, zh));
    }
    return ok;
}

// ---- 启动指定模式进程 ----
void MainWindow::StartProcess(FrpMode mode) {
    bool zh = (currentLang_ == LangZh);
    if (infoDirty_) { cachedInfo_ = GetCachedInfo(); infoDirty_ = false; }
    const std::wstring exePath = (mode == FrpMode::Client)
        ? cachedInfo_.frpcExe : cachedInfo_.frpsExe;
    const std::wstring cfgPath = ConfigPathForMode(mode);
    const std::wstring rootDir = settings_.GetFrpRoot();

    if (exePath.empty() || cfgPath.empty() || rootDir.empty()) {
        MessageBoxW(hwnd_, Ls(S_MSG_START_FAILED, zh), Ls(S_MSG_ERROR, zh), MB_ICONERROR);
        return;
    }

    if (IsExactExeRunning(exePath)) {
        const wchar_t* msg = mode == FrpMode::Client ? Ls(S_MSG_FRPC_ALREADY_RUNNING, zh) : Ls(S_MSG_FRPS_ALREADY_RUNNING, zh);
        SetWindowTextW(hwndStatusBar_, msg);
        if (tray_) tray_->ShowBalloon(Ls(S_BALLOON_NOTICE, zh), msg, 2000);
        UpdateProcessControls();
        return;
    }

    if (ProcessForMode(mode).IsRunning()) {
        const wchar_t* msg = mode == FrpMode::Client ? Ls(S_MSG_FRPC_STILL_RUNNING, zh) : Ls(S_MSG_FRPS_STILL_RUNNING, zh);
        SetWindowTextW(hwndStatusBar_, msg);
        return;
    }

    if (ProcessForMode(mode).Start(exePath, cfgPath, rootDir)) {
        const wchar_t* msg = mode == FrpMode::Client ? Ls(S_MSG_FRPC_STARTED, zh) : Ls(S_MSG_FRPS_STARTED, zh);
        SetWindowTextW(hwndStatusBar_, msg);
    }
    else {
        std::wstring txt = std::wstring(L"Start ") + ModeName(mode) + (zh ? L" 失败" : L" failed");
        MessageBoxW(hwnd_, txt.c_str(), Ls(S_MSG_ERROR, zh), MB_ICONERROR);
    }

    UpdateProcessControls();
}

// 停止指定模式进程，清理同名残留进程并更新界面
void MainWindow::StopProcess(FrpMode mode) {
    bool zh = (currentLang_ == LangZh);
    if (infoDirty_) { cachedInfo_ = GetCachedInfo(); infoDirty_ = false; }
    const std::wstring exePath = (mode == FrpMode::Client)
        ? cachedInfo_.frpcExe : cachedInfo_.frpsExe;

    ProcessForMode(mode).Stop();

    int cleaned = 0;
    if (!exePath.empty())
        cleaned = KillSamePathExe(exePath);

    const wchar_t* stopped = mode == FrpMode::Client ? Ls(S_MSG_FRPC_STOPPED, zh) : Ls(S_MSG_FRPS_STOPPED, zh);
    std::wstring msg = stopped;
    if (cleaned > 0) {
        msg += std::wstring(Ls(S_MSG_CLEANUP, zh)) + std::to_wstring(cleaned) + (zh ? L" 个残留进程" : L" leftover processes");
    }

    SetWindowTextW(hwndStatusBar_, msg.c_str());
    if (tray_) tray_->ShowBalloon(Ls(S_BALLOON_CLEANED, zh), msg.c_str(), 2000);

    UpdateProcessControls();
    RefreshSummary();
}

// 进程异常退出时更新状态栏、显示气泡提示并刷新界面
void MainWindow::HandleProcessExitUi(FrpMode mode, DWORD exitCode) {
    bool zh = (currentLang_ == LangZh);
    const wchar_t* exitPrefix = mode == FrpMode::Client ? Ls(S_MSG_FRPC_EXIT, zh) : Ls(S_MSG_FRPS_EXIT, zh);
    std::wstring msg = std::wstring(exitPrefix) + std::to_wstring(exitCode);
    if (!exiting_ && exitCode != 0 && tray_)
        tray_->ShowBalloon(Ls(S_BALLOON_EXIT_ERROR, zh), msg.c_str(), 2000);

    SetWindowTextW(hwndStatusBar_, msg.c_str());
    UpdateProcessControls();
    RefreshSummary();
}

// 根据模式返回对应的 FrpProcess 引用
FrpProcess& MainWindow::ProcessForMode(FrpMode mode) {
    return (mode == FrpMode::Client) ? frpc_ : frps_;
}

// 返回模式对应的进程名称字符串
const wchar_t* MainWindow::ModeName(FrpMode mode) const {
    return (mode == FrpMode::Client) ? L"frpc" : L"frps";
}

// 返回模式对应的配置文件路径
std::wstring MainWindow::ConfigPathForMode(FrpMode mode) const {
    return (mode == FrpMode::Client) ? cachedInfo_.frpcConfig : cachedInfo_.frpsConfig;
}

// 获取缓存的 DetectionInfo，仅脏时重新检测
const Settings::DetectionInfo& MainWindow::GetCachedInfo() {
    if (infoDirty_) {
        cachedInfo_ = settings_.Detect();
        infoDirty_ = false;
    }
    return cachedInfo_;
}

// 根据配置文件构建摘要文本（服务端地址、端口、代理信息等）
std::wstring MainWindow::BuildSummaryText(FrpMode mode) const {
    std::wstring cfg = ConfigPathForMode(mode);
    if (cfg.empty()) return Ls(S_NO_CONFIG, currentLang_ == LangZh);
    FrpConfig parsed;
    if (!TomlHelper::Load(cfg, parsed)) return currentLang_ == LangZh ? L"配置文件读取失败。" : L"Failed to read config file.";
    bool zh = (currentLang_ == LangZh);

    std::wstring txt;
    if (mode == FrpMode::Client) {
        if (parsed.common_server_addr) {
            txt += std::wstring(Ls(S_HDR_SERVER_ADDR, zh)) + L": " + TomlHelper::Utf8ToWide(*parsed.common_server_addr);
            if (parsed.common_server_port)
                txt += L":" + std::to_wstring(*parsed.common_server_port);
            txt += L"\r\n";
        }
        if (parsed.common_log_file)
            txt += std::wstring(Ls(S_HDR_LOG_FILE, zh)) + L": " + TomlHelper::Utf8ToWide(*parsed.common_log_file) + L"\r\n";
        if (parsed.common_log_level)
            txt += std::wstring(Ls(S_HDR_LOG_LEVEL, zh)) + L": " + TomlHelper::Utf8ToWide(*parsed.common_log_level) + L"\r\n";
        if (parsed.common_log_max_days)
            txt += std::wstring(Ls(S_HDR_LOG_DAYS, zh)) + L": " + std::to_wstring(*parsed.common_log_max_days) + (zh ? L" 天\r\n" : L" days\r\n");
        for (const auto& p : parsed.proxies) {
            txt += TomlHelper::Utf8ToWide(p.name) + L" (" + TomlHelper::Utf8ToWide(p.type) + L") ";
            if (p.localPort)  txt += std::wstring(Ls(S_LOCAL, zh)) + L" " + std::to_wstring(*p.localPort);
            if (p.remotePort) txt += std::wstring(Ls(S_REMOTE, zh)) + L" " + std::to_wstring(*p.remotePort);
            txt += L"\r\n";
        }
    }
    else {  // Server
        txt += std::wstring(Ls(S_HDR_LOG_PORT, zh)) + L": " + ToWString(parsed.bind_port, zh);
        txt += std::wstring(L"\r\n") + Ls(S_HDR_HTTP_PORT, zh) + L": " + ToWString(parsed.vhost_http_port, zh);
        txt += std::wstring(L"\r\n") + Ls(S_HDR_HTTPS_PORT, zh) + L": " + ToWString(parsed.vhost_https_port, zh);
        if (parsed.common_log_file)
            txt += std::wstring(L"\r\n") + Ls(S_HDR_LOG_FILE, zh) + L": " + TomlHelper::Utf8ToWide(*parsed.common_log_file);
        if (parsed.common_log_level)
            txt += std::wstring(L"\r\n") + Ls(S_HDR_LOG_LEVEL, zh) + L": " + TomlHelper::Utf8ToWide(*parsed.common_log_level);
        if (parsed.common_log_max_days)
            txt += std::wstring(L"\r\n") + Ls(S_HDR_LOG_DAYS, zh) + L": " + std::to_wstring(*parsed.common_log_max_days) + (zh ? L" 天" : L" days");
        if (parsed.dashboard_port) {
            txt += std::wstring(L"\r\n") + Ls(S_HDR_DASH_ADDR, zh) + L": http://127.0.0.1:" + std::to_wstring(*parsed.dashboard_port);
            if (parsed.dashboard_user)
                txt += std::wstring(L"\r\n") + Ls(S_HDR_DASH_USER, zh) + L": " + TomlHelper::Utf8ToWide(*parsed.dashboard_user);
            if (parsed.dashboard_pwd)
                txt += std::wstring(L"\r\n") + Ls(S_HDR_DASH_PWD, zh) + L": ***";
        }
    }
    return txt;
}

// IFrpProcessCallback：进程输出数据，异步投递到主线程
void MainWindow::OnOutput(FrpMode mode, const char* line, int len) {
    if (!hwnd_) return;
    auto* event = new OutputEvent{ mode, std::string(line, static_cast<size_t>(len)) };
    PostMessageW(hwnd_, WM_OUTPUT, 0, reinterpret_cast<LPARAM>(event));
}

// IFrpProcessCallback：进程退出，异步投递到主线程
void MainWindow::OnExit(FrpMode mode, DWORD exitCode) {
    if (!hwnd_) return;
    auto* event = new ExitEvent{ mode, exitCode };
    PostMessageW(hwnd_, WM_PROCESS_EXIT, 0, reinterpret_cast<LPARAM>(event));
}

// ITrayCallback：托盘双击显示窗口
void MainWindow::OnTrayShow() {
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

// ITrayCallback：托盘退出请求，标记并关闭
void MainWindow::OnTrayExit() {
    exiting_ = true;
    PostMessageW(hwnd_, WM_TRAY_EXIT, 0, 0);
}

// 停止所有进程（供 Settings 对话框调用）
void MainWindow::StopAllProcesses() {
    frpc_.Stop();
    frps_.Stop();
    UpdateProcessControls();
}

// 切换中英文语言
void MainWindow::ToggleLang() {
    currentLang_ = (currentLang_ == LangZh) ? LangEn : LangZh;
    ApplyLang();
}

// 根据 currentLang_ 更新所有控件文本、托盘菜单及设置窗口语言
void MainWindow::ApplyLang() {
    bool zh = (currentLang_ == LangZh);
    // 更新窗口标题
    std::wstring title = zh ? L"FRP 管理器" : L"FRP Manager";
    title += Ls(IsUserAdmin() ? S_ADMIN : S_NON_ADMIN, zh);
    SetWindowTextW(hwnd_, title.c_str());

    // 更新版本标题（使用缓存，避免每次切换语言都跑一次 frpc -v）
    if (cachedFrpVer_.empty()) cachedFrpVer_ = zh ? L"未知" : L"Unknown";
    SetWindowTextW(hwndTitle_, ((zh ? L"FRP 版本：" : L"FRP Version: ") + cachedFrpVer_).c_str());

    // 设置对话框打开时，通知它刷新语言
    HWND settingsHwnd = settings_.GetDialogHwnd();
    if (settingsHwnd && IsWindow(settingsHwnd))
        SendMessageW(settingsHwnd, WM_REFRESH_LANG, zh ? 1 : 0, 0);

    SetWindowTextW(hwndBtnSettings_, Ls(S_BTN_SETTINGS, zh));
    SetWindowTextW(hwndBtnFrpcStart_, Ls(S_BTN_START, zh));
    SetWindowTextW(hwndBtnFrpcStop_, Ls(S_BTN_STOP, zh));
    SetWindowTextW(hwndBtnFrpsStart_, Ls(S_BTN_START, zh));
    SetWindowTextW(hwndBtnFrpsStop_, Ls(S_BTN_STOP, zh));
    SetWindowTextW(hwndChkAutoStart_, Ls(S_LABEL_AUTO_START, zh));
    SetWindowTextW(hwndStatusBar_, Ls(S_READY, zh));

    // 托盘 tooltip 和菜单
    tray_->SetTooltip(Ls(S_TRAY_TOOLTIP, zh));
    const wchar_t* showL = Ls(S_SHOW, zh);
    const wchar_t* langL = (currentLang_ == LangZh) ? Ls(S_SWITCH_TO_EN, zh) : Ls(S_SWITCH_TO_ZH, zh);
    const wchar_t* exitL = Ls(S_EXIT, zh);
    tray_->SetMenuStrings(showL, langL, exitL);

    // 刷新状态文字
    UpdateProcessControls();
    RefreshSummary();
}

// 窗口过程静态回调，分发到实例成员 HandleMsg
LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->HandleMsg(msg, wp, lp);
}

// 消息处理分发中心，响应所有窗口消息
LRESULT MainWindow::HandleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAYICON) {
        if (tray_) tray_->HandleMsg(hwnd_, msg, wp, lp);
        return 0;
    }

    // explorer 崩溃后重建托盘图标
    if (wmTaskbarCreated_ && msg == wmTaskbarCreated_) {
        if (tray_) {
            tray_->Destroy();
            tray_->Create(hwnd_, this, Ls(S_TRAY_TOOLTIP, currentLang_ == LangZh));
        }
        return 0;
    }

    switch (msg) {
    case WM_SIZE:
        OnSize(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;

    case WM_TIMER:
        if (wp == 1) CheckConfigChanges();
        return 0;

    case WM_TOGGLE_LANG:
        ToggleLang();
        return 0;

    case WM_CLOSE:
        if (!exiting_) {
            bool zh = (currentLang_ == LangZh);
            SetWindowTextW(hwndStatusBar_, zh ? L"程序已缩小到托盘，右键托盘图标选择\"显示窗口\"" : L"Minimized to tray. Right-click tray icon to show window.");
            if (tray_) tray_->ShowBalloon(Ls(S_BALLOON_NOTICE, zh), zh ? L"程序已缩小到托盘" : L"Minimized to tray", 2000);
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, 1);
        FlushPendingMessages();
        PostQuitMessage(0);
        return 0;

    case WM_OUTPUT: {
        auto* event = reinterpret_cast<OutputEvent*>(lp);
        if (event) {
            HWND hEdit = (event->mode == FrpMode::Client) ? hwndFrpcCard_ : hwndFrpsCard_;
            std::wstring wline = TomlHelper::Utf8ToWide(event->text) + L"\r\n";
            SendMessageW(hEdit, EM_SETSEL, -1, -1);
            SendMessageW(hEdit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(wline.c_str()));
            LimitEditLines(hEdit, 200);
            delete event;
        }
        return 0;
    }

    case WM_PROCESS_EXIT: {
        auto* event = reinterpret_cast<ExitEvent*>(lp);
        if (event) {
            HandleProcessExitUi(event->mode, event->exitCode);
            delete event;
        }
        return 0;
    }

    case WM_TRAY_EXIT:
        frpc_.Stop();
        frps_.Stop();
        DestroyWindow(hwnd_);
        return 0;
    }

    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// 清理残留的异步消息，防止内存泄漏
void MainWindow::FlushPendingMessages() {
    MSG msg;
    while (PeekMessageW(&msg, hwnd_, WM_OUTPUT, WM_OUTPUT, PM_REMOVE)) {
        auto* event = reinterpret_cast<OutputEvent*>(msg.lParam);
        delete event;
    }
    while (PeekMessageW(&msg, hwnd_, WM_PROCESS_EXIT, WM_PROCESS_EXIT, PM_REMOVE)) {
        auto* event = reinterpret_cast<ExitEvent*>(msg.lParam);
        delete event;
    }
}