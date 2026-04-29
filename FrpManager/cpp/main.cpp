#include <windows.h>
#include "Window.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPWSTR pCmdLine, int nCmdShow) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"COM 初始化失败", L"错误", MB_ICONERROR);
        return 1;
    }

    MainWindow win;
    if (!win.RegisterClass()) {
        MessageBoxW(nullptr, L"注册窗口类失败", L"错误", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    HWND hwnd = win.CreateWindow_();
    if (!hwnd) {
        MessageBoxW(nullptr, L"创建窗口失败", L"错误", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}