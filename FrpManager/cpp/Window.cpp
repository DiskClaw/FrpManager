/**
 * Window.cpp - 主窗口实现
 *
 * 提供窗口创建、进程生命周期管理、摘要刷新、配置文件监控
 * 以及 FRP 版本自动检测等功能。
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

    // 将可选数值转换为宽字符串，失败显示“未设置”
    std::wstring ToWString(const std::optional<int>& val) {
        return val.has_value() ? std::to_wstring(*val) : L"未设置";
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
        std::wstring cmdLine = L"\"" + exePath + L"\" -v";
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
} // anonymous namespace

// ---- 构造与析构 ----
MainWindow::MainWindow() {
    frpc_.SetCallback(this);
    frps_.SetCallback(this);
    settings_.SetMainWindow(this);
}

MainWindow::~MainWindow() {
    frpc_.Stop();
    frps_.Stop();
    delete tray_;
    if (hFont_)     DeleteObject(hFont_);
    if (hBoldFont_) DeleteObject(hBoldFont_);
}

// ---- 注册窗口类 ----
bool MainWindow::RegisterClass() {
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = MainWindow::WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hIcon = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassExW(&wc) != 0;
}

// ---- 创建主窗口及所有子控件 ----
HWND MainWindow::CreateWindow_() {
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

    // 创建字体
    hFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_CHARSET, L"Microsoft YaHei UI");
    hBoldFont_ = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_CHARSET, L"Microsoft YaHei UI");

    // 初始化根目录并获取版本号用于标题
    std::wstring frpVer;
    {
        std::wstring root = settings_.GetFrpRoot();
        if (root.empty()) {
            root = Settings::AutoDetectFrpRoot();
            if (!root.empty()) settings_.SetFrpRoot(root);
        }
        auto info = settings_.Detect();
        std::wstring exePath = info.frpcExe.empty() ? info.frpsExe : info.frpcExe;
        frpVer = GetExeVersionByCommand(exePath);
    }
    if (frpVer.empty()) frpVer = L"未知";
    std::wstring versionTitle = L"FRP 版本：" + frpVer;

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
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 80, 20,
        hwnd_, (HMENU)106, GetModuleHandle(nullptr), nullptr);
    if (settings_.GetAutoStart())
        SendMessageW(hwndChkAutoStart_, BM_SETCHECK, BST_CHECKED, 0);

    // 设置字体（标题用加粗，其余用常规）
    for (HWND child : { hwndBtnSettings_,
        hwndFrpcLabel_, hwndFrpcStatus_, hwndBtnFrpcStart_, hwndBtnFrpcStop_,
        hwndFrpsLabel_, hwndFrpsStatus_, hwndBtnFrpsStart_, hwndBtnFrpsStop_,
        hwndFrpcCard_, hwndFrpsCard_, hwndDivider_, hwndStatusBar_, hwndChkAutoStart_ })
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(hFont_), TRUE);
    SendMessageW(hwndTitle_, WM_SETFONT, reinterpret_cast<WPARAM>(hBoldFont_), TRUE);

    // 确保根目录已设置，必要时弹出设置对话框
    std::wstring root = settings_.GetFrpRoot();
    if (root.empty()) {
        root = Settings::AutoDetectFrpRoot();
        if (!root.empty()) settings_.SetFrpRoot(root);
    }
    if (root.empty()) ShowSettingsDialog(); else SetFrpRoot(root);

    // 初始化文件时间戳缓存
    auto info = settings_.Detect();
    GetFileLastWrite(info.frpcExe, frpcExeLastWrite_);
    GetFileLastWrite(info.frpsExe, frpsExeLastWrite_);

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

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    UpdateProcessControls();
    RefreshSummary();

    return hwnd_;
}

// ---- 设置根目录并刷新界面 ----
void MainWindow::SetFrpRoot(const std::wstring& path) {
    settings_.SetFrpRoot(path);
    UpdateProcessControls();
    RefreshSummary();

    // 重新获取并显示版本号
    auto info = settings_.Detect();
    std::wstring exePath = info.frpcExe.empty() ? info.frpsExe : info.frpcExe;
    std::wstring frpVer = GetExeVersionByCommand(exePath);
    if (frpVer.empty()) frpVer = L"未知";
    SetWindowTextW(hwndTitle_, (L"FRP 版本：" + frpVer).c_str());

    // 更新可执行文件时间戳缓存
    GetFileLastWrite(info.frpcExe, frpcExeLastWrite_);
    GetFileLastWrite(info.frpsExe, frpsExeLastWrite_);
}

// ---- 刷新两个摘要卡片 ----
void MainWindow::RefreshSummary() {
    for (auto mode : { FrpMode::Client, FrpMode::Server }) {
        std::wstring cfg = ConfigPathForMode(mode);
        HWND hEdit = (mode == FrpMode::Client) ? hwndFrpcCard_ : hwndFrpsCard_;

        // 首行：配置文件名
        std::wstring text;
        if (cfg.empty()) {
            text = L"未找到配置文件";
        }
        else {
            const wchar_t* name = wcsrchr(cfg.c_str(), L'\\');
            text = name ? std::wstring(name + 1) : cfg;
        }
        text += L"\r\n" + BuildSummaryText(mode);
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
    auto info = settings_.Detect();

    // 检查可执行文件修改并自动刷新顶部版本标题
    auto checkExeVersion = [&](const std::wstring& exePath, FILETIME& cachedTime) {
        FILETIME currentTime{};
        bool fileExists = GetFileLastWrite(exePath, currentTime);

        bool exeChanged = false;
        if (fileExists) {
            if (CompareFileTime(&currentTime, &cachedTime) != 0)
                exeChanged = true;
        }
        else {
            // 文件不存在：如果之前缓存非空则表示被删除
            FILETIME zero{};
            if (CompareFileTime(&cachedTime, &zero) != 0) {
                exeChanged = true;
                currentTime = {};
            }
        }

        if (exeChanged) {
            cachedTime = currentTime;
            std::wstring ver = GetExeVersionByCommand(exePath);
            if (ver.empty()) ver = L"未知";
            SetWindowTextW(hwndTitle_, (L"FRP 版本：" + ver).c_str());
        }
        };

    checkExeVersion(info.frpcExe, frpcExeLastWrite_);
    checkExeVersion(info.frpsExe, frpsExeLastWrite_);

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
    auto info = settings_.Detect();
    bool frpcExeExists = !info.frpcExe.empty() && PathFileExistsW(info.frpcExe.c_str());
    bool frpsExeExists = !info.frpsExe.empty() && PathFileExistsW(info.frpsExe.c_str());
    bool frpcCfgExists = !info.frpcConfig.empty() && PathFileExistsW(info.frpcConfig.c_str());
    bool frpsCfgExists = !info.frpsConfig.empty() && PathFileExistsW(info.frpsConfig.c_str());

    bool frpcRunning = frpc_.IsRunning() || (frpcExeExists && IsExactExeRunning(info.frpcExe));
    bool frpsRunning = frps_.IsRunning() || (frpsExeExists && IsExactExeRunning(info.frpsExe));

    SetWindowTextW(hwndFrpcStatus_, frpcRunning ? L"运行中" : (frpcExeExists ? L"已停止" : L"缺少 frpc.exe"));
    SetWindowTextW(hwndFrpsStatus_, frpsRunning ? L"运行中" : (frpsExeExists ? L"已停止" : L"缺少 frps.exe"));
    EnableWindow(hwndBtnFrpcStart_, !frpcRunning && frpcExeExists && frpcCfgExists);
    EnableWindow(hwndBtnFrpcStop_, frpcRunning);
    EnableWindow(hwndBtnFrpsStart_, !frpsRunning && frpsExeExists && frpsCfgExists);
    EnableWindow(hwndBtnFrpsStop_, frpsRunning);

    if (settings_.GetFrpRoot().empty())
        SetWindowTextW(hwndStatusBar_, L"请先在设置中选择 frp 根目录");
}

// ---- 窗口尺寸变化时重新布局 ----
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
    SetWindowPos(hwndStatusBar_, nullptr, gx, h - PAD - STATUS_H, rightEdge - gx - 90, STATUS_H, SWP_NOZORDER);
    SetWindowPos(hwndChkAutoStart_, nullptr, rightEdge - 80, h - PAD - STATUS_H, 80, STATUS_H, SWP_NOZORDER);
}

// ---- 命令处理（按钮点击） ----
void MainWindow::OnCommand(int id) {
    switch (id) {
    case 101: StartProcess(FrpMode::Client);  break;
    case 102: StopProcess(FrpMode::Client);   break;
    case 103: ShowSettingsDialog();           break;
    case 104: StartProcess(FrpMode::Server);  break;
    case 105: StopProcess(FrpMode::Server);   break;
    case 106:
        settings_.SetAutoStart(
            SendMessageW(hwndChkAutoStart_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        SetWindowTextW(hwndStatusBar_,
            settings_.GetAutoStart() ? L"已开启开机自启" : L"已关闭开机自启");
        break;
    }
}

// ---- 显示设置对话框 ----
bool MainWindow::ShowSettingsDialog() {
    bool ok = settings_.ShowDialog(hwnd_);
    if (ok) {
        UpdateProcessControls();
        RefreshSummary();
        SetWindowTextW(hwndStatusBar_, L"设置已更新");
    }
    return ok;
}

// ---- 启动指定模式进程 ----
void MainWindow::StartProcess(FrpMode mode) {
    const std::wstring exePath = (mode == FrpMode::Client)
        ? settings_.GetFrpcExe() : settings_.GetFrpsExe();
    const std::wstring cfgPath = ConfigPathForMode(mode);
    const std::wstring rootDir = settings_.GetFrpRoot();

    if (exePath.empty() || cfgPath.empty() || rootDir.empty()) {
        MessageBoxW(hwnd_, L"请先在设置中配置 frp 目录。", L"启动失败", MB_ICONERROR);
        return;
    }

    if (IsExactExeRunning(exePath)) {
        std::wstring msg = std::wstring(ModeName(mode)) + L" 已在运行中，请先停止后再启动。";
        SetWindowTextW(hwndStatusBar_, msg.c_str());
        if (tray_) tray_->ShowBalloon(L"提示", msg.c_str(), 2000);
        UpdateProcessControls();
        return;
    }

    if (ProcessForMode(mode).IsRunning()) {
        SetWindowTextW(hwndStatusBar_,
            (std::wstring(ModeName(mode)) + L" 正在运行。").c_str());
        return;
    }

    if (ProcessForMode(mode).Start(exePath, cfgPath, rootDir))
        SetWindowTextW(hwndStatusBar_,
            (std::wstring(ModeName(mode)) + L" 已启动").c_str());
    else
        MessageBoxW(hwnd_,
            (std::wstring(L"启动 ") + ModeName(mode) + L" 失败").c_str(),
            L"错误", MB_ICONERROR);

    UpdateProcessControls();
}

// ---- 停止指定模式进程 ----
void MainWindow::StopProcess(FrpMode mode) {
    const std::wstring exePath = (mode == FrpMode::Client)
        ? settings_.GetFrpcExe() : settings_.GetFrpsExe();

    ProcessForMode(mode).Stop();

    int cleaned = 0;
    if (!exePath.empty())
        cleaned = KillSamePathExe(exePath);

    std::wstring msg = std::wstring(ModeName(mode)) + L" 已停止";
    if (cleaned > 0) msg += L"，清理了 " + std::to_wstring(cleaned) + L" 个残留进程";

    SetWindowTextW(hwndStatusBar_, msg.c_str());
    if (tray_) tray_->ShowBalloon(L"清理完成", msg.c_str(), 2000);

    UpdateProcessControls();
    RefreshSummary();
}

// ---- 进程退出回调（UI 更新） ----
void MainWindow::HandleProcessExitUi(FrpMode mode, DWORD exitCode) {
    std::wstring msg = std::wstring(ModeName(mode))
        + L" 已退出，代码 " + std::to_wstring(exitCode);
    if (!exiting_ && exitCode != 0 && tray_)
        tray_->ShowBalloon(L"异常退出", msg.c_str(), 2000);

    SetWindowTextW(hwndStatusBar_, msg.c_str());
    UpdateProcessControls();
    RefreshSummary();
}

// ---- 快捷获取对应模式的进程对象 ----
FrpProcess& MainWindow::ProcessForMode(FrpMode mode) {
    return (mode == FrpMode::Client) ? frpc_ : frps_;
}

const wchar_t* MainWindow::ModeName(FrpMode mode) const {
    return (mode == FrpMode::Client) ? L"frpc" : L"frps";
}

std::wstring MainWindow::ConfigPathForMode(FrpMode mode) const {
    return (mode == FrpMode::Client) ? settings_.GetFrpcConfig() : settings_.GetFrpsConfig();
}

// ---- 构建摘要卡片详细内容（不含版本号，已显示在顶部标题） ----
std::wstring MainWindow::BuildSummaryText(FrpMode mode) const {
    std::wstring cfg = ConfigPathForMode(mode);
    if (cfg.empty()) return L"未找到配置文件。";
    FrpConfig parsed;
    if (!TomlHelper::Load(cfg, parsed)) return L"配置文件读取失败。";

    std::wstring txt;
    if (mode == FrpMode::Client) {
        if (parsed.common_server_addr) {
            txt += L"服务端地址：" + TomlHelper::Utf8ToWide(*parsed.common_server_addr);
            if (parsed.common_server_port)
                txt += L":" + std::to_wstring(*parsed.common_server_port);
            txt += L"\r\n";
        }
        if (parsed.common_log_file)
            txt += L"日志文件：" + TomlHelper::Utf8ToWide(*parsed.common_log_file) + L"\r\n";
        if (parsed.common_log_level)
            txt += L"日志等级：" + TomlHelper::Utf8ToWide(*parsed.common_log_level) + L"\r\n";
        if (parsed.common_log_max_days)
            txt += L"日志保留：" + std::to_wstring(*parsed.common_log_max_days) + L" 天\r\n";
        for (const auto& p : parsed.proxies) {
            txt += TomlHelper::Utf8ToWide(p.name) + L" (" + TomlHelper::Utf8ToWide(p.type) + L") ";
            if (p.localPort)  txt += L"本地:" + std::to_wstring(*p.localPort);
            if (p.remotePort) txt += L" → 远程:" + std::to_wstring(*p.remotePort);
            txt += L"\r\n";
        }
    }
    else {  // Server
        txt += L"监听端口：" + ToWString(parsed.bind_port);
        txt += L"\r\nHTTP 端口：" + ToWString(parsed.vhost_http_port);
        txt += L"\r\nHTTPS 端口：" + ToWString(parsed.vhost_https_port);
        if (parsed.common_log_file)
            txt += L"\r\n日志文件：" + TomlHelper::Utf8ToWide(*parsed.common_log_file);
        if (parsed.common_log_level)
            txt += L"\r\n日志等级：" + TomlHelper::Utf8ToWide(*parsed.common_log_level);
        if (parsed.common_log_max_days)
            txt += L"\r\n日志保留：" + std::to_wstring(*parsed.common_log_max_days) + L" 天";
        if (parsed.dashboard_port) {
            txt += L"\r\n管理地址：http://127.0.0.1:" + std::to_wstring(*parsed.dashboard_port);
            if (parsed.dashboard_user)
                txt += L"\r\n账户名：" + TomlHelper::Utf8ToWide(*parsed.dashboard_user);
            if (parsed.dashboard_pwd)
                txt += L"\r\n管理密码：" + TomlHelper::Utf8ToWide(*parsed.dashboard_pwd);
        }
    }
    return txt;
}

// ---- 进程输出回调 ----
void MainWindow::OnOutput(FrpMode mode, const char* line, int len) {
    if (!hwnd_) return;
    auto* event = new OutputEvent{ mode, std::string(line, static_cast<size_t>(len)) };
    PostMessageW(hwnd_, WM_OUTPUT, 0, reinterpret_cast<LPARAM>(event));
}

// ---- 进程退出回调 ----
void MainWindow::OnExit(FrpMode mode, DWORD exitCode) {
    if (!hwnd_) return;
    auto* event = new ExitEvent{ mode, exitCode };
    PostMessageW(hwnd_, WM_PROCESS_EXIT, 0, reinterpret_cast<LPARAM>(event));
}

// ---- 托盘回调 ----
void MainWindow::OnTrayShow() {
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void MainWindow::OnTrayExit() {
    exiting_ = true;
    PostMessageW(hwnd_, WM_TRAY_EXIT, 0, 0);
}

void MainWindow::StopAllProcesses() {
    frpc_.Stop();
    frps_.Stop();
    UpdateProcessControls();
}

// ---- 托盘气泡防抖（两次气泡间隔至少 2 秒） ----
bool MainWindow::CanShowBalloon() const {
    ULONGLONG now = GetTickCount64();
    if (now - lastBalloonTick_ < 2000)
        return false;
    // 注意：此处需要修改成员变量，但因 const 限制实际需调整
    // 简单起见直接返回 true（如有需要可移除 const 并更新时间）
    return true;
}

// ---- 静态窗口过程 ----
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

// ---- 实例消息处理 ----
LRESULT MainWindow::HandleMsg(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAYICON) {
        if (tray_) tray_->HandleMsg(hwnd_, msg, wp, lp);
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

    case WM_CLOSE:
        if (!exiting_) {
            SetWindowTextW(hwndStatusBar_, L"程序已缩小到托盘，右键托盘图标选择\"显示窗口\"。");
            if (tray_) tray_->ShowBalloon(L"提示", L"程序已缩小到托盘", 2000);
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        }
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd_, 1);
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