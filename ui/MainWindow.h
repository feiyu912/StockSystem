#pragma once

#include "AppMessages.h"

ATOM RegisterMainWindowClass(HINSTANCE instance);
BOOL InitMainWindow(HINSTANCE instance, int showCommand);
LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
