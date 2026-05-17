// StockSystem.cpp : Win32 quantitative backtest and matching simulator.

#include "framework.h"
#include "StockSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <execution>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <commctrl.h>
#include <windowsx.h>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "concrt.lib")

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

namespace {

constexpr UINT WM_APP_ENGINE_UPDATE = WM_APP + 1;
constexpr int IDC_BTN_START = 2001;
constexpr int IDC_BTN_PAUSE = 2002;
constexpr int IDC_BTN_RESET = 2003;
constexpr int IDC_BTN_OPTIMIZE = 2004;
constexpr int IDC_COMBO_STRATEGY = 2005;
constexpr int IDC_LIST_ORDERS = 2006;
constexpr int IDC_LIST_TRADES = 2007;
constexpr int IDC_LIST_LOGS = 2008;
constexpr int IDC_EDIT_CASH = 2009;
constexpr int IDC_EDIT_SHORT = 2010;
constexpr int IDC_EDIT_LONG = 2011;

constexpr COLORREF CLR_BG = RGB(10, 15, 25);
constexpr COLORREF CLR_PANEL = RGB(15, 23, 42);
constexpr COLORREF CLR_PANEL_2 = RGB(20, 31, 51);
constexpr COLORREF CLR_BORDER = RGB(51, 65, 85);
constexpr COLORREF CLR_TEXT = RGB(226, 232, 240);
constexpr COLORREF CLR_MUTED = RGB(148, 163, 184);
constexpr COLORREF CLR_GRID = RGB(30, 41, 59);
constexpr COLORREF CLR_RED = RGB(248, 113, 113);
constexpr COLORREF CLR_GREEN = RGB(52, 211, 153);
constexpr COLORREF CLR_BLUE = RGB(96, 165, 250);
constexpr COLORREF CLR_YELLOW = RGB(250, 204, 21);

struct Rects {
    RECT chart{};
    RECT side{};
    RECT bottom{};
};

std::wstring toWide(const std::string& value)
{
    if (value.empty()) {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring out(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), size);
    return out;
}

std::wstring money(double value)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << value;
    return ss.str();
}

std::wstring percent(double value)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << value * 100.0 << L"%";
    return ss.str();
}

template <typename T>
class BlockingQueue {
public:
    void push(T item)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    bool pop(T& item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::queue<T> empty;
        queue_.swap(empty);
        closed_ = false;
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;
};

enum class EventType {
    MarketData,
    Order,
    Trade,
    Account,
    Log
};

struct Event {
    explicit Event(EventType eventType) : type(eventType) {}
    virtual ~Event() = default;
    EventType type;
};

struct MarketDataEvent : Event {
    MarketDataEvent() : Event(EventType::MarketData) {}
    std::string symbol = "TEST";
    double price = 100.0;
    double open = 100.0;
    double high = 100.0;
    double low = 100.0;
    double close = 100.0;
    int volume = 0;
    long long timestamp = 0;
};

struct Order {
    int id = 0;
    bool isBuy = true;
    double price = 0.0;
    int volume = 0;
    long long timestamp = 0;
};

struct Trade {
    int id = 0;
    int orderId = 0;
    bool isBuy = true;
    double price = 0.0;
    int volume = 0;
    long long timestamp = 0;
};

struct Account {
    double cash = 100000.0;
    int position = 0;
    double equity = 100000.0;
    double peakEquity = 100000.0;
    double maxDrawdown = 0.0;
};

class RiskManager {
public:
    bool check(const Order& order, const Account& account, double lastPrice) const
    {
        if (order.volume <= 0 || order.volume > maxOrderVolume_) {
            return false;
        }
        if (order.isBuy && account.cash < order.price * order.volume) {
            return false;
        }
        if (!order.isBuy && account.position < order.volume) {
            return false;
        }
        const double deviation = std::abs(order.price - lastPrice) / std::max(1.0, lastPrice);
        return deviation <= maxPriceDeviation_;
    }

private:
    int maxOrderVolume_ = 1000;
    double maxPriceDeviation_ = 0.08;
};

class OrderBook {
public:
    std::vector<Trade> addOrder(Order order)
    {
        std::vector<Trade> trades;
        if (order.isBuy) {
            matchBuy(order, trades);
            if (order.volume > 0) {
                bids_[order.price].push_back(order);
            }
        } else {
            matchSell(order, trades);
            if (order.volume > 0) {
                asks_[order.price].push_back(order);
            }
        }
        return trades;
    }

    void seedLiquidity(double midPrice, long long timestamp)
    {
        if (!bids_.empty() || !asks_.empty()) {
            return;
        }
        for (int i = 1; i <= 3; ++i) {
            bids_[midPrice - i * 0.05].push_back(Order{ nextOrderId_++, true, midPrice - i * 0.05, 500, timestamp });
            asks_[midPrice + i * 0.05].push_back(Order{ nextOrderId_++, false, midPrice + i * 0.05, 500, timestamp });
        }
    }

private:
    void matchBuy(Order& order, std::vector<Trade>& trades)
    {
        while (order.volume > 0 && !asks_.empty()) {
            auto bestAsk = asks_.begin();
            if (order.price < bestAsk->first) {
                break;
            }
            auto& restingQueue = bestAsk->second;
            Order& resting = restingQueue.front();
            const int volume = std::min(order.volume, resting.volume);
            trades.push_back(Trade{ nextTradeId_++, order.id, true, bestAsk->first, volume, order.timestamp });
            order.volume -= volume;
            resting.volume -= volume;
            if (resting.volume == 0) {
                restingQueue.pop_front();
            }
            if (restingQueue.empty()) {
                asks_.erase(bestAsk);
            }
        }
    }

    void matchSell(Order& order, std::vector<Trade>& trades)
    {
        while (order.volume > 0 && !bids_.empty()) {
            auto bestBid = bids_.begin();
            if (order.price > bestBid->first) {
                break;
            }
            auto& restingQueue = bestBid->second;
            Order& resting = restingQueue.front();
            const int volume = std::min(order.volume, resting.volume);
            trades.push_back(Trade{ nextTradeId_++, order.id, false, bestBid->first, volume, order.timestamp });
            order.volume -= volume;
            resting.volume -= volume;
            if (resting.volume == 0) {
                restingQueue.pop_front();
            }
            if (restingQueue.empty()) {
                bids_.erase(bestBid);
            }
        }
    }

    std::map<double, std::deque<Order>, std::greater<double>> bids_;
    std::map<double, std::deque<Order>> asks_;
    int nextTradeId_ = 1;
    int nextOrderId_ = -1;
};

class MovingAverageStrategy {
public:
    MovingAverageStrategy(int shortWindow, int longWindow)
        : shortWindow_(shortWindow), longWindow_(std::max(shortWindow + 1, longWindow))
    {
    }

    std::optional<Order> onMarketData(const MarketDataEvent& data, int position)
    {
        prices_.push_back(data.price);
        if (static_cast<int>(prices_.size()) > longWindow_) {
            prices_.pop_front();
        }
        if (static_cast<int>(prices_.size()) < longWindow_) {
            return std::nullopt;
        }

        const double shortAvg = average(shortWindow_);
        const double longAvg = average(longWindow_);
        const bool bullish = shortAvg > longAvg;
        std::optional<Order> signal;

        if (bullish && !lastBullish_ && position <= 0) {
            signal = Order{ nextOrderId_++, true, data.price + 0.10, 100, data.timestamp };
        } else if (!bullish && lastBullish_ && position > 0) {
            signal = Order{ nextOrderId_++, false, data.price - 0.10, std::min(100, position), data.timestamp };
        }

        lastBullish_ = bullish;
        return signal;
    }

private:
    double average(int window) const
    {
        auto start = prices_.end() - window;
        return std::accumulate(start, prices_.end(), 0.0) / window;
    }

    int shortWindow_ = 5;
    int longWindow_ = 20;
    int nextOrderId_ = 1;
    bool lastBullish_ = false;
    std::deque<double> prices_;
};

struct UiSnapshot {
    std::vector<MarketDataEvent> bars;
    std::vector<double> prices;
    std::vector<double> equities;
    std::vector<Order> orders;
    std::vector<Trade> trades;
    std::vector<std::wstring> logs;
    Account account;
    double lastPrice = 100.0;
    bool running = false;
    bool paused = false;
};

std::vector<double> movingAverageParallel(const std::vector<double>& values, size_t window)
{
    std::vector<double> ma(values.size(), 0.0);
    if (values.empty() || window == 0) {
        return ma;
    }

    std::vector<size_t> indexes(values.size());
    std::iota(indexes.begin(), indexes.end(), 0);

    // Parallel-computing requirement: C++17 parallel STL computes MA values in batches.
    std::for_each(std::execution::par, indexes.begin(), indexes.end(), [&](size_t i) {
        if (i + 1 < window) {
            ma[i] = values[i];
            return;
        }
        const auto first = values.begin() + static_cast<std::ptrdiff_t>(i + 1 - window);
        const auto last = values.begin() + static_cast<std::ptrdiff_t>(i + 1);
        ma[i] = std::accumulate(first, last, 0.0) / static_cast<double>(window);
    });
    return ma;
}

class SimulationEngine {
public:
    ~SimulationEngine()
    {
        stop();
    }

    void configure(double initialCash, int shortWindow, int longWindow)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initialCash_ = initialCash;
        shortWindow_ = shortWindow;
        longWindow_ = longWindow;
    }

    void start(HWND notifyWindow)
    {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resetLocked();
            running_ = true;
            paused_ = false;
            notifyWindow_ = notifyWindow;
            logs_.push_back(L"Engine started: market replay, strategy and matching run in worker threads.");
        }

        marketThread_ = std::thread(&SimulationEngine::marketLoop, this);
        strategyThread_ = std::thread(&SimulationEngine::strategyLoop, this);
        matchingThread_ = std::thread(&SimulationEngine::matchingLoop, this);
    }

    void pauseOrResume()
    {
        const bool paused = !paused_.load();
        paused_ = paused;
        addLog(paused ? L"Replay paused." : L"Replay resumed.");
    }

    void stop()
    {
        const bool wasRunning = running_.exchange(false);
        if (!wasRunning && !marketThread_.joinable() && !strategyThread_.joinable()
            && !matchingThread_.joinable() && !optimizationThread_.joinable()) {
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
        if (optimizationThread_.joinable()) {
            optimizationThread_.join();
        }
        marketQueue_.reset();
        orderQueue_.reset();
        addLog(L"Engine stopped.");
    }

    void reset()
    {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resetLocked();
            logs_.push_back(L"Simulation reset.");
        }
        notify();
    }

    UiSnapshot snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return UiSnapshot{ bars_, prices_, equities_, orders_, trades_, logs_, account_, lastPrice_, running_, paused_ };
    }

    void optimize(HWND notifyWindow)
    {
        if (optimizationThread_.joinable()) {
            optimizationThread_.join();
        }
        optimizationThread_ = std::thread([this, notifyWindow] {
            addLog(L"Parameter optimization started.");
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
            PostMessage(notifyWindow, WM_APP_ENGINE_UPDATE, 0, 0);
        });
    }

private:
    struct FastBacktestResult {
        int shortW;
        int longW;
        double totalReturn;
        double drawdown;
    };

    static FastBacktestResult runFastBacktest(int shortW, int longW)
    {
        Account account;
        MovingAverageStrategy strategy(shortW, longW);
        RiskManager risk;
        OrderBook book;
        double price = 100.0;
        std::mt19937 rng(shortW * 1000 + longW);
        std::normal_distribution<double> noise(0.0004, 0.008);

        for (long long t = 1; t <= 500; ++t) {
            price = std::max(5.0, price * (1.0 + noise(rng)));
            book.seedLiquidity(price, t);
            MarketDataEvent data;
            data.price = price;
            data.open = price;
            data.high = price;
            data.low = price;
            data.close = price;
            data.volume = 100 + static_cast<int>(rng() % 900);
            data.timestamp = t;

            auto order = strategy.onMarketData(data, account.position);
            if (order && risk.check(*order, account, price)) {
                for (const auto& trade : book.addOrder(*order)) {
                    if (trade.isBuy) {
                        account.cash -= trade.price * trade.volume;
                        account.position += trade.volume;
                    } else {
                        account.cash += trade.price * trade.volume;
                        account.position -= trade.volume;
                    }
                }
            }
            account.equity = account.cash + account.position * price;
            account.peakEquity = std::max(account.peakEquity, account.equity);
            account.maxDrawdown = std::max(account.maxDrawdown, 1.0 - account.equity / account.peakEquity);
        }
        return FastBacktestResult{ shortW, longW, account.equity / 100000.0 - 1.0, account.maxDrawdown };
    }

    void marketLoop()
    {
        std::mt19937 rng(std::random_device{}());
        std::normal_distribution<double> noise(0.0002, 0.006);
        double price = 100.0;
        for (long long timestamp = 1; running_.load() && timestamp <= 2000; ++timestamp) {
            while (paused_.load() && running_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
            const double open = price;
            const double close = std::max(5.0, open * (1.0 + noise(rng)));
            const double shadow = std::abs(close - open) + 0.20 + static_cast<double>(rng() % 40) / 100.0;
            const double high = std::max(open, close) + shadow * 0.55;
            const double low = std::max(1.0, std::min(open, close) - shadow * 0.45);
            price = close;
            MarketDataEvent event;
            event.price = price;
            event.open = open;
            event.high = high;
            event.low = low;
            event.close = close;
            event.volume = 100 + static_cast<int>(rng() % 900);
            event.timestamp = timestamp;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                lastPrice_ = price;
                bars_.push_back(event);
                trim(bars_, 360);
                prices_.push_back(price);
                trim(prices_, 360);
                account_.equity = account_.cash + account_.position * lastPrice_;
                account_.peakEquity = std::max(account_.peakEquity, account_.equity);
                account_.maxDrawdown = std::max(account_.maxDrawdown, 1.0 - account_.equity / std::max(1.0, account_.peakEquity));
                equities_.push_back(account_.equity);
                trim(equities_, 360);
            }
            marketQueue_.push(event);
            notify();
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
        }
        running_ = false;
        marketQueue_.close();
        orderQueue_.close();
        addLog(L"Market replay finished.");
    }

    void strategyLoop()
    {
        MovingAverageStrategy strategy(shortWindow_, longWindow_);
        MarketDataEvent data;
        while (marketQueue_.pop(data)) {
            const int position = [this] {
                std::lock_guard<std::mutex> lock(mutex_);
                return account_.position;
            }();
            auto order = strategy.onMarketData(data, position);
            if (order) {
                orderQueue_.push(*order);
                std::wstringstream ss;
                ss << L"Signal generated order #" << order->id << L" "
                   << (order->isBuy ? L"BUY" : L"SELL") << L" @ " << money(order->price);
                addLog(ss.str());
            }
        }
    }

    void matchingLoop()
    {
        OrderBook book;
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
            book.seedLiquidity(lastPrice, order.timestamp);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                orders_.push_back(order);
                trim(orders_, 80);
            }
            if (!risk.check(order, account, lastPrice)) {
                addLog(L"Risk rejected an order before matching.");
                continue;
            }
            const auto trades = book.addOrder(order);
            for (const auto& trade : trades) {
                applyTrade(trade);
            }
            notify();
        }
    }

    void applyTrade(const Trade& trade)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (trade.isBuy) {
            account_.cash -= trade.price * trade.volume;
            account_.position += trade.volume;
        } else {
            account_.cash += trade.price * trade.volume;
            account_.position -= trade.volume;
        }
        account_.equity = account_.cash + account_.position * lastPrice_;
        account_.peakEquity = std::max(account_.peakEquity, account_.equity);
        account_.maxDrawdown = std::max(account_.maxDrawdown, 1.0 - account_.equity / account_.peakEquity);
        equities_.push_back(account_.equity);
        trim(equities_, 240);
        trades_.push_back(trade);
        trim(trades_, 80);
        std::wstringstream ss;
        ss << L"Trade #" << trade.id << L" order #" << trade.orderId << L" "
           << (trade.isBuy ? L"BUY" : L"SELL") << L" " << trade.volume
           << L" @ " << money(trade.price);
        logs_.push_back(ss.str());
        trim(logs_, 120);
    }

    void addLog(const std::wstring& text)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            logs_.push_back(text);
            trim(logs_, 120);
        }
        notify();
    }

    template <typename T>
    static void trim(std::vector<T>& values, size_t maxSize)
    {
        if (values.size() > maxSize) {
            values.erase(values.begin(), values.begin() + (values.size() - maxSize));
        }
    }

    void resetLocked()
    {
        account_ = Account{};
        account_.cash = initialCash_;
        account_.equity = initialCash_;
        account_.peakEquity = initialCash_;
        prices_.clear();
        bars_.clear();
        equities_.clear();
        orders_.clear();
        trades_.clear();
        logs_.clear();
        lastPrice_ = 100.0;
    }

    void notify() const
    {
        HWND hwnd = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            hwnd = notifyWindow_;
        }
        if (hwnd) {
            PostMessage(hwnd, WM_APP_ENGINE_UPDATE, 0, 0);
        }
    }

    mutable std::mutex mutex_;
    BlockingQueue<MarketDataEvent> marketQueue_;
    BlockingQueue<Order> orderQueue_;
    std::thread marketThread_;
    std::thread strategyThread_;
    std::thread matchingThread_;
    std::thread optimizationThread_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> paused_ = false;
    HWND notifyWindow_ = nullptr;
    double initialCash_ = 100000.0;
    int shortWindow_ = 5;
    int longWindow_ = 30;
    double lastPrice_ = 100.0;
    Account account_;
    std::vector<double> prices_;
    std::vector<MarketDataEvent> bars_;
    std::vector<double> equities_;
    std::vector<Order> orders_;
    std::vector<Trade> trades_;
    std::vector<std::wstring> logs_;
};

struct AppState {
    HWND hStart = nullptr;
    HWND hPause = nullptr;
    HWND hReset = nullptr;
    HWND hOptimize = nullptr;
    HWND hStrategy = nullptr;
    HWND hOrders = nullptr;
    HWND hTrades = nullptr;
    HWND hLogs = nullptr;
    HWND hCash = nullptr;
    HWND hShort = nullptr;
    HWND hLong = nullptr;
    HWND hStatus = nullptr;
    Rects rects;
    int visibleBars = 110;
    std::wstring chartHint = L"Click the chart to inspect a bar. Mouse wheel zooms the K-line view.";
    HBRUSH bgBrush = CreateSolidBrush(CLR_BG);
    HBRUSH panelBrush = CreateSolidBrush(CLR_PANEL);
    HBRUSH editBrush = CreateSolidBrush(CLR_PANEL_2);
    HFONT uiFont = nullptr;
    SimulationEngine engine;
    UiSnapshot snapshot;

    ~AppState()
    {
        if (bgBrush) {
            DeleteObject(bgBrush);
        }
        if (panelBrush) {
            DeleteObject(panelBrush);
        }
        if (editBrush) {
            DeleteObject(editBrush);
        }
        if (uiFont) {
            DeleteObject(uiFont);
        }
    }
};

std::unique_ptr<AppState> g_app;

void refreshLists(HWND hWnd);

double readDouble(HWND edit, double fallback)
{
    wchar_t buffer[64]{};
    GetWindowTextW(edit, buffer, 64);
    wchar_t* end = nullptr;
    const double value = wcstod(buffer, &end);
    return end == buffer ? fallback : value;
}

int readInt(HWND edit, int fallback)
{
    wchar_t buffer[64]{};
    GetWindowTextW(edit, buffer, 64);
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 10);
    return end == buffer ? fallback : static_cast<int>(value);
}

void setChildFont(HWND child)
{
    if (g_app && g_app->uiFont) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->uiFont), TRUE);
    }
}

void startBacktest(HWND hWnd)
{
    const double cash = readDouble(g_app->hCash, 100000.0);
    const int shortW = std::max(1, readInt(g_app->hShort, 5));
    const int longW = std::max(shortW + 1, readInt(g_app->hLong, 30));
    g_app->engine.configure(cash, shortW, longW);
    g_app->engine.start(hWnd);
    refreshLists(hWnd);
}

void addListLine(HWND listBox, const std::wstring& text)
{
    SendMessageW(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void refreshLists(HWND hWnd)
{
    if (!g_app) {
        return;
    }
    g_app->snapshot = g_app->engine.snapshot();

    SendMessageW(g_app->hOrders, LB_RESETCONTENT, 0, 0);
    for (auto it = g_app->snapshot.orders.rbegin(); it != g_app->snapshot.orders.rend(); ++it) {
        std::wstringstream ss;
        ss << L"#" << it->id << L" " << (it->isBuy ? L"BUY " : L"SELL ")
           << it->volume << L" @ " << money(it->price);
        addListLine(g_app->hOrders, ss.str());
    }

    SendMessageW(g_app->hTrades, LB_RESETCONTENT, 0, 0);
    for (auto it = g_app->snapshot.trades.rbegin(); it != g_app->snapshot.trades.rend(); ++it) {
        std::wstringstream ss;
        ss << L"#" << it->id << L" " << (it->isBuy ? L"BUY " : L"SELL ")
           << it->volume << L" @ " << money(it->price);
        addListLine(g_app->hTrades, ss.str());
    }

    SendMessageW(g_app->hLogs, LB_RESETCONTENT, 0, 0);
    for (auto it = g_app->snapshot.logs.rbegin(); it != g_app->snapshot.logs.rend(); ++it) {
        addListLine(g_app->hLogs, *it);
    }

    std::wstringstream status;
    status << L"Price " << money(g_app->snapshot.lastPrice)
           << L" | Cash " << money(g_app->snapshot.account.cash)
           << L" | Position " << g_app->snapshot.account.position
           << L" | Equity " << money(g_app->snapshot.account.equity)
           << L" | MaxDD " << percent(g_app->snapshot.account.maxDrawdown);
    SetWindowTextW(g_app->hStatus, status.str().c_str());
    InvalidateRect(hWnd, nullptr, FALSE);
}

void drawSeries(HDC hdc, const RECT& rect, const std::vector<double>& values, COLORREF color)
{
    if (values.size() < 2) {
        return;
    }
    const auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    const double minValue = *minIt;
    const double maxValue = *maxIt;
    const double span = std::max(0.0001, maxValue - minValue);
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    auto pointAt = [&](size_t i) {
        const double xRatio = static_cast<double>(i) / (values.size() - 1);
        const double yRatio = (values[i] - minValue) / span;
        POINT p{
            rect.left + static_cast<LONG>(xRatio * (rect.right - rect.left)),
            rect.bottom - static_cast<LONG>(yRatio * (rect.bottom - rect.top))
        };
        return p;
    };
    POINT first = pointAt(0);
    MoveToEx(hdc, first.x, first.y, nullptr);
    for (size_t i = 1; i < values.size(); ++i) {
        POINT p = pointAt(i);
        LineTo(hdc, p.x, p.y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void drawLineWithScale(HDC hdc, const RECT& rect, const std::vector<double>& values, double minValue, double maxValue, COLORREF color, int width = 1)
{
    if (values.size() < 2) {
        return;
    }
    const double span = std::max(0.0001, maxValue - minValue);
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    auto pointAt = [&](size_t i) {
        const double xRatio = static_cast<double>(i) / static_cast<double>(values.size() - 1);
        const double yRatio = (values[i] - minValue) / span;
        return POINT{
            rect.left + static_cast<LONG>(xRatio * (rect.right - rect.left)),
            rect.bottom - static_cast<LONG>(yRatio * (rect.bottom - rect.top))
        };
    };
    POINT first = pointAt(0);
    MoveToEx(hdc, first.x, first.y, nullptr);
    for (size_t i = 1; i < values.size(); ++i) {
        POINT p = pointAt(i);
        LineTo(hdc, p.x, p.y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void drawKLine(HDC hdc, const RECT& rect, const std::vector<MarketDataEvent>& allBars, int visibleBars)
{
    if (allBars.empty()) {
        SetTextColor(hdc, CLR_MUTED);
        DrawTextW(hdc, L"No market data yet. Press Start or Space.", -1, const_cast<RECT*>(&rect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    const size_t count = std::min(static_cast<size_t>(std::max(20, visibleBars)), allBars.size());
    const auto first = allBars.end() - static_cast<std::ptrdiff_t>(count);
    std::vector<MarketDataEvent> bars(first, allBars.end());
    std::vector<double> closes;
    closes.reserve(bars.size());

    double minPrice = bars.front().low;
    double maxPrice = bars.front().high;
    for (const auto& bar : bars) {
        minPrice = std::min(minPrice, bar.low);
        maxPrice = std::max(maxPrice, bar.high);
        closes.push_back(bar.close);
    }
    const auto ma5 = movingAverageParallel(closes, 5);
    const auto ma20 = movingAverageParallel(closes, 20);

    const double span = std::max(0.0001, maxPrice - minPrice);
    auto yOf = [&](double price) {
        const double ratio = (price - minPrice) / span;
        return rect.bottom - static_cast<int>(ratio * (rect.bottom - rect.top));
    };

    HPEN gridPen = CreatePen(PS_SOLID, 1, CLR_GRID);
    HGDIOBJ oldPen = SelectObject(hdc, gridPen);
    for (int i = 1; i < 5; ++i) {
        const int y = rect.top + (rect.bottom - rect.top) * i / 5;
        MoveToEx(hdc, rect.left, y, nullptr);
        LineTo(hdc, rect.right, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);

    const int width = std::max(3, static_cast<int>((rect.right - rect.left) / static_cast<LONG>(bars.size())));
    const int bodyWidth = std::max(3, width - 3);
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& bar = bars[i];
        const int x = rect.left + static_cast<int>((i + 0.5) * (rect.right - rect.left) / bars.size());
        const bool up = bar.close >= bar.open;
        const COLORREF color = up ? CLR_RED : CLR_GREEN;
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ prevPen = SelectObject(hdc, pen);
        HGDIOBJ prevBrush = SelectObject(hdc, brush);
        MoveToEx(hdc, x, yOf(bar.high), nullptr);
        LineTo(hdc, x, yOf(bar.low));
        RECT body{
            x - bodyWidth / 2,
            std::min(yOf(bar.open), yOf(bar.close)),
            x + bodyWidth / 2,
            std::max(yOf(bar.open), yOf(bar.close)) + 1
        };
        Rectangle(hdc, body.left, body.top, body.right, body.bottom);
        SelectObject(hdc, prevBrush);
        SelectObject(hdc, prevPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    drawLineWithScale(hdc, rect, ma5, minPrice, maxPrice, CLR_YELLOW, 2);
    drawLineWithScale(hdc, rect, ma20, minPrice, maxPrice, CLR_BLUE, 2);

    SetTextColor(hdc, CLR_MUTED);
    std::wstringstream axis;
    axis << L"High " << money(maxPrice) << L"    Low " << money(minPrice);
    RECT textRect = rect;
    textRect.left += 4;
    textRect.top += 4;
    DrawTextW(hdc, axis.str().c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

void drawDashboard(HWND hWnd, HDC hdc)
{
    if (!g_app) {
        return;
    }
    RECT client{};
    GetClientRect(hWnd, &client);
    FillRect(hdc, &client, g_app->bgBrush);

    RECT chart = g_app->rects.chart;
    FillRect(hdc, &chart, g_app->panelBrush);
    HBRUSH border = CreateSolidBrush(CLR_BORDER);
    FrameRect(hdc, &chart, border);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, CLR_TEXT);
    RECT title = chart;
    title.left += 16;
    title.top += 10;
    DrawTextW(hdc, L"TEST.SH  K-Line / MA / Equity", -1, &title, DT_TOP | DT_LEFT | DT_SINGLELINE);

    RECT inner = chart;
    inner.left += 20;
    inner.right -= 16;
    inner.top += 42;
    inner.bottom -= 88;
    drawKLine(hdc, inner, g_app->snapshot.bars, g_app->visibleBars);

    RECT equityRect = chart;
    equityRect.left += 20;
    equityRect.right -= 16;
    equityRect.top = inner.bottom + 18;
    equityRect.bottom -= 22;
    HBRUSH equityBrush = CreateSolidBrush(CLR_PANEL_2);
    FillRect(hdc, &equityRect, equityBrush);
    DeleteObject(equityBrush);
    FrameRect(hdc, &equityRect, border);
    drawSeries(hdc, equityRect, g_app->snapshot.equities, CLR_GREEN);

    RECT hint = chart;
    hint.left += 16;
    hint.top = chart.bottom - 20;
    SetTextColor(hdc, CLR_MUTED);
    DrawTextW(hdc, g_app->chartHint.c_str(), -1, &hint, DT_LEFT | DT_SINGLELINE);
    DeleteObject(border);
}

void layout(HWND hWnd)
{
    if (!g_app) {
        return;
    }
    RECT rc{};
    GetClientRect(hWnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int top = 14;
    const int sideWidth = 260;
    const int bottomHeight = 190;
    const int gap = 10;

    g_app->rects.chart = { 14, top + 42, width - sideWidth - gap * 2, height - bottomHeight - gap };
    g_app->rects.side = { width - sideWidth - gap, top, width - gap, height - bottomHeight - gap };
    g_app->rects.bottom = { 14, height - bottomHeight, width - 14, height - 32 };

    MoveWindow(g_app->hStart, 14, top, 72, 28, TRUE);
    MoveWindow(g_app->hPause, 92, top, 72, 28, TRUE);
    MoveWindow(g_app->hReset, 170, top, 72, 28, TRUE);
    MoveWindow(g_app->hOptimize, 248, top, 104, 28, TRUE);

    int sx = g_app->rects.side.left;
    int sy = g_app->rects.side.top;
    MoveWindow(g_app->hStrategy, sx, sy + 20, sideWidth - 20, 26, TRUE);
    MoveWindow(g_app->hCash, sx, sy + 78, sideWidth - 20, 24, TRUE);
    MoveWindow(g_app->hShort, sx, sy + 136, 110, 24, TRUE);
    MoveWindow(g_app->hLong, sx + 128, sy + 136, 110, 24, TRUE);

    const int third = (g_app->rects.bottom.right - g_app->rects.bottom.left - 20) / 3;
    MoveWindow(g_app->hOrders, g_app->rects.bottom.left, g_app->rects.bottom.top + 24, third, bottomHeight - 60, TRUE);
    MoveWindow(g_app->hTrades, g_app->rects.bottom.left + third + 10, g_app->rects.bottom.top + 24, third, bottomHeight - 60, TRUE);
    MoveWindow(g_app->hLogs, g_app->rects.bottom.left + (third + 10) * 2, g_app->rects.bottom.top + 24, third, bottomHeight - 60, TRUE);
    MoveWindow(g_app->hStatus, 0, height - 24, width, 24, TRUE);
}

void createControls(HWND hWnd)
{
    auto menuId = [](int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); };
    g_app->uiFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    g_app->hStart = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, menuId(IDC_BTN_START), hInst, nullptr);
    g_app->hPause = CreateWindowW(L"BUTTON", L"Pause", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, menuId(IDC_BTN_PAUSE), hInst, nullptr);
    g_app->hReset = CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, menuId(IDC_BTN_RESET), hInst, nullptr);
    g_app->hOptimize = CreateWindowW(L"BUTTON", L"Optimize", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, menuId(IDC_BTN_OPTIMIZE), hInst, nullptr);
    g_app->hStrategy = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0, 0, hWnd, menuId(IDC_COMBO_STRATEGY), hInst, nullptr);
    SendMessageW(g_app->hStrategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Moving Average"));
    SendMessageW(g_app->hStrategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Momentum Breakout"));
    SendMessageW(g_app->hStrategy, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Grid Trading"));
    SendMessageW(g_app->hStrategy, CB_SETCURSEL, 0, 0);

    g_app->hCash = CreateWindowW(L"EDIT", L"100000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, menuId(IDC_EDIT_CASH), hInst, nullptr);
    g_app->hShort = CreateWindowW(L"EDIT", L"5", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, menuId(IDC_EDIT_SHORT), hInst, nullptr);
    g_app->hLong = CreateWindowW(L"EDIT", L"30", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, hWnd, menuId(IDC_EDIT_LONG), hInst, nullptr);
    g_app->hOrders = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 0, 0, 0, 0, hWnd, menuId(IDC_LIST_ORDERS), hInst, nullptr);
    g_app->hTrades = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 0, 0, 0, 0, hWnd, menuId(IDC_LIST_TRADES), hInst, nullptr);
    g_app->hLogs = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 0, 0, 0, 0, hWnd, menuId(IDC_LIST_LOGS), hInst, nullptr);
    g_app->hStatus = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_SUNKEN, 0, 0, 0, 0, hWnd, nullptr, hInst, nullptr);
    for (HWND child : { g_app->hStart, g_app->hPause, g_app->hReset, g_app->hOptimize, g_app->hStrategy,
        g_app->hCash, g_app->hShort, g_app->hLong, g_app->hOrders, g_app->hTrades, g_app->hLogs, g_app->hStatus }) {
        setChildFont(child);
    }
    layout(hWnd);
}

void drawLabels(HWND hWnd, HDC hdc)
{
    if (!g_app) {
        return;
    }
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, CLR_MUTED);
    RECT r = g_app->rects.side;
    TextOutW(hdc, r.left, r.top, L"Strategy", 8);
    TextOutW(hdc, r.left, r.top + 58, L"Initial Cash", 12);
    TextOutW(hdc, r.left, r.top + 116, L"Short MA", 8);
    TextOutW(hdc, r.left + 128, r.top + 116, L"Long MA", 7);
    TextOutW(hdc, g_app->rects.bottom.left, g_app->rects.bottom.top, L"Orders", 6);
    TextOutW(hdc, g_app->rects.bottom.left + ((g_app->rects.bottom.right - g_app->rects.bottom.left - 20) / 3) + 10, g_app->rects.bottom.top, L"Trades", 6);
    TextOutW(hdc, g_app->rects.bottom.left + (((g_app->rects.bottom.right - g_app->rects.bottom.left - 20) / 3) + 10) * 2, g_app->rects.bottom.top, L"Logs", 4);
}

} // namespace

ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_STOCKSYSTEM, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow)) {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_STOCKSYSTEM));
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_STOCKSYSTEM));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_STOCKSYSTEM);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    HWND hWnd = CreateWindowW(
        szWindowClass,
        L"Low-Latency Quant Backtest and Matching Simulator",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        0,
        1120,
        760,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hWnd) {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE:
        g_app = std::make_unique<AppState>();
        createControls(hWnd);
        g_app->engine.reset();
        refreshLists(hWnd);
        break;
    case WM_SIZE:
        layout(hWnd);
        InvalidateRect(hWnd, nullptr, TRUE);
        break;
    case WM_COMMAND:
    {
        const int wmId = LOWORD(wParam);
        switch (wmId) {
        case IDC_BTN_START:
            startBacktest(hWnd);
            break;
        case IDC_BTN_PAUSE:
            g_app->engine.pauseOrResume();
            refreshLists(hWnd);
            break;
        case IDC_BTN_RESET:
            g_app->engine.reset();
            refreshLists(hWnd);
            break;
        case IDC_BTN_OPTIMIZE:
            g_app->engine.optimize(hWnd);
            break;
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, CLR_PANEL_2);
        SetTextColor(dc, CLR_TEXT);
        return reinterpret_cast<LRESULT>(g_app ? g_app->editBrush : GetStockObject(BLACK_BRUSH));
    }
    case WM_KEYDOWN:
        if (wParam == VK_SPACE) {
            if (!g_app->snapshot.running) {
                startBacktest(hWnd);
                break;
            }
            g_app->engine.pauseOrResume();
        } else if (wParam == 'R') {
            g_app->engine.reset();
        } else if (wParam == VK_ESCAPE) {
            g_app->engine.stop();
        } else if (wParam == VK_F1) {
            MessageBoxW(hWnd,
                L"Win32 homework hooks:\n\n"
                L"Space: start or pause backtest\n"
                L"R: reset\n"
                L"Esc: stop\n"
                L"Mouse wheel: zoom K-line bars\n"
                L"Left click chart: inspect OHLC price\n\n"
                L"Backtest runs on std::thread workers and posts WM_APP messages to the UI thread.",
                L"Help", MB_OK | MB_ICONINFORMATION);
        }
        refreshLists(hWnd);
        break;
    case WM_LBUTTONDOWN:
        if (g_app) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            RECT chartInner = g_app->rects.chart;
            chartInner.left += 20;
            chartInner.right -= 16;
            chartInner.top += 42;
            chartInner.bottom -= 88;
            if (PtInRect(&chartInner, POINT{ x, y }) && !g_app->snapshot.bars.empty()) {
                const size_t count = std::min(static_cast<size_t>(std::max(20, g_app->visibleBars)), g_app->snapshot.bars.size());
                const int chartWidth = std::max(1, static_cast<int>(chartInner.right - chartInner.left));
                const int relativeX = std::clamp(static_cast<int>(x - chartInner.left), 0, chartWidth);
                const size_t index = std::min(count - 1,
                    static_cast<size_t>(static_cast<double>(relativeX) / chartWidth * count));
                const auto& bar = *(g_app->snapshot.bars.end() - static_cast<std::ptrdiff_t>(count) + static_cast<std::ptrdiff_t>(index));
                std::wstringstream ss;
                ss << L"Bar " << bar.timestamp
                   << L"  O " << money(bar.open)
                   << L" H " << money(bar.high)
                   << L" L " << money(bar.low)
                   << L" C " << money(bar.close)
                   << L" Vol " << bar.volume;
                g_app->chartHint = ss.str();
                SetWindowTextW(g_app->hStatus, ss.str().c_str());
                InvalidateRect(hWnd, &g_app->rects.chart, FALSE);
            }
        }
        break;
    case WM_MOUSEWHEEL:
        if (g_app) {
            const bool zoomIn = GET_WHEEL_DELTA_WPARAM(wParam) > 0;
            g_app->visibleBars = std::clamp(g_app->visibleBars + (zoomIn ? -15 : 15), 30, 240);
            std::wstringstream ss;
            ss << L"K-line zoom: showing last " << g_app->visibleBars << L" bars";
            g_app->chartHint = ss.str();
            SetWindowTextW(g_app->hStatus, ss.str().c_str());
            InvalidateRect(hWnd, &g_app->rects.chart, FALSE);
        }
        break;
    case WM_MOUSEMOVE:
        if (g_app) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (PtInRect(&g_app->rects.chart, POINT{ x, y })) {
                std::wstringstream ss;
                ss << L"Mouse chart position x=" << x << L", y=" << y
                   << L" | Last price " << money(g_app->snapshot.lastPrice);
                SetWindowTextW(g_app->hStatus, ss.str().c_str());
            }
        }
        break;
    case WM_APP_ENGINE_UPDATE:
        refreshLists(hWnd);
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        drawDashboard(hWnd, hdc);
        drawLabels(hWnd, hdc);
        EndPaint(hWnd, &ps);
    }
    break;
    case WM_DESTROY:
        if (g_app) {
            g_app->engine.stop();
            g_app.reset();
        }
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        return static_cast<INT_PTR>(TRUE);
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return static_cast<INT_PTR>(TRUE);
        }
        break;
    }
    return static_cast<INT_PTR>(FALSE);
}
