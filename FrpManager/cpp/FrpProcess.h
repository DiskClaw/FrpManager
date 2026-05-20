#pragma once
#include <windows.h>
#include <mutex>
#include <string>

enum class FrpMode { Client, Server };

struct IFrpProcessCallback {
    virtual void OnOutput(FrpMode mode, const char* line, int len) = 0;
    virtual void OnExit(FrpMode mode, DWORD exitCode) = 0;
    virtual ~IFrpProcessCallback() {}
};

class FrpProcess {
public:
    explicit FrpProcess(FrpMode mode);
    ~FrpProcess();

    FrpProcess(const FrpProcess&) = delete;
    FrpProcess& operator=(const FrpProcess&) = delete;

    bool Start(const std::wstring& exePath, const std::wstring& configPath, const std::wstring& workingDir);
    void Stop();
    bool IsRunning() const;
    FrpMode Mode() const { return mode_; }

    void SetCallback(IFrpProcessCallback* cb);

private:
    struct ThreadParam {
        FrpProcess* self;
        unsigned int generation;
    };

    static DWORD WINAPI ReadPipeThread(LPVOID param);
    void ReadOutput(HANDLE pipe, unsigned int generation);
    void NotifyExit(DWORD exitCode);
    void Cleanup(unsigned int generation);

    FrpMode mode_;
    PROCESS_INFORMATION pi_ = {};
    HANDLE readPipe_ = nullptr;
    HANDLE thread_ = nullptr;
    IFrpProcessCallback* cb_ = nullptr;
    bool running_ = false;
    bool exitNotified_ = false;
    unsigned int generation_ = 0;
    mutable std::mutex mutex_;
    std::string pendingLine_;
};
