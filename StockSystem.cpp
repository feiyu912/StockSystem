// StockSystem.cpp : native Win32 program entry and message loop.

#include "framework.h"
#include "ui/MainWindow.h"
#include "Resource.h"

#include <commctrl.h>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "concrt.lib")

int APIENTRY wWinMain(_In_ HINSTANCE instance,
    _In_opt_ HINSTANCE previousInstance,
    _In_ LPWSTR commandLine,
    _In_ int showCommand)
{
    UNREFERENCED_PARAMETER(previousInstance);
    UNREFERENCED_PARAMETER(commandLine);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    RegisterMainWindowClass(instance);
    if (!InitMainWindow(instance, showCommand)) {
        return FALSE;
    }

    HACCEL accelerators = LoadAccelerators(instance, MAKEINTRESOURCE(IDC_STOCKSYSTEM));
    MSG message;
    while (GetMessage(&message, nullptr, 0, 0)) {
        if (!TranslateAccelerator(message.hwnd, accelerators, &message)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    }

    return static_cast<int>(message.wParam);
}
