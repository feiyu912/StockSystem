# StockSystem

StockSystem is a pure Win32 C++ stock trading simulator and quantitative backtest demo. It uses native Windows controls, GDI drawing, worker threads, local CSV market data, and several built-in strategies to show how a desktop trading/backtest system can be structured without Qt, MFC, DuiLib, or a web runtime.

## Highlights

- Pure native Windows application: `wWinMain`, window class registration, message loop, window procedure, and Win32 controls.
- Trading-style dashboard drawn with GDI: quote header, period toolbar, K-line chart, moving averages, volume/indicator panel, orders, trades, logs, equity curve, and side metrics.
- Multi-threaded simulation engine: market replay, strategy generation, order matching, optimization, and simulated user load run on worker threads.
- UI-thread marshaling with `PostMessage`, so worker threads update the interface through the main window thread.
- Local market data store backed by AkShare-style CSV files under `data/`.
- Strategy selector with moving-average crossover, breakout, mean reversion, momentum, RSI reversal, and Bollinger band strategies.
- Technical indicators: volume, RSI, KDJ, and MACD.
- Replay controls for symbol, period, speed, initial cash, strategy, and strategy windows.
- Concurrency monitor showing worker states, thread ids, queue counts, async optimization activity, UI message counts, and simulated user latency.

## Architecture

The project is split into a small Win32 shell, a simulation engine, trading-domain logic, indicators, and a local data store:

- `StockSystem.cpp` starts the Windows application, registers the main window class, creates the main window, and runs the message loop.
- `MainWindow.cpp` owns the UI state, child controls, GDI rendering, chart interaction, keyboard/mouse handling, and refresh flow.
- `AppMessages.h` defines the custom `WM_APP_ENGINE_UPDATE` message used by worker threads to request UI refreshes.
- `BacktestEngine.cpp` owns the runtime engine: start, pause, reset, stop, snapshots, replay, strategy scheduling, matching, optimization, and simulated user load.
- `TradingCore.cpp` owns reusable trading primitives: blocking queues, account/equity tracking, risk checks, order book matching, orders, trades, and strategy implementations.
- `Indicators.cpp` owns calculation helpers for moving averages, RSI, KDJ, and MACD. Moving-average calculation uses `std::execution::par`.
- `MarketDataStore.cpp` loads local CSV files into memory and serves replay/user-load range queries.
- `StockSystem.rc`, icons, and `Resource.h` provide the native Windows application resources.

Runtime communication is deliberately simple: the market thread pushes bars into a `BlockingQueue`, the strategy thread consumes market data and emits orders, the matching thread turns accepted orders into trades through the `OrderBook`, and the UI reads immutable snapshots on the main thread. Worker threads never directly repaint controls; they call `PostMessage` and let the main window handle refreshes.

## Screenshot-Oriented Feature Tour

The main window is organized like a market terminal:

- Top area: selected symbol, quote snapshot, period buttons, replay speed, and indicator selector.
- Center chart: K-line candles with MA5, MA10, MA20, and MA60 overlays.
- Lower chart: switchable volume, RSI, KDJ, or MACD panel.
- Left/control area: start, pause, reset, capital, strategy, short/long windows, and status.
- Tables: generated orders and matched trades.
- Right panel: account metrics, drawdown, worker-thread state, queue counters, and simulated multi-user load.

The UI controls include:

- `Start`, `Pause`, `Reset`, and `Optimize` buttons.
- Symbol, period, replay speed, indicator, and strategy combo boxes.
- Initial cash, short window, and long window inputs.
- Orders, trades, and logs list boxes.
- A bottom status bar with the selected symbol, period, latest price, cash, position, equity, and drawdown.

## Build

Open `StockSystem.vcxproj` or `StockSystem.slnx` in Visual Studio and build `Debug|x64` or `Release|x64`.

Expected environment:

- Visual Studio 2022
- Windows SDK 10.0
- Platform toolset `v143`
- C++20

Configured project platforms:

- `Debug|Win32`
- `Release|Win32`
- `Debug|x64`
- `Release|x64`

The project builds as a normal Windows subsystem executable. Runtime does not require Python, AkShare, network access, Qt, MFC, or DuiLib.

## Run

After building, run the generated executable:

```powershell
.\x64\Debug\StockSystem.exe
```

The application starts with the sample CSV data in `data/akshare_export_TEST_SH.csv`. If no imported stock list exists, the UI falls back to the bundled sample.

## Controls

- `Space`: start or pause replay
- `R`: reset the simulation
- `Esc`: stop
- `F1`: show help
- Mouse wheel over the chart: zoom visible K-line bars
- Left click on the chart: inspect OHLC data near the cursor
- Drag the main chart left or right: pan through visible history
- Click the user-load section: expand or collapse simulated users when more rows are available

## Market Data

StockSystem reads local CSV files, so the simulator is deterministic and can run offline. CSV rows should use this format:

```csv
symbol,timestamp,open,high,low,close,volume
TEST.SH,1,12.30,12.80,12.10,12.65,360000
```

By convention, generated stock files are named:

```text
data/akshare_export_<symbol>.csv
```

The optional stock list is:

```text
data/stocks.csv
```

with rows containing:

```csv
symbol,name,path
600519.SH,Kweichow Moutai,data\akshare_export_600519_SH.csv
```

If `data/stocks.csv` is missing, the program scans `data/` for `akshare_export_*.csv` files and also keeps the bundled sample available.

## Fetch Real A-Share Data

The simulator itself does not need Python, but the repository includes a helper script for exporting local CSV files from AkShare:

```powershell
pip install akshare
python scripts\fetch_stock_data.py 600519 000001 300750
```

Useful options:

```powershell
python scripts\fetch_stock_data.py 600519 --start 20200101 --end 20260520 --output-dir data
```

The script writes one CSV per symbol and updates `data/stocks.csv`.

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

The `Optimize` button launches a background optimization thread. It evaluates 9 moving-average parameter combinations with `std::async` tasks, then logs the best short/long window pair by total return and max drawdown.

The right-side concurrency panel shows:

- Market, strategy, matching, optimization, and user-load thread status.
- Thread ids shortened for display.
- Queue/event counters.
- `PostMessage` refresh count.
- 8 simulated users issuing parallel quote/backtest-style requests and reporting equity/request/latency snapshots.

## Repository Layout

- `AppMessages.h`: custom Win32 app message id.
- `StockSystem.cpp`: Win32 entry point and message loop.
- `MainWindow.h/.cpp`: native controls, layout, GDI drawing, chart interactions, and UI refresh.
- `BacktestEngine.h/.cpp`: worker threads, replay loop, strategy loop, matching loop, optimization, snapshots, and user-load simulation.
- `TradingCore.h/.cpp`: blocking queue, account model, orders, trades, risk manager, order book, and strategy implementations.
- `Indicators.h/.cpp`: moving averages, RSI, KDJ, and MACD calculations.
- `MarketDataStore.h/.cpp`: local CSV loading and range queries.
- `scripts/fetch_stock_data.py`: optional AkShare export helper.
- `data/akshare_export_TEST_SH.csv`: bundled sample data.
- `StockSystem.vcxproj`, `StockSystem.vcxproj.filters`, `StockSystem.slnx`: Visual Studio project files.
- `StockSystem.rc`, `Resource.h`, `StockSystem.ico`, `small.ico`: Windows resources.
- `framework.h`, `targetver.h`, `StockSystem.h`: standard Win32 project headers.

## Notes

- Visual Studio's `Source Files`, `Header Files`, and `Resource Files` groups come from `StockSystem.vcxproj.filters`; they are project filters, not real folders. GitHub shows the actual repository file layout.
- The default project toolset is `v143`. If your local Visual Studio installation only has a newer toolset, either install the v143 build tools or retarget the project in Visual Studio.
- The replay engine loads local data into memory, then uses it for chart replay and simulated user quote/backtest requests.
- Orders can be filled, partially filled, left open as limit orders, or rejected by risk checks.
