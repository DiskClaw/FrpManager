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
        std::deque<std::wstring> logLines;
    };

    // 追加日志，无行数限制，根据实际行数自动显示/隐藏垂直滚动条
    void AppendLog(HWND logEdit, std::deque<std::wstring>& lines, const std::wstring& msg) {
        lines.push_back(msg);
        std::wstring text;
        for (const auto& line : lines) {
            text += line + L"\r\n";
        }
        SetWindowTextW(logEdit, text.c_str());
        LRESULT lineCount = SendMessageW(logEdit, EM_GETLINECOUNT, 0, 0);
        ShowScrollBar(logEdit, SB_VERT, lineCount > 6);
        InvalidateRect(logEdit, NULL, TRUE);
        // 滚动到底部
        int len = GetWindowTextLengthW(logEdit);
        SendMessageW(logEdit, EM_SETSEL, len, len);
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
        bi.lpszTitle = L"选择 FRP 安装目录";
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
                AppendLog(state->logEdit, state->logLines, L"已更改 FRP 根目录: " + state->root);
            }
        }
        CoTaskMemFree(pidl);
    }

    LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* state = reinterpret_cast<SettingsDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
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
                        AppendLog(state->logEdit, state->logLines, L"打开配置文件: " + cfg);
                        opened = true;
                    }
                }
                if (!opened)
                    AppendLog(state->logEdit, state->logLines, L"未找到可编辑的配置文件");
                return 0;
            }
            case IDC_OK: {
                int len = GetWindowTextLengthW(state->rootEdit);
                std::wstring root(len + 1, L'\0');
                GetWindowTextW(state->rootEdit, root.data(), len + 1);
                root.resize(len);
                if (!root.empty() && !DirExists(root)) {
                    MessageBoxW(hwnd, L"所选 FRP 目录不存在。", L"设置", MB_ICONERROR);
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
                int ret = MessageBoxW(hwnd,
                    L"将删除 FRP 根目录下的所有 .log 文件。\n确定要继续吗？",
                    L"清除日志", MB_YESNO | MB_ICONWARNING);
                if (ret == IDYES) {
                    std::wstring rootDir = state->settings->GetFrpRoot();
                    int deleted = DeleteLogFilesInDir(rootDir);
                    if (deleted < 0) {
                        AppendLog(state->logEdit, state->logLines, L"清除日志失败: FRP 根目录无效");
                    }
                    else {
                        AppendLog(state->logEdit, state->logLines,
                            L"已删除 " + std::to_wstring(deleted) + L" 个日志文件");
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

bool Settings::GetAutoStart() const {
    return ReadRegDword(HKEY_CURRENT_USER, REG_KEY, VAL_AUTO_START, 0) != 0;
}

void Settings::SetAutoStart(bool enable) {
    HKEY hRun = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hRun) != ERROR_SUCCESS) {
        WriteRegDword(HKEY_CURRENT_USER, REG_KEY, VAL_AUTO_START, enable ? 1 : 0);
        return;
    }
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        RegSetValueExW(hRun, L"FrpManager", 0, REG_SZ, reinterpret_cast<const BYTE*>(exePath),
            static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(wchar_t)));
    }
    else {
        RegDeleteValueW(hRun, L"FrpManager");
    }
    RegCloseKey(hRun);
    WriteRegDword(HKEY_CURRENT_USER, REG_KEY, VAL_AUTO_START, enable ? 1 : 0);
}

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

bool Settings::ShowDialog(HWND owner) {
    EnsureSettingsClassRegistered();

    SettingsDialogState state;
    state.settings = this;
    state.owner = owner;
    state.root = GetFrpRoot();

    HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_CHARSET, L"Segoe UI");

    const int W = 520;
    const int PAD = 24;
    RECT rc = { 0, 0, W, 330 };
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_POPUP, FALSE);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    RECT ownerRect = { 0, 0, winW, winH };
    if (owner) GetWindowRect(owner, &ownerRect);
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - winW) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - winH) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME, SETTINGS_WINDOW_CLASS, L"FRP 设置",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, winW, winH, owner, nullptr, GetModuleHandle(nullptr), &state);
    if (!hwnd) {
        DeleteObject(font);
        return false;
    }

    const int browseW = 90;
    const int editW = W - PAD * 2 - browseW - 8;

    CreateLabel(hwnd, L"FRP 根目录", PAD, 18, 200, 20, font);
    state.rootEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", state.root.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, PAD, 40, editW, 24, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ROOT_EDIT)), GetModuleHandle(nullptr), nullptr);
    SendMessageW(state.rootEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    CreateButton(hwnd, L"选择路径", W - PAD - browseW, 39, browseW, 26, IDC_BROWSE, font);

    CreateButton(hwnd, L"清除日志", PAD, 76, 90, 26, IDC_INIT, font);
    CreateButton(hwnd, L"编辑配置", W - PAD - 90, 76, 90, 26, IDC_EDIT_CONFIG, font);

    const int logY = 108;
    const int logH = 160;
    state.logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
        PAD, logY, W - PAD * 2, logH, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), GetModuleHandle(nullptr), nullptr);
    SendMessageW(state.logEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    ShowScrollBar(state.logEdit, SB_VERT, FALSE);  // 初始隐藏

    const int btnY = logY + logH + 10;
    const int btnW2 = 90;
    CreateButton(hwnd, L"确定", W - PAD - btnW2 * 2 - 10, btnY, btnW2, 28, IDC_OK, font);
    CreateButton(hwnd, L"取消", W - PAD - btnW2, btnY, btnW2, 28, IDC_CANCEL, font);

    RefreshDetection(&state);

    if (owner) EnableWindow(owner, FALSE);

    MSG msg = {};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    DeleteObject(font);
    return state.accepted;
}

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

void Settings::InitCleanup() {
    if (m_pMain) {
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