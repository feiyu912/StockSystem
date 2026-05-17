#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

constexpr UINT WM_APP_ENGINE_UPDATE = WM_APP + 1;

ATOM RegisterMainWindowClass(HINSTANCE instance);
BOOL InitMainWindow(HINSTANCE instance, int showCommand);
LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
