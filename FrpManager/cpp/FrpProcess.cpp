#include "FrpProcess.h"
#include <vector>

FrpProcess::FrpProcess(FrpMode mode) : mode_(mode) {
    ZeroMemory(&pi_, sizeof(pi_));
}

FrpProcess::~FrpProcess() {
    Stop();
}

// 设置进程回调（OnOutput、OnExit）
void FrpProcess::SetCallback(IFrpProcessCallback* cb) {
    cb_ = cb;
}

// 启动 frpc/frps 进程，捕获 stdout/stderr 输出到回调
bool FrpProcess::Start(const std::wstring& exePath,
    const std::wstring& configPath,
    const std::wstring& workingDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return false;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" -c \"" + configPath + L"\"";
    std::vector<wchar_t> buffer(cmdLine.begin(), cmdLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;               // 隐藏窗口

    BOOL ok = CreateProcessW(
        nullptr,                            // lpApplicationName
        buffer.data(),                      // lpCommandLine
        nullptr,                            // lpProcessAttributes
        nullptr,                            // lpThreadAttributes
        TRUE,                               // bInheritHandles
        CREATE_NO_WINDOW,                   // dwCreationFlags
        nullptr,                            // lpEnvironment
        workingDir.empty() ? nullptr : workingDir.c_str(), // lpCurrentDirectory
        &si,                                // lpStartupInfo
        &pi_                                // lpProcessInformation
    );

    CloseHandle(hWritePipe);

    if (!ok) {
        CloseHandle(hReadPipe);
        return false;
    }

    running_ = true;
    exitNotified_ = false;
    readPipe_ = hReadPipe;
    pendingLine_.clear();

    thread_ = CreateThread(nullptr, 0, ReadPipeThread, this, 0, nullptr);
    if (!thread_) {
        TerminateProcess(pi_.hProcess, 1);
        CloseHandle(pi_.hProcess);
        CloseHandle(pi_.hThread);
        ZeroMemory(&pi_, sizeof(pi_));
        CloseHandle(hReadPipe);
        readPipe_ = nullptr;
        running_ = false;
        return false;
    }

    if (pi_.hThread) {
        CloseHandle(pi_.hThread);
        pi_.hThread = nullptr;
    }

    return true;
}

// 停止进程：先关管道解除读线程阻塞，再终止进程，最后等待线程退出
void FrpProcess::Stop() {
    HANDLE thread = nullptr, process = nullptr, pipe = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !pi_.hProcess) return;
        running_ = false;
        thread = thread_;
        process = pi_.hProcess;
        pipe = readPipe_;
        thread_ = nullptr;
        pi_.hProcess = nullptr;
        readPipe_ = nullptr;
        pendingLine_.clear();
    }

    // 先关管道：让 ReadFile 立刻返回，读线程不会卡住
    if (pipe) CloseHandle(pipe);

    if (process) {
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process, 0);
        }
    }

    // 管道已关，读线程应该很快退出，2 秒足够
    if (thread) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }

    if (process) CloseHandle(process);
}

// 检查进程是否仍在运行（加锁避免 data race）
bool FrpProcess::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !pi_.hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(pi_.hProcess, &code)) return false;
    return (code == STILL_ACTIVE);
}

// 管道读取线程入口，转发到成员函数
DWORD WINAPI FrpProcess::ReadPipeThread(LPVOID param) {
    auto* self = static_cast<FrpProcess*>(param);
    self->ReadOutput(self->readPipe_);
    return 0;
}

// 从管道读取输出，按行切割并回调 OnOutput
void FrpProcess::ReadOutput(HANDLE pipe) {
    char buf[4096];
    DWORD bytesRead;

    for (;;) {
        BOOL success = ReadFile(pipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
        if (!success || bytesRead == 0) break;

        buf[bytesRead] = '\0';
        for (DWORD i = 0; i < bytesRead; ++i) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                if (!pendingLine_.empty()) {
                    if (cb_) cb_->OnOutput(mode_, pendingLine_.c_str(), (int)pendingLine_.size());
                    pendingLine_.clear();
                }
                if (buf[i] == '\r' && i + 1 < bytesRead && buf[i + 1] == '\n') ++i;
            }
            else {
                pendingLine_.push_back(buf[i]);
            }
        }
    }

    if (!pendingLine_.empty()) {
        if (cb_) cb_->OnOutput(mode_, pendingLine_.c_str(), (int)pendingLine_.size());
        pendingLine_.clear();
    }

    Cleanup();
}

// 清理进程句柄和管道，回调 OnExit
void FrpProcess::Cleanup() {
    HANDLE process = nullptr, pipe = nullptr;
    DWORD exitCode = 0;
    bool shouldNotify = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !pi_.hProcess && !readPipe_) return;
        running_ = false;
        process = pi_.hProcess;
        pipe = readPipe_;
        if (process) GetExitCodeProcess(process, &exitCode);
        pi_.hProcess = nullptr;
        readPipe_ = nullptr;
        pendingLine_.clear();
        shouldNotify = true;
    }

    if (pipe) CloseHandle(pipe);
    if (process) CloseHandle(process);

    if (shouldNotify) NotifyExit(exitCode);
}

// 通知回调进程已退出（防重入）
void FrpProcess::NotifyExit(DWORD exitCode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exitNotified_) return;
    exitNotified_ = true;
    if (cb_) cb_->OnExit(mode_, exitCode);
}