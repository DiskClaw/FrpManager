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

    std::wstring cmdLine = std::wstring(L"\"") + exePath + L"\" -c \"" + configPath + L"\"";
    std::vector<wchar_t> buffer(cmdLine.begin(), cmdLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    BOOL ok = CreateProcessW(
        nullptr,
        buffer.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        workingDir.empty() ? nullptr : workingDir.c_str(),
        &si,
        &pi_
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
    generation_++;

    auto* param = new ThreadParam{ this, generation_ };
    thread_ = CreateThread(nullptr, 0, ReadPipeThread, param, 0, nullptr);
    if (!thread_) {
        delete param;
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
        exitNotified_ = true;
        thread = thread_;
        process = pi_.hProcess;
        pipe = readPipe_;
        thread_ = nullptr;
        pi_.hProcess = nullptr;
        readPipe_ = nullptr;
        pendingLine_.clear();
    }

    if (pipe) CloseHandle(pipe);

    if (process) {
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process, 0);
        }
        CloseHandle(process);
    }

    if (thread) CloseHandle(thread);
}

bool FrpProcess::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !pi_.hProcess) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(pi_.hProcess, &code)) return false;
    return (code == STILL_ACTIVE);
}

DWORD WINAPI FrpProcess::ReadPipeThread(LPVOID param) {
    auto* tp = static_cast<ThreadParam*>(param);
    FrpProcess* self = tp->self;
    unsigned int gen = tp->generation;
    delete tp;
    self->ReadOutput(self->readPipe_, gen);
    return 0;
}

void FrpProcess::ReadOutput(HANDLE pipe, unsigned int generation) {
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

    Cleanup(generation);
}

void FrpProcess::Cleanup(unsigned int generation) {
    HANDLE process = nullptr, pipe = nullptr;
    DWORD exitCode = 0;
    bool shouldNotify = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation != generation_) return;
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
