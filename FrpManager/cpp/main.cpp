#include <windows.h>
#include "Window.h"

// 自动检测系统语言
static bool IsChineseLocale() {
    LANGID langId = GetSystemDefaultUILanguage();
    return PRIMARYLANGID(langId) == LANG_CHINESE;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPWSTR pCmdLine, int nCmdShow) {
    bool zh = IsChineseLocale();
    const wchar_t* errTitle = zh ? L"错误" : L"Error";

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, zh ? L"COM 初始化失败" : L"COM init failed", errTitle, MB_ICONERROR);
        return 1;
    }

    MainWindow win;
    win.SetHideMode(wcsstr(pCmdLine, L"--hide") != nullptr);
    if (!win.RegisterClass()) {
        MessageBoxW(nullptr, zh ? L"注册窗口类失败" : L"Register class failed", errTitle, MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    HWND hwnd = win.CreateWindow_();
    if (!hwnd) {
        // 单实例检测到已有实例时返回0（非错误），--hide模式下也返回0
        CoUninitialize();
        return 0;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
