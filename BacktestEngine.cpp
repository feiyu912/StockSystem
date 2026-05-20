#include "BacktestEngine.h"
#include "AppMessages.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace {

std::wstring percent(double value)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << value * 100.0 << L"%";
    return ss.str();
}

std::wstring money(double value)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << value;
    return ss.str();
}

std::wstring threadIdText()
{
    std::wstringstream ss;
    ss << std::this_thread::get_id();
    return ss.str();
}

constexpr double kCommissionRate = 0.0003;
constexpr double kMinCommission = 5.0;
constexpr double kStampDutyRate = 0.0005;
constexpr double kTransferFeeRate = 0.00001;

} // namespace

SimulationEngine::~SimulationEngine()
{
    stop();
}

void SimulationEngine::configure(double initialCash, int shortWindow, int longWindow, StrategyKind strategyKind, const std::wstring& dataPath, const std::string& symbol)
{
    std::lock_guard<std::mutex> lock(mutex_);
    initialCash_ = initialCash;
    shortWindow_ = std::max(1, shortWindow);
    longWindow_ = std::max(shortWindow_ + 1, longWindow);
    strategyKind_ = strategyKind;
    dataPath_ = dataPath.empty() ? L"data\\akshare_export_TEST_SH.csv" : dataPath;
    symbol_ = symbol.empty() ? "TEST.SH" : symbol;
}

void SimulationEngine::setReplayDelay(int milliseconds)
{
    replayDelayMs_ = std::clamp(milliseconds, 10, 1000);
}

void SimulationEngine::start(HWND notifyWindow)
{
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resetLocked();
        running_ = true;
        paused_ = false;
        notifyWindow_ = notifyWindow;
        logs_.push_back(L"Engine started: market replay, strategy and matching run in worker threads.");
        logs_.push_back(L"Parallel indicator demo enabled: moving averages use std::execution::par.");
    }

    marketThread_ = std::thread(&SimulationEngine::marketLoop, this);
    strategyThread_ = std::thread(&SimulationEngine::strategyLoop, this);
    matchingThread_ = std::thread(&SimulationEngine::matchingLoop, this);
    userLoadThread_ = std::thread(&SimulationEngine::userLoadLoop, this);
}

void SimulationEngine::pauseOrResume()
{
    const bool paused = !paused_.load();
    paused_ = paused;
    addLog(paused ? L"Replay paused." : L"Replay resumed.");
}

void SimulationEngine::stop()
{
    const bool wasRunning = running_.exchange(false);
    if (!wasRunning && !marketThread_.joinable() && !strategyThread_.joinable()
        && !matchingThread_.joinable() && !optimizationThread_.joinable() && !userLoadThread_.joinable()) {
        return;
    }
    paused_ = false;
    marketQueue_.close();
    orderQueue_.close();
    if (marketThread_.joinable()) {
        marketThread_.join();
    }
    if (strategyThread_.joinable()) {
        strategyThread_.join();
    }
    if (matchingThread_.joinable()) {
        matchingThread_.join();
    }
    if (userLoadThread_.joinable()) {
        userLoadThread_.join();
    }
    if (optimizationThread_.joinable()) {
        optimizationThread_.join();
    }
    marketQueue_.reset();
    orderQueue_.reset();
    addLog(L"Engine stopped.");
}

void SimulationEngine::reset()
{
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resetLocked();
        logs_.push_back(L"Simulation reset.");
    }
    notify();
}

UiSnapshot SimulationEngine::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return UiSnapshot{ bars_, prices_, equities_, orders_, trades_, logs_, account_, initialCash_, totalFees_, concurrency_, dataStore_.snapshot(), lastPrice_, running_, paused_ };
}

void SimulationEngine::optimize(HWND notifyWindow)
{
    if (optimizationThread_.joinable()) {
        optimizationThread_.join();
    }
    optimizationThread_ = std::thread([this, notifyWindow] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            concurrency_.optimizationThreadId = threadIdText();
            concurrency_.optimizationActive = true;
            concurrency_.optimizationTasks = 9;
        }
        addLog(L"Optimization thread started with 9 std::async parameter tasks.");
        std::vector<std::future<FastBacktestResult>> futures;
        for (int shortW : { 5, 10, 15 }) {
            for (int longW : { 30, 60, 90 }) {
                futures.push_back(std::async(std::launch::async, [=] {
                    return runFastBacktest(shortW, longW);
                }));
            }
        }

        FastBacktestResult best{ 0, 0, -100.0, 0.0 };
        for (auto& future : futures) {
            const FastBacktestResult result = future.get();
            if (result.totalReturn > best.totalReturn) {
                best = result;
            }
        }

        std::wstringstream ss;
        ss << L"Optimization best: MA(" << best.shortW << L"," << best.longW
           << L"), return " << percent(best.totalReturn)
           << L", max drawdown " << percent(best.drawdown) << L".";
        addLog(ss.str());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            concurrency_.optimizationActive = false;
            ++concurrency_.uiPostMessages;
        }
        PostMessage(notifyWindow, WM_APP_ENGINE_UPDATE, 0, 0);
    });
}

SimulationEngine::FastBacktestResult SimulationEngine::runFastBacktest(int shortW, int longW)
{
    Account account;
    account.reset(100000.0);
    MovingAverageStrategy strategy(shortW, longW);
    RiskManager risk;
    OrderBook book;
    double price = 100.0;
    std::mt19937 rng(shortW * 1000 + longW);
    std::normal_distribution<double> noise(0.0004, 0.008);
    const auto feeOf = [](const Trade& trade) {
        const double notional = trade.price * trade.volume;
        const double commission = std::max(kMinCommission, notional * kCommissionRate);
        const double transfer = notional * kTransferFeeRate;
        const double stampDuty = trade.isBuy ? 0.0 : notional * kStampDutyRate;
        return commission + transfer + stampDuty;
    };

    for (long long t = 1; t <= 500; ++t) {
        price = std::max(5.0, price * (1.0 + noise(rng)));
        book.seedLiquidity(price, t);
        MarketBar data;
        data.price = price;
        data.open = price;
        data.high = price;
        data.low = price;
        data.close = price;
        data.volume = 100 + static_cast<int>(rng() % 900);
        data.timestamp = t;

        auto order = strategy.onMarketData(data, account);
        if (order && risk.check(*order, account, price)) {
            for (const auto& trade : book.addOrder(*order)) {
                const double fee = feeOf(trade);
                if (trade.isBuy) {
                    account.cash -= trade.price * trade.volume + fee;
                    account.position += trade.volume;
                } else {
                    account.cash += trade.price * trade.volume - fee;
                    account.position -= trade.volume;
                }
            }
        }
        account.markToMarket(price);
    }
    return FastBacktestResult{ shortW, longW, account.equity / 100000.0 - 1.0, account.maxDrawdown };
}

void SimulationEngine::marketLoop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.marketThreadId = threadIdText();
        concurrency_.marketActive = true;
    }
    addLog(L"Market thread started: generates synthetic OHLC bars.");
    std::wstring dataPath;
    std::string symbol;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dataPath = dataPath_;
        symbol = symbol_;
    }
    dataStore_.loadOrCreate(dataPath, symbol);
    auto replayBars = dataStore_.allBars();
    {
        std::wstringstream ss;
        ss << L"Local data store loaded: " << dataPath << L", cached in memory for replay and user queries.";
        addLog(ss.str());
    }

    std::mt19937 rng(std::random_device{}());
    std::normal_distribution<double> noise(0.0002, 0.006);
    double price = 100.0;

    for (long long timestamp = 1; running_.load() && timestamp <= 2000; ++timestamp) {
        while (paused_.load() && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }

        if (replayBars.empty()) {
            const double open = price;
            const double close = std::max(5.0, open * (1.0 + noise(rng)));
            const double shadow = std::abs(close - open) + 0.20 + static_cast<double>(rng() % 40) / 100.0;
            MarketBar generated;
            generated.open = open;
            generated.high = std::max(open, close) + shadow * 0.55;
            generated.low = std::max(1.0, std::min(open, close) - shadow * 0.45);
            generated.close = close;
            generated.price = close;
            generated.volume = 100 + static_cast<int>(rng() % 900);
            generated.symbol = symbol;
            replayBars.push_back(generated);
            price = close;
        }

        MarketBar event = replayBars[static_cast<size_t>((timestamp - 1) % replayBars.size())];
        event.timestamp = timestamp;
        event.symbol = symbol;
        price = event.close;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastPrice_ = price;
            bars_.push_back(event);
            trim(bars_, 360);
            prices_.push_back(price);
            trim(prices_, 360);
            account_.markToMarket(lastPrice_);
            equities_.push_back(account_.equity);
            trim(equities_, 360);
            ++concurrency_.marketEvents;
        }

        marketQueue_.push(event);
        notify();
        std::this_thread::sleep_for(std::chrono::milliseconds(replayDelayMs_.load()));
    }

    running_ = false;
    marketQueue_.close();
    orderQueue_.close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.marketActive = false;
    }
    addLog(L"Market replay finished.");
}

void SimulationEngine::strategyLoop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.strategyThreadId = threadIdText();
        concurrency_.strategyActive = true;
    }
    addLog(L"Strategy thread started: consumes market queue and emits orders.");

    StrategyKind strategyKind;
    int shortWindow;
    int longWindow;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        strategyKind = strategyKind_;
        shortWindow = shortWindow_;
        longWindow = longWindow_;
    }

    MovingAverageStrategy maStrategy(shortWindow, longWindow);
    BreakoutStrategy breakoutStrategy(longWindow);
    MeanReversionStrategy meanReversionStrategy(longWindow);
    MomentumStrategy momentumStrategy(longWindow);
    RsiReversalStrategy rsiReversalStrategy(shortWindow);
    BollingerBandStrategy bollingerBandStrategy(longWindow);
    MarketBar data;
    while (marketQueue_.pop(data)) {
        const Account account = [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return account_;
        }();

        std::optional<Order> order;
        switch (strategyKind) {
        case StrategyKind::Breakout:
            order = breakoutStrategy.onMarketData(data, account);
            break;
        case StrategyKind::MeanReversion:
            order = meanReversionStrategy.onMarketData(data, account);
            break;
        case StrategyKind::Momentum:
            order = momentumStrategy.onMarketData(data, account);
            break;
        case StrategyKind::RsiReversal:
            order = rsiReversalStrategy.onMarketData(data, account);
            break;
        case StrategyKind::BollingerBand:
            order = bollingerBandStrategy.onMarketData(data, account);
            break;
        case StrategyKind::MovingAverage:
        default:
            order = maStrategy.onMarketData(data, account);
            break;
        }
        if (order) {
            orderQueue_.push(*order);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++concurrency_.strategySignals;
            }
            std::wstringstream ss;
            ss << L"Strategy signal: order #" << order->id << L" "
               << (order->isBuy ? L"BUY" : L"SELL") << L" "
               << order->volume << L" @ " << money(order->price);
            addLog(ss.str());
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.strategyActive = false;
    }
}

void SimulationEngine::matchingLoop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.matchingThreadId = threadIdText();
        concurrency_.matchingActive = true;
    }
    addLog(L"Matching thread started: consumes order queue and applies trades.");

    RiskManager risk;
    Order order;

    while (orderQueue_.pop(order)) {
        double lastPrice = 100.0;
        Account account;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastPrice = lastPrice_;
            account = account_;
        }

        if (!risk.check(order, account, lastPrice)) {
            recordOrder(order, L"Rejected by risk", 0, 0.0);
            std::wstringstream ss;
            ss << L"Risk rejected order #" << order.id << L".";
            addLog(ss.str());
            continue;
        }

        OrderBook book;
        const double simulatedMid = order.isBuy ? order.price - 0.10 : order.price + 0.10;
        book.seedLiquidity(simulatedMid, order.timestamp);
        const auto trades = book.addOrder(order);
        int filledVolume = 0;
        double notional = 0.0;
        for (const auto& trade : trades) {
            filledVolume += trade.volume;
            notional += trade.price * trade.volume;
            applyTrade(trade);
        }
        const double averageFill = filledVolume > 0 ? notional / filledVolume : 0.0;
        const std::wstring status = filledVolume == 0
            ? L"Open limit"
            : (filledVolume == order.volume ? L"Filled" : L"Part filled");
        recordOrder(order, status, filledVolume, averageFill);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++concurrency_.matchedOrders;
        }
        notify();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.matchingActive = false;
    }
}

void SimulationEngine::userLoadLoop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.userLoadThreadId = threadIdText();
        concurrency_.userLoadActive = true;
        concurrency_.users.clear();
        for (int i = 1; i <= 8; ++i) {
            concurrency_.users.push_back(ConcurrencySnapshot::UserSession{ i, 100000.0 + i * 800.0, 0, 0, true });
        }
        concurrency_.activeUsers = static_cast<int>(concurrency_.users.size());
    }
    addLog(L"User load thread started: 8 simulated users run parallel std::async quote/backtest requests.");

    int cycle = 0;
    while (running_.load()) {
        while (paused_.load() && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }

        std::vector<ConcurrencySnapshot::UserSession> baseUsers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            baseUsers = concurrency_.users;
        }

        std::vector<std::future<ConcurrencySnapshot::UserSession>> futures;
        futures.reserve(baseUsers.size());
        for (const auto& user : baseUsers) {
            futures.push_back(std::async(std::launch::async, [this, user, cycle] {
                ConcurrencySnapshot::UserSession updated = user;
                std::mt19937 localRng(static_cast<unsigned>(user.id * 7919 + cycle * 104729));
                std::normal_distribution<double> pnl(0.0, 120.0);
                const auto rows = dataStore_.queryRange(static_cast<size_t>((cycle * 17 + user.id * 31) % 1000), 120);
                double localPnl = pnl(localRng);
                if (!rows.empty()) {
                    localPnl += (rows.back().close - rows.front().open) * 10.0;
                }
                updated.equity = std::max(1000.0, updated.equity + localPnl);
                updated.requests += 1 + static_cast<int>(localRng() % 4);
                updated.latencyMs = 8 + static_cast<int>(localRng() % 45) + static_cast<int>(rows.size() / 80);
                updated.active = true;
                return updated;
            }));
        }

        std::vector<ConcurrencySnapshot::UserSession> updatedUsers;
        size_t requestDelta = 0;
        for (auto& future : futures) {
            auto user = future.get();
            requestDelta += static_cast<size_t>(std::max(0, user.requests - baseUsers[user.id - 1].requests));
            updatedUsers.push_back(user);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            concurrency_.users = updatedUsers;
            concurrency_.activeUsers = static_cast<int>(updatedUsers.size());
            concurrency_.userRequests += requestDelta;
        }
        notify();
        ++cycle;
        std::this_thread::sleep_for(std::chrono::milliseconds(260));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.userLoadActive = false;
        concurrency_.activeUsers = 0;
        for (auto& user : concurrency_.users) {
            user.active = false;
        }
    }
}

void SimulationEngine::recordOrder(const Order& order, const std::wstring& status, int filledVolume, double averageFillPrice)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const double notional = averageFillPrice * filledVolume;
    const double fee = filledVolume > 0
        ? std::max(kMinCommission, notional * kCommissionRate) + notional * kTransferFeeRate + (order.isBuy ? 0.0 : notional * kStampDutyRate)
        : 0.0;
    orders_.push_back(OrderRecord{ order, status, filledVolume, averageFillPrice, fee });
    trim(orders_, 80);
}

void SimulationEngine::applyTrade(const Trade& trade)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const double fee = transactionFee(trade);
    if (trade.isBuy) {
        account_.cash -= trade.price * trade.volume + fee;
        account_.position += trade.volume;
    } else {
        account_.cash += trade.price * trade.volume - fee;
        account_.position -= trade.volume;
    }
    totalFees_ += fee;
    account_.markToMarket(lastPrice_);
    equities_.push_back(account_.equity);
    trim(equities_, 360);
    trades_.push_back(trade);
    trim(trades_, 80);

    std::wstringstream ss;
    ss << L"Trade #" << trade.id << L" order #" << trade.orderId << L" "
       << (trade.isBuy ? L"BUY" : L"SELL") << L" " << trade.volume
       << L" @ " << money(trade.price) << L" fee " << money(fee);
    logs_.push_back(ss.str());
    trim(logs_, 120);
}

double SimulationEngine::transactionFee(const Trade& trade) const
{
    const double notional = trade.price * trade.volume;
    const double commission = std::max(kMinCommission, notional * kCommissionRate);
    const double transfer = notional * kTransferFeeRate;
    const double stampDuty = trade.isBuy ? 0.0 : notional * kStampDutyRate;
    return commission + transfer + stampDuty;
}

void SimulationEngine::addLog(const std::wstring& text)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        logs_.push_back(text);
        trim(logs_, 120);
    }
    notify();
}

void SimulationEngine::resetLocked()
{
    account_.reset(initialCash_);
    totalFees_ = 0.0;
    bars_.clear();
    prices_.clear();
    equities_.clear();
    orders_.clear();
    trades_.clear();
    logs_.clear();
    concurrency_ = ConcurrencySnapshot{};
    lastPrice_ = 100.0;
}

void SimulationEngine::notify() const
{
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hwnd = notifyWindow_;
        ++concurrency_.uiPostMessages;
    }
    if (hwnd) {
        PostMessage(hwnd, WM_APP_ENGINE_UPDATE, 0, 0);
    }
}
