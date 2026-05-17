# StockSystem

Pure Win32 C++ stock trading and quantitative backtest simulator.

## What This Version Demonstrates

- Windows native entry point: `wWinMain`, window class registration, window procedure, and message loop.
- Native controls: buttons, combo box, edit boxes, list boxes, and status text.
- Custom drawing with GDI: dark trading-style dashboard, K-line chart, moving averages, and equity curve.
- Mouse and keyboard messages:
  - `Space`: start or pause the backtest
  - `R`: reset
  - `Esc`: stop
  - `F1`: help
  - Left click on chart: inspect OHLC data
  - Mouse wheel: zoom visible K-line bars
- Worker threads: market replay, strategy signal generation, and order matching run on `std::thread`.
- UI-thread marshaling: workers call `PostMessage(..., WM_APP + 1, ...)` and the main thread refreshes controls.
- Parallel calculation: moving averages are computed with C++17 `std::execution::par`.
- No Qt dependency.

## Build

Open `StockSystem.vcxproj` in Visual Studio 2022 and build `Debug|x64` or `Release|x64`.

Expected environment:

- Visual Studio 2022
- Platform toolset `v143`
- Windows SDK 10.0

The project builds as a normal Windows subsystem executable and does not require Qt, Python, or DuiLib.

## Files

- `StockSystem.cpp`: Win32 UI, trading core, backtest engine, threading, chart drawing, and message handling.
- `StockSystem.vcxproj`: Visual Studio project configured for Windows native build.
- `StockSystem.rc`, `Resource.h`, `StockSystem.ico`, `small.ico`: Windows resources.
- `framework.h`, `targetver.h`, `StockSystem.h`: standard Win32 project headers.
