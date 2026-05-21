#include "BacktestEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

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

struct UserStock {
    std::string symbol;
    std::wstring path;
};

std::wstring widenAscii(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

std::string narrowAscii(const std::wstring& text)
{
    std::string result;
    result.reserve(text.size());
    for (wchar_t ch : text) {
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::wstring strategyName(StrategyKind kind)
{
    switch (kind) {
    case StrategyKind::Breakout:
        return L"Breakout";
    case StrategyKind::MeanReversion:
        return L"MeanRev";
    case StrategyKind::Momentum:
        return L"Momentum";
    case StrategyKind::RsiReversal:
        return L"RSI";
    case StrategyKind::BollingerBand:
        return L"Bollinger";
    case StrategyKind::MovingAverage:
    default:
        return L"MA Cross";
    }
}

StrategyKind userStrategy(int id)
{
    switch ((id - 1) % 6) {
    case 1:
        return StrategyKind::Breakout;
    case 2:
        return StrategyKind::MeanReversion;
    case 3:
        return StrategyKind::Momentum;
    case 4:
        return StrategyKind::RsiReversal;
    case 5:
        return StrategyKind::BollingerBand;
    case 0:
    default:
        return StrategyKind::MovingAverage;
    }
}

double strategyExposure(StrategyKind kind, const std::vector<MarketBar>& rows)
{
    if (rows.size() < 2) {
        return 0.0;
    }

    const double open = std::max(0.0001, rows.front().open);
    const double last = rows.back().close;
    const double first = rows.front().close;
    const double pct = last / open - 1.0;

    double high = rows.front().high;
    double low = rows.front().low;
    double sum = 0.0;
    for (const auto& row : rows) {
        high = std::max(high, row.high);
        low = std::min(low, row.low);
        sum += row.close;
    }
    const double mean = sum / static_cast<double>(rows.size());

    switch (kind) {
    case StrategyKind::Breakout:
        return last >= high * 0.985 ? pct * 1.25 : pct * 0.35;
    case StrategyKind::MeanReversion:
        return last < mean ? -pct * 0.7 : pct * 0.25;
    case StrategyKind::Momentum:
        return (last / std::max(0.0001, first) - 1.0) * 1.15;
    case StrategyKind::RsiReversal:
        return last < mean * 0.96 ? std::abs(pct) * 0.75 : pct * 0.2;
    case StrategyKind::BollingerBand:
        return (last < mean ? -pct : pct) * 0.55;
    case StrategyKind::MovingAverage:
    default:
        return pct * 0.8;
    }
}

std::vector<UserStock> loadUserStocks()
{
    std::vector<UserStock> stocks;
    std::ifstream file("data\\stocks.csv");
    std::string line;
    if (file) {
        std::getline(file, line);
        while (std::getline(file, line)) {
            if (line.empty()) {
                continue;
            }
            std::stringstream ss(line);
            std::string symbol;
            std::string name;
            std::string path;
            std::getline(ss, symbol, ',');
            std::getline(ss, name, ',');
            std::getline(ss, path, ',');
            if (!symbol.empty() && !path.empty()) {
                stocks.push_back(UserStock{ symbol, widenAscii(path) });
            }
        }
    }
    if (stocks.empty()) {
        stocks.push_back(UserStock{ "TEST.SH", L"data\\akshare_export_TEST_SH.csv" });
    }
    return stocks;
}

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
    if (!universe_.empty()) {
        rebuildSelectedChartLocked(replayIndex_);
    }
}

void SimulationEngine::setReplayDelay(int milliseconds)
{
    replayDelayMs_ = std::clamp(milliseconds, 10, 1000);
}

void SimulationEngine::start(std::function<void()> onUpdate)
{
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        resetLocked();
        running_ = true;
        paused_ = false;
        onUpdate_ = std::move(onUpdate);
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

void SimulationEngine::selectSymbol(const std::wstring& dataPath, const std::string& symbol)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dataPath_ = dataPath.empty() ? dataPath_ : dataPath;
        symbol_ = symbol.empty() ? symbol_ : symbol;
        rebuildSelectedChartLocked(replayIndex_);
        std::wstringstream ss;
        ss << L"Viewing symbol changed to " << widenAscii(symbol_) << L" at current market time.";
        logs_.push_back(ss.str());
        trim(logs_, 80);
    }
    notify();
}

UiSnapshot SimulationEngine::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return UiSnapshot{ bars_, prices_, equities_, orders_, trades_, logs_, account_, initialCash_, totalFees_, concurrency_, dataStore_.snapshot(), lastPrice_, running_, paused_ };
}

void SimulationEngine::optimize(std::function<void()> onUpdate)
{
    if (optimizationThread_.joinable()) {
        optimizationThread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        onUpdate_ = std::move(onUpdate);
    }
    optimizationThread_ = std::thread([this] {
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
        }
        notify();
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
    addLog(L"Market thread started: advances one shared timeline for all loaded stocks.");
    std::wstring selectedPath;
    std::string selectedSymbol;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selectedPath = dataPath_;
        selectedSymbol = symbol_;
    }

    std::vector<ReplaySource> loadedSources;
    for (const auto& stock : loadUserStocks()) {
        MarketDataStore store;
        store.loadOrCreate(stock.path, stock.symbol);
        auto bars = store.allBars();
        if (!bars.empty()) {
            loadedSources.push_back(ReplaySource{ stock.path, stock.symbol, std::move(bars) });
        }
    }
    if (loadedSources.empty()) {
        MarketDataStore store;
        store.loadOrCreate(selectedPath, selectedSymbol);
        loadedSources.push_back(ReplaySource{ selectedPath, selectedSymbol, store.allBars() });
    }

    size_t replayLength = 0;
    for (const auto& source : loadedSources) {
        replayLength = std::max(replayLength, source.bars.size());
    }
    dataStore_.loadOrCreate(selectedPath, selectedSymbol);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        universe_ = std::move(loadedSources);
        replayIndex_ = 0;
    }
    {
        std::wstringstream ss;
        ss << L"Market universe loaded: " << universe_.size() << L" symbols share one replay clock.";
        addLog(ss.str());
    }

    for (size_t index = 0; running_.load() && index < replayLength; ++index) {
        while (paused_.load() && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }

        MarketBar event;
        bool hasSelectedEvent = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            replayIndex_ = index + 1;
            for (const auto& source : universe_) {
                if (source.symbol == symbol_ && index < source.bars.size()) {
                    event = source.bars[index];
                    hasSelectedEvent = true;
                    break;
                }
            }
            if (!hasSelectedEvent) {
                for (const auto& source : universe_) {
                    if (index < source.bars.size()) {
                        event = source.bars[index];
                        symbol_ = source.symbol;
                        dataPath_ = source.path;
                        hasSelectedEvent = true;
                        break;
                    }
                }
            }
        }
        if (!hasSelectedEvent) {
            continue;
        }
        const double price = event.close;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastPrice_ = price;
            rebuildSelectedChartLocked(index + 1);
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
    const auto stocks = loadUserStocks();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        concurrency_.userLoadThreadId = threadIdText();
        concurrency_.userLoadActive = true;
        concurrency_.users.clear();
        for (int i = 1; i <= 8; ++i) {
            const auto& stock = stocks[static_cast<size_t>(i - 1) % stocks.size()];
            ConcurrencySnapshot::UserSession user;
            user.id = i;
            user.symbol = widenAscii(stock.symbol);
            user.strategy = strategyName(userStrategy(i));
            user.dataPath = stock.path;
            user.equity = 100000.0 + i * 800.0;
            user.active = true;
            concurrency_.users.push_back(user);
        }
        concurrency_.activeUsers = static_cast<int>(concurrency_.users.size());
    }
    addLog(L"User load thread started: 8 simulated users trade existing stocks with rotating strategies.");

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
            futures.push_back(std::async(std::launch::async, [user, cycle] {
                ConcurrencySnapshot::UserSession updated = user;
                std::mt19937 localRng(static_cast<unsigned>(user.id * 7919 + cycle * 104729));
                std::normal_distribution<double> noise(0.0, 65.0);
                MarketDataStore localStore;
                localStore.loadOrCreate(user.dataPath, narrowAscii(user.symbol));
                const auto allRows = localStore.allBars();
                const size_t endIndex = allRows.empty()
                    ? 0
                    : static_cast<size_t>((cycle * 17 + user.id * 31) % allRows.size());
                const auto rows = localStore.queryRange(endIndex, 120);
                const StrategyKind strategy = userStrategy(user.id);
                double localPnl = noise(localRng);
                if (!rows.empty()) {
                    const double exposure = strategyExposure(strategy, rows);
                    localPnl += updated.equity * exposure * 0.05;
                }
                updated.equity = std::max(1000.0, updated.equity + localPnl);
                updated.requests += 1 + static_cast<int>(localRng() % 4);
                updated.latencyMs = 10 + static_cast<int>(localRng() % 35) + static_cast<int>(rows.size() / 80);
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
    universe_.clear();
    replayIndex_ = 0;
    lastPrice_ = 100.0;
}

void SimulationEngine::rebuildSelectedChartLocked(size_t endIndex)
{
    bars_.clear();
    prices_.clear();
    const ReplaySource* selected = nullptr;
    for (const auto& source : universe_) {
        if (source.symbol == symbol_) {
            selected = &source;
            break;
        }
    }
    if (!selected && !universe_.empty()) {
        selected = &universe_.front();
        symbol_ = selected->symbol;
        dataPath_ = selected->path;
    }
    if (!selected) {
        return;
    }

    const size_t count = std::min(endIndex, selected->bars.size());
    const size_t begin = count > 360 ? count - 360 : 0;
    bars_.reserve(count - begin);
    prices_.reserve(count - begin);
    for (size_t i = begin; i < count; ++i) {
        bars_.push_back(selected->bars[i]);
        prices_.push_back(selected->bars[i].close);
    }
    if (!bars_.empty()) {
        lastPrice_ = bars_.back().close;
    }
}

void SimulationEngine::notify() const
{
    std::function<void()> onUpdate;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        onUpdate = onUpdate_;
        ++concurrency_.updateNotifications;
    }
    if (onUpdate) {
        onUpdate();
    }
}
