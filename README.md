# StockSystem

Pure Win32 C++ stock trading and quantitative backtest simulator.

## What This Version Demonstrates

- Windows native entry point: `wWinMain`, window class registration, window procedure, and message loop.
- Native controls: buttons, combo box, edit boxes, list boxes, and status text.
- Custom drawing with GDI: dark trading-style dashboard, K-line chart, moving averages, and equity curve.
- Market-style layout: quote header, orange period toolbar, K-line chart, volume panel, technical indicator panel, and right-side quote/fund-flow summary.
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
- Concurrency monitor: the right-side panel shows worker thread states, thread ids, queue activity counters, async optimization tasks, and UI `PostMessage` counts.
- Multi-user load monitor: a background user-load thread launches parallel `std::async` simulated user requests and displays active users, request counts, per-user equity, and latency.
- Local data store: market replay and simulated user requests query `data/akshare_export_TEST_SH.csv`, a local AkShare-style CSV cache generated on first run when no imported file exists.
- A sample `data/akshare_export_TEST_SH.csv` is included so the project demonstrates local data loading immediately.
- To use real exported data, replace `data/akshare_export_TEST_SH.csv` with AkShare output using columns `timestamp,open,high,low,close,volume`; no Python or network access is needed at runtime.
- No Qt dependency.

## Build

Open `StockSystem.vcxproj` in Visual Studio 2022 and build `Debug|x64` or `Release|x64`.

Expected environment:

- Visual Studio 2022
- Platform toolset `v143`
- Windows SDK 10.0

The project builds as a normal Windows subsystem executable and does not require Qt, Python, or DuiLib.

## Files

- `StockSystem.cpp`: Win32 program entry and message loop.
- `MainWindow.h/.cpp`: native controls, GDI chart drawing, mouse/keyboard messages, and UI refresh.
- `BacktestEngine.h/.cpp`: worker threads, market replay, strategy scheduling, order matching, optimization, and UI snapshots.
- `TradingCore.h/.cpp`: account, order, trade, risk manager, order book, and moving-average strategy.
- `Indicators.h/.cpp`: parallel moving-average calculation with `std::execution::par`.
- `StockSystem.vcxproj`: Visual Studio project configured for Windows native build.
- `StockSystem.rc`, `Resource.h`, `StockSystem.ico`, `small.ico`: Windows resources.
- `framework.h`, `targetver.h`, `StockSystem.h`: standard Win32 project headers.

## Runtime Notes

- Market data is synthetic. `SimulationEngine::marketLoop` generates random OHLC bars in memory for the demo, so the app does not need network access or a data vendor.
- `Orders` shows strategy orders after risk and matching, including status such as `Filled`, `Part filled`, `Open limit`, or `Rejected by risk`.
- `Trades` shows actual matched executions. A trade changes cash, position, equity, and drawdown.
- `Strategy` currently uses the moving-average crossover implementation. `Short MA` and `Long MA` control the two moving average windows used by that strategy.
- The chart can be viewed as `1m`, `5m`, `15m`, `30m`, `60m`, `Day`, or `Week` by aggregating the synthetic bars in memory.
- Replay speed can be changed while the simulation is running.
- The lower indicator panel supports `Volume`, `RSI`, `KDJ`, and `MACD`; the main chart shows MA5, MA10, MA20, and MA60.
