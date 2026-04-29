#include "FrpProcess.h"
#include <vector>

FrpProcess::FrpProcess(FrpMode mode) : mode_(mode) {
    ZeroMemory(&pi_, sizeof(pi_));
}

FrpProcess::~FrpProcess() {
    Stop();
}

void FrpProcess::SetCallback(IFrpProcessCallback* cb) {
    cb_ = cb;
}

bool FrpProcess::Start(const std::wstring& exePath,
    const std::wstring& configPath,
    const std::wstring& workingDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return false;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdLine = L"\"" + exePath + L"\" -c \"" + configPath + L"\"";
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

    if (process) {
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process, 0);
        }
    }

    if (thread) {
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }

    if (process) CloseHandle(process);
    if (pipe)    CloseHandle(pipe);
}

bool FrpProcess::IsRunning() const {
    if (!running_ || !pi_.hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(pi_.hProcess, &code)) return false;
    return (code == STILL_ACTIVE);
}

DWORD WINAPI FrpProcess::ReadPipeThread(LPVOID param) {
    auto* self = static_cast<FrpProcess*>(param);
    self->ReadOutput(self->readPipe_);
    return 0;
}

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

void FrpProcess::NotifyExit(DWORD exitCode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (exitNotified_) return;
    exitNotified_ = true;
    if (cb_) cb_->OnExit(mode_, exitCode);
}