# StockSystem

StockSystem is a pure Win32 C++ stock trading simulator and quantitative backtest demo. It uses native Windows controls, GDI drawing, worker threads, local CSV market data, and built-in strategies to show how a desktop trading/backtest system can be structured without Qt, MFC, DuiLib, or a web runtime.

The current project is centered on real A-share daily-bar data. The app loads all symbols listed in `data/stocks.csv` into one shared market replay clock, then lets the UI switch the visible symbol without restarting market time.

## Highlights

- Pure native Windows application: `wWinMain`, window class registration, message loop, window procedure, and Win32 controls.
- Trading-style dashboard drawn with GDI: quote header, period selector, K-line chart, moving averages, volume/indicator panel, orders, trades, logs, equity curve, and side metrics.
- Real local A-share daily data exported from AkShare-compatible CSV files under `data/`.
- Shared market timeline: `Start` advances all loaded stocks together, `Pause` freezes market time, and `Reset` returns the whole simulation to its initial state.
- Symbol selector changes the viewed stock only; it does not restart or pause the replay clock.
- Period selector is aligned with the current daily data source: `Day`, `Week`, and `Month`.
- Strategy selector with moving-average crossover, breakout, mean reversion, momentum, RSI reversal, and Bollinger band strategies.
- Technical indicators: volume, RSI, KDJ, and MACD.
- Multi-threaded simulation engine: market replay, strategy generation, matching, optimization, and virtual user simulation run on worker threads.
- Virtual strategy users rotate across the loaded stocks and strategies to show concurrent multi-symbol activity.

## Architecture

The codebase is split into a Win32 shell, UI layer, core simulation logic, local data scripts, and a reserved Python research layer:

- `StockSystem.cpp` starts the Windows application, registers the main window class, creates the main window, and runs the message loop.
- `ui/MainWindow.cpp` owns the UI state, child controls, GDI rendering, chart interaction, keyboard/mouse handling, and refresh flow.
- `ui/AppMessages.h` defines the custom UI update message.
- `core/BacktestEngine.cpp` owns the runtime engine: start, pause, reset, stop, snapshots, shared market replay, strategy scheduling, matching, optimization, and virtual user simulation.
- `core/TradingCore.cpp` owns reusable trading primitives: blocking queues, account/equity tracking, risk checks, order book matching, orders, trades, and strategy implementations.
- `core/Indicators.cpp` owns calculation helpers for moving averages, RSI, KDJ, and MACD.
- `core/MarketDataStore.cpp` loads local CSV files into memory and serves range queries.
- `scripts/` contains data fetch and normalization utilities.
- `python/` is reserved for future Python strategy and research interfaces.

Runtime communication is deliberately simple: the market thread advances a shared timeline, publishes the currently viewed symbol to the UI snapshot, pushes market events into a queue, and worker threads handle strategy and matching. Worker threads never repaint controls directly; they notify the main window and the UI reads immutable snapshots.

## Build

Open `StockSystem.vcxproj` or `StockSystem.slnx` in Visual Studio and build `Debug|x64` or `Release|x64`.

Expected environment:

- Visual Studio 2022 or newer
- Windows SDK 10.0
- C++20

The project file defaults to toolset `v143`. On a machine with a newer toolset only, build with an override such as:

```powershell
& "D:\visual studio\MSBuild\Current\Bin\amd64\MSBuild.exe" "G:\c\StockSystem\StockSystem.vcxproj" /p:Configuration=Debug /p:Platform=x64 /p:PlatformToolset=v145 /m
```

## Run

After building, run:

```powershell
.\x64\Debug\StockSystem.exe
```

The app reads `data/stocks.csv` at startup. The current real-data set contains:

- `000001.SZ` - Ping An Bank
- `300750.SZ` - CATL
- `600519.SH` - Kweichow Moutai

## Controls

- `Start`: start the shared market timeline for all loaded stocks.
- `Pause`: pause or resume the shared market timeline.
- `Reset`: return the whole simulation to the initial state.
- Symbol combo box: switch the visible stock only.
- Period combo box: aggregate daily bars as `Day`, `Week`, or `Month`.
- Strategy combo box: choose the strategy used by the main strategy thread.
- `Space`: start or pause replay.
- `R`: reset the simulation.
- `Esc`: stop.
- `F1`: show help.
- Mouse wheel over the chart: zoom visible K-line bars.
- Left click on the chart: inspect OHLC data near the cursor.
- Drag the main chart left or right: pan through visible history.
- Click the virtual-user section: expand or collapse simulated users when more rows are available.

## Market Data

StockSystem reads deterministic local CSV files. CSV rows use this normalized schema:

```csv
symbol,timestamp,open,high,low,close,volume
000001.SZ,20200102,14.18,14.48,14.08,14.40,1530232
```

Files are named:

```text
data/akshare_export_<symbol>.csv
```

The stock list is:

```text
data/stocks.csv
```

with rows:

```csv
symbol,name,path
000001.SZ,平安银行,data\akshare_export_000001_SZ.csv
300750.SZ,宁德时代,data\akshare_export_300750_SZ.csv
600519.SH,贵州茅台,data\akshare_export_600519_SH.csv
```

`timestamp` should be a real trading date in `YYYYMMDD` form. If old files still contain `1,2,3...`, rerun the fetch script with the current version.

## Fetch And Prepare Data

The app itself does not require Python at runtime, but the repository includes Python helpers for data work.

Install AkShare in the Python environment used for fetching:

```powershell
pip install akshare
```

Fetch the current three-stock daily-bar data:

```powershell
python scripts\fetch_stock_data.py 600519 000001 300750 --start 20200101 --end 20260520 --output-dir data --no-proxy --retries 5
```

Normalize, validate, sort, deduplicate, regenerate `data/stocks.csv`, and write a compact report:

```powershell
python scripts\prepare_market_data.py --write
```

The report is written to:

```text
data/market_data_report.csv
```

## Strategies

The `Strategy` selector supports:

- Moving-average crossover
- Breakout channel
- Mean reversion
- Momentum tracking
- RSI reversal
- Bollinger band

`Short MA` and `Long MA` are used as moving-average windows and as lookback windows for the other strategies. Generated strategy orders pass through risk checks and order matching before appearing in the trade list.

## Backtest And Matching Model

The simulator tracks:

- Cash, position, equity, peak equity, and max drawdown.
- Orders generated by strategies after risk validation.
- Trades produced by the order book.
- Total fees, including commission, transfer fee, and stamp duty logic in the engine.
- Open, filled, partially filled, and risk-rejected order states.

The `Optimize` button launches a background optimization thread. It evaluates moving-average parameter combinations with `std::async` tasks, then logs the best short/long window pair by total return and max drawdown.

The right-side concurrency panel shows:

- Market, strategy, matching, optimization, and virtual-user thread status.
- Thread ids shortened for display.
- Queue/event counters.
- UI update notification count.
- 8 virtual users assigned to existing stocks and rotating strategies.

## Repository Layout

- `core/BacktestEngine.h/.cpp`: worker threads, shared market replay loop, strategy loop, matching loop, optimization, snapshots, and virtual users.
- `core/TradingCore.h/.cpp`: blocking queue, account model, orders, trades, risk manager, order book, and strategies.
- `core/Indicators.h/.cpp`: moving averages, RSI, KDJ, and MACD calculations.
- `core/MarketDataStore.h/.cpp`: local CSV loading and range queries.
- `ui/AppMessages.h`: custom Win32 app message id.
- `ui/MainWindow.h/.cpp`: native controls, layout, GDI drawing, chart interactions, and UI refresh.
- `scripts/fetch_stock_data.py`: AkShare daily-bar export helper with retry and no-proxy options.
- `scripts/prepare_market_data.py`: CSV validation, normalization, stock-list generation, and report generation.
- `python/README.md`: placeholder for future Python strategy and research interfaces.
- `data/`: local daily-bar CSV files, `stocks.csv`, and `market_data_report.csv`.
- `StockSystem.vcxproj`, `StockSystem.vcxproj.filters`, `StockSystem.slnx`: Visual Studio project files.
- `StockSystem.rc`, `Resource.h`, `StockSystem.ico`, `small.ico`: Windows resources.
- `framework.h`, `targetver.h`, `StockSystem.h`: standard Win32 project headers.

## Notes

- Current real data is daily-bar data. Minute data is not included yet.
- `Day`, `Week`, and `Month` are display aggregations over local daily bars.
- `000001.SZ` is Ping An Bank, not the Shanghai Composite Index. The Shanghai Composite is `000001.SH` and should be fetched through an index-data path if needed later.
- Visual Studio's filters are project filters, not necessarily physical folders.
- The replay engine loads local data into memory before replay.
