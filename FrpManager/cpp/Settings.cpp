#include "Settings.h"
#include <windowsx.h>
#include <shlobj.h>
#include <TlHelp32.h>
#include <winsvc.h>
#include <deque>
#include <string>
#include "Window.h"

#pragma comment(lib, "advapi32.lib")

namespace {

    constexpr wchar_t SETTINGS_WINDOW_CLASS[] = L"FrpManagerSettingsWindow";

    // 当前语言设置（由ShowDialog设置）
    thread_local bool g_zh = true;

    const wchar_t* S(int id, bool zh) {
        static const wchar_t* t[][2] = {
            /* 0 */  { L"选择 FRP 安装目录",         L"Select FRP Root" },
            /* 1 */  { L"已更改 FRP 根目录: ",      L"FRP root changed: " },
            /* 2 */  { L"打开配置文件: ",            L"Open config: " },
            /* 3 */  { L"未找到可编辑的配置文件",    L"No editable config found" },
            /* 4 */  { L"所选 FRP 目录不存在。",     L"Directory does not exist." },
            /* 5 */  { L"将删除 FRP 根目录下的所有 .log 文件。\n确定要继续吗？",
                       L"All .log files in FRP root will be deleted.\nContinue?" },
            /* 6 */  { L"清除日志",                 L"Clear Logs" },
            /* 7 */  { L"清除日志失败: FRP 根目录无效", L"Failed: invalid FRP root" },
            /* 8 */  { L"FRP 设置",               L"FRP Settings" },
            /* 9 */  { L"FRP 根目录",               L"FRP Root" },
            /* 10 */ { L"选择路径",                L"Browse" },
            /* 11 */ { L"编辑配置",               L"Edit Config" },
            /* 12 */ { L"确定",                   L"OK" },
            /* 13 */ { L"取消",                   L"Cancel" },
        };
        return t[id][zh ? 0 : 1];
    }

    enum ControlId {
        IDC_ROOT_EDIT = 1001,
        IDC_BROWSE = 1002,
        IDC_FRPC_EXE = 1004,
        IDC_FRPS_EXE = 1005,
        IDC_FRPC_CFG = 1006,
        IDC_FRPS_CFG = 1007,
        IDC_OK = 1008,
        IDC_CANCEL = 1009,
        IDC_HINT = 1010,
        IDC_EDIT_CONFIG = 1011,
        IDC_INIT = 1012,
        IDC_LOG = 1013
    };

    bool FileExists(const std::wstring& path) {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool DirExists(const std::wstring& path) {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring JoinPath(const std::wstring& base, const wchar_t* leaf) {
        if (base.empty()) return {};
        return base + L"\\" + leaf;
    }

    std::wstring ReadRegStr(HKEY hRoot, const wchar_t* subKey, const wchar_t* name) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return {};
        wchar_t buf[MAX_PATH] = {};
        DWORD len = sizeof(buf);
        DWORD type = REG_SZ;
        if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &len) != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return {};
        }
        RegCloseKey(hKey);
        return std::wstring(buf);
    }

    bool WriteRegStr(HKEY hRoot, const wchar_t* subKey, const wchar_t* name, const wchar_t* value) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(hRoot, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            return false;
        bool ok = RegSetValueExW(hKey, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
            static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        RegCloseKey(hKey);
        return ok;
    }

    DWORD ReadRegDword(HKEY hRoot, const wchar_t* subKey, const wchar_t* name, DWORD def = 0) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return def;
        DWORD value = def;
        DWORD len = sizeof(value);
        RegQueryValueExW(hKey, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(&value), &len);
        RegCloseKey(hKey);
        return value;
    }

    bool WriteRegDword(HKEY hRoot, const wchar_t* subKey, const wchar_t* name, DWORD value) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(hRoot, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
            return false;
        bool ok = RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
            sizeof(value)) == ERROR_SUCCESS;
        RegCloseKey(hKey);
        return ok;
    }

    bool IsRunAsAdmin() {
        BOOL isElevated = FALSE;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION elevation = {};
            DWORD len = sizeof(TOKEN_ELEVATION);
            if (GetTokenInformation(token, TokenElevation, &elevation, len, &len))
                isElevated = elevation.TokenIsElevated;
            CloseHandle(token);
        }
        return isElevated;
    }

    int DeleteLogFilesInDir(const std::wstring& directory) {
        if (directory.empty() || !DirExists(directory)) return -1;
        std::wstring search = directory + L"\\*.log";
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return 0;

        int count = 0;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring fullPath = directory + L"\\" + fd.cFileName;
                if (DeleteFileW(fullPath.c_str())) ++count;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
        return count;
    }

    struct SettingsDialogState {
        Settings* settings = nullptr;
        HWND owner = nullptr;
        HWND hwnd = nullptr;
        HWND rootEdit = nullptr;
        HWND logEdit = nullptr;
        std::wstring root;
        bool accepted = false;
        bool zh = true;
        std::deque<std::wstring> logLines;
    };

    // 追加日志，使用 EM_REPLACESEL 高效追加，限制最大行数
    void AppendLog(HWND logEdit, std::deque<std::wstring>& lines, const std::wstring& msg) {
        lines.push_back(msg);
        if (lines.size() > 500) lines.pop_front();
        int len = GetWindowTextLengthW(logEdit);
        SendMessageW(logEdit, EM_SETSEL, len, len);
        std::wstring wmsg = msg + L"\r\n";
        SendMessageW(logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(wmsg.c_str()));
        LRESULT lineCount = SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0);
        ShowScrollBar(logEdit, SB_VERT, lineCount > 6);
        SendMessageW(logEdit, EM_SCROLLCARET, 0, 0);
    }

    void RefreshDetection(SettingsDialogState* state) {
        if (!state) return;
        int len = GetWindowTextLengthW(state->rootEdit);
        std::wstring buffer(len + 1, L'\0');
        GetWindowTextW(state->rootEdit, buffer.data(), len + 1);
        buffer.resize(len);
        state->root = buffer;
    }

    int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) {
        if (uMsg == BFFM_INITIALIZED && lpData) {
            SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, lpData);
        }
        return 0;
    }

    void BrowseForRoot(SettingsDialogState* state) {
        if (!state) return;
        BROWSEINFOW bi = {};
        bi.hwndOwner = state->hwnd;
        bi.lpszTitle = S(0, state->zh);
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        bi.lpfn = BrowseCallbackProc;
        std::wstring initDir = state->root;
        if (!initDir.empty() && DirExists(initDir)) {
            bi.lParam = reinterpret_cast<LPARAM>(initDir.c_str());
        }
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return;
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
            std::wstring oldRoot = state->root;
            SetWindowTextW(state->rootEdit, path);
            RefreshDetection(state);
            if (oldRoot != state->root) {
                AppendLog(state->logEdit, state->logLines, std::wstring(S(1, state->zh)) + state->root);
            }
        }
        CoTaskMemFree(pidl);
    }

    LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
        case WM_REFRESH_LANG:
            if (state) {
                state->zh = (wp != 0);
                SetWindowTextW(hwnd, S(8, state->zh));
                SetWindowTextW(state->rootEdit, state->root.c_str());
                SetWindowTextW(GetDlgItem(hwnd, IDC_BROWSE), S(10, state->zh));
                SetWindowTextW(GetDlgItem(hwnd, IDC_INIT), S(6, state->zh));
                SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_CONFIG), S(11, state->zh));
                SetWindowTextW(GetDlgItem(hwnd, IDC_OK), S(12, state->zh));
                SetWindowTextW(GetDlgItem(hwnd, IDC_CANCEL), S(13, state->zh));
                SetWindowTextW(GetDlgItem(hwnd, IDC_HINT), S(9, state->zh));
            }
            return 0;
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            state = reinterpret_cast<SettingsDialogState*>(cs->lpCreateParams);
            state->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return TRUE;
        }
        case WM_COMMAND:
            if (!state) break;
            switch (LOWORD(wp)) {
            case IDC_BROWSE:
                BrowseForRoot(state);
                return 0;
            case IDC_EDIT_CONFIG: {
                bool opened = false;
                for (const auto& cfg : { state->settings->GetFrpcConfig(), state->settings->GetFrpsConfig() }) {
                    if (!cfg.empty()) {
                        ShellExecuteW(hwnd, L"open", cfg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        AppendLog(state->logEdit, state->logLines, std::wstring(S(2, state->zh)) + cfg);
                        opened = true;
                    }
                }
                if (!opened)
                    AppendLog(state->logEdit, state->logLines, S(3, state->zh));
                return 0;
            }
            case IDC_OK: {
                int len = GetWindowTextLengthW(state->rootEdit);
                std::wstring root(len + 1, L'\0');
                GetWindowTextW(state->rootEdit, root.data(), len + 1);
                root.resize(len);
                if (!root.empty() && !DirExists(root)) {
                    MessageBoxW(hwnd, S(4, state->zh), S(8, state->zh), MB_ICONERROR);
                    return 0;
                }
                state->settings->SetFrpRoot(root);
                state->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            }
            case IDC_CANCEL:
                DestroyWindow(hwnd);
                return 0;
            case IDC_ROOT_EDIT:
                if (HIWORD(wp) == EN_CHANGE) RefreshDetection(state);
                return 0;
            case IDC_INIT: {
                int ret = MessageBoxW(hwnd, S(5, state->zh), S(6, state->zh), MB_YESNO | MB_ICONWARNING);
                if (ret == IDYES) {
                    std::wstring rootDir = state->settings->GetFrpRoot();
                    int deleted = DeleteLogFilesInDir(rootDir);
                    if (deleted < 0) {
                        AppendLog(state->logEdit, state->logLines, S(7, state->zh));
                    }
                    else {
                        AppendLog(state->logEdit, state->logLines,
                            std::wstring(state->zh ? L"已删除 " : L"Deleted ") + std::to_wstring(deleted) +
                            (state->zh ? L" 个日志文件" : L" log files"));
                    }
                }
                return 0;
            }
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void EnsureSettingsClassRegistered() {
        static bool registered = false;
        if (registered) return;
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = SETTINGS_WINDOW_CLASS;
        wc.hIcon = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);
        registered = true;
    }

    HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, HFONT font) {
        HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return hwnd;
    }

    HWND CreateButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id, HFONT font, DWORD style = BS_PUSHBUTTON) {
        HWND hwnd = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return hwnd;
    }

} // namespace

const wchar_t* Settings::REG_KEY = L"Software\\FrpManager";
const wchar_t* Settings::VAL_FRP_ROOT = L"FrpRoot";
const wchar_t* Settings::VAL_AUTO_START = L"AutoStart";

Settings::Settings() {}
Settings::~Settings() {}

bool Settings::DetectionInfo::HasAnyExecutable() const {
    return !frpcExe.empty() || !frpsExe.empty();
}

std::wstring Settings::GetFrpRoot() const {
    return ReadRegStr(HKEY_CURRENT_USER, REG_KEY, VAL_FRP_ROOT);
}

void Settings::SetFrpRoot(const std::wstring& path) {
    WriteRegStr(HKEY_CURRENT_USER, REG_KEY, VAL_FRP_ROOT, path.c_str());
}

// 读取开机自启动注册表状态
bool Settings::GetAutoStart() const {
    return ReadRegDword(HKEY_CURRENT_USER, REG_KEY, VAL_AUTO_START, 0) != 0;
}

// 设置开机自启动（写入 HKCU\Run 键）
void Settings::SetAutoStart(bool enable) {
    HKEY hRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hRun) != ERROR_SUCCESS) {
        WriteRegDword(HKEY_CURRENT_USER, REG_KEY, VAL_AUTO_START, enable ? 1 : 0);
        return;
    }
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cmd = std::wstring(L"\"") + exePath + L"\" --hide";
        RegSetValueExW(hRun, L"FrpManager", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    }
    else {
        RegDeleteValueW(hRun, L"FrpManager");
    }
    RegCloseKey(hRun);
    WriteRegDword(HKEY_CURRENT_USER, REG_KEY, VAL_AUTO_START, enable ? 1 : 0);
}

// 检测 FRP 目录下的可执行文件和配置文件路径
Settings::DetectionInfo Settings::Detect() const {
    DetectionInfo info;
    info.root = GetFrpRoot();
    info.frpcExe = FileExists(JoinPath(info.root, L"frpc.exe")) ? JoinPath(info.root, L"frpc.exe") : L"";
    info.frpsExe = FileExists(JoinPath(info.root, L"frps.exe")) ? JoinPath(info.root, L"frps.exe") : L"";
    info.frpcConfig = FileExists(JoinPath(info.root, L"frpc.toml")) ? JoinPath(info.root, L"frpc.toml")
        : (FileExists(JoinPath(info.root, L"frpc.ini")) ? JoinPath(info.root, L"frpc.ini") : L"");
    info.frpsConfig = FileExists(JoinPath(info.root, L"frps.toml")) ? JoinPath(info.root, L"frps.toml")
        : (FileExists(JoinPath(info.root, L"frps.ini")) ? JoinPath(info.root, L"frps.ini") : L"");
    return info;
}

std::wstring Settings::GetFrpcExe() const { return Detect().frpcExe; }
std::wstring Settings::GetFrpsExe() const { return Detect().frpsExe; }
std::wstring Settings::GetFrpcConfig() const { return Detect().frpcConfig; }
std::wstring Settings::GetFrpsConfig() const { return Detect().frpsConfig; }

// 自动检测 FRP 根目录：优先程序同目录，其次上级目录
std::wstring Settings::AutoDetectFrpRoot() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return {};
    *slash = L'\0';
    std::wstring currentDir = exePath;
    auto hasFrp = [](const std::wstring& dir) {
        return FileExists(JoinPath(dir, L"frpc.exe")) || FileExists(JoinPath(dir, L"frps.exe"));
        };
    if (hasFrp(currentDir)) return currentDir;
    wchar_t* parentSlash = wcsrchr(exePath, L'\\');
    if (parentSlash) {
        *parentSlash = L'\0';
        if (hasFrp(exePath)) return exePath;
    }
    return {};
}

// 显示设置对话框模态窗口（浏览目录、清理日志、编辑配置）
bool Settings::ShowDialog(HWND owner, bool zh) {
    EnsureSettingsClassRegistered();

    g_zh = zh;
    SettingsDialogState state;
    state.settings = this;
    state.owner = owner;
    state.root = GetFrpRoot();
    state.zh = zh;

    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_CHARSET, L"Microsoft YaHei UI");

    const int W = 420;
    const int PAD = 20;
    RECT rc = { 0, 0, W, 300 };
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    RECT ownerRect = { 0, 0, winW, winH };
    if (owner) GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - winW) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - winH) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, SETTINGS_WINDOW_CLASS, S(8, zh),
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, winW, winH, owner, nullptr, GetModuleHandle(nullptr), &state);
    if (!hwnd) {
        DeleteObject(font);
        this->hwndDialog_ = nullptr;
        return false;
    }
    this->hwndDialog_ = hwnd;

    // 布局与主窗口同步：gx=20, rightEdge=W-20, 内容宽度=380
    const int contentW = W - PAD * 2;  // 380
    const int gx = PAD;                 // 20
    const int rightEdge = W - PAD;      // 400
    const int browseW = 80;
    const int editW = rightEdge - gx - browseW - 8;

    CreateLabel(hwnd, S(9, zh), gx, 16, 200, 20, font);
    state.rootEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state.root.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, gx, 38, editW, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ROOT_EDIT)), GetModuleHandle(nullptr), nullptr);
    SendMessageW(state.rootEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    CreateButton(hwnd, S(10, zh), rightEdge - browseW, 37, browseW, 30, IDC_BROWSE, font);

    CreateButton(hwnd, S(6, zh), gx, 72, 80, 30, IDC_INIT, font);
    CreateButton(hwnd, S(11, zh), rightEdge - 80, 72, 80, 30, IDC_EDIT_CONFIG, font);

    const int logY = 108;
    const int logH = 130;
    state.logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
        gx, logY, contentW, logH, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), GetModuleHandle(nullptr), nullptr);
    SendMessageW(state.logEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ShowScrollBar(state.logEdit, SB_VERT, FALSE);

    const int btnY = logY + logH + 10;
    const int btnW2 = 80;
    CreateButton(hwnd, S(12, zh), rightEdge - btnW2 * 2 - 8, btnY, btnW2, 30, IDC_OK, font);
    CreateButton(hwnd, S(13, zh), rightEdge - btnW2, btnY, btnW2, 30, IDC_CANCEL, font);

    RefreshDetection(&state);

    if (owner) EnableWindow(owner, FALSE);

    bool quitReceived = false;
    MSG msg = {};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    // WM_QUIT 被模态循环吞掉了，重新投递给主循环
    if (msg.message == WM_QUIT) {
        quitReceived = true;
        this->hwndDialog_ = nullptr;
    }

    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    DeleteObject(font);
    if (quitReceived) PostQuitMessage(static_cast<int>(msg.wParam));
    return state.accepted;
}

// 删除 Windows 服务 frpc/frps（需管理员权限）
void Settings::DeleteFrpServices() {
    if (!IsRunAsAdmin()) return;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    for (const wchar_t* svcName : { L"frpc", L"frps" }) {
        SC_HANDLE svc = OpenServiceW(scm, svcName, DELETE | SERVICE_STOP);
        if (svc) {
            SERVICE_STATUS status;
            ControlService(svc, SERVICE_CONTROL_STOP, &status);
            Sleep(200);
            DeleteService(svc);
            CloseServiceHandle(svc);
        }
    }
    CloseServiceHandle(scm);
}

// 程序初始化清理：停止所有 frp 进程、关闭开机自启、删除服务（需确认）
void Settings::InitCleanup() {
    if (m_pMain) {
        HWND mainHwnd = m_pMain->GetHwnd();
        // 确认对话框：避免误杀生产环境 frp 进程
        int ret = MessageBoxW(mainHwnd,
            L"将停止所有 frpc/frps 进程、关闭开机自启并删除服务。\n确定继续？",
            L"初始化清理", MB_YESNO | MB_ICONWARNING);
        if (ret != IDYES) return;
        m_pMain->StopAllProcesses();
    }
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, L"frpc.exe") == 0 ||
                    _wcsicmp(pe.szExeFile, L"frps.exe") == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        TerminateProcess(hProc, 1);
                        CloseHandle(hProc);
                    }
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }
    SetAutoStart(false);
    DeleteFrpServices();
}