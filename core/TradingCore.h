#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>
#include <cstddef>

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

struct MarketBar {
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

enum class StrategyKind {
    MovingAverage = 0,
    Breakout = 1,
    MeanReversion = 2,
    Momentum = 3,
    RsiReversal = 4,
    BollingerBand = 5,
};

struct Account {
    double cash = 100000.0;
    int position = 0;
    double equity = 100000.0;
    double peakEquity = 100000.0;
    double maxDrawdown = 0.0;

    void reset(double initialCash);
    void markToMarket(double lastPrice);
};

class RiskManager {
public:
    bool check(const Order& order, const Account& account, double lastPrice) const;

private:
    int maxOrderVolume_ = 100000;
    double maxPriceDeviation_ = 0.08;
};

class OrderBook {
public:
    std::vector<Trade> addOrder(Order order);
    void seedLiquidity(double midPrice, long long timestamp);

private:
    void matchBuy(Order& order, std::vector<Trade>& trades);
    void matchSell(Order& order, std::vector<Trade>& trades);

    std::map<double, std::deque<Order>, std::greater<double>> bids_;
    std::map<double, std::deque<Order>> asks_;
    int nextTradeId_ = 1;
    int nextOrderId_ = -1;
};

class MovingAverageStrategy {
public:
    MovingAverageStrategy(int shortWindow, int longWindow);
    std::optional<Order> onMarketData(const MarketBar& data, const Account& account);

private:
    double average(int window) const;
    static int buyVolume(double cash, double price);

    int shortWindow_ = 5;
    int longWindow_ = 20;
    int nextOrderId_ = 1;
    bool lastBullish_ = false;
    long long lastSignalTimestamp_ = 0;
    std::deque<double> prices_;
};

class BreakoutStrategy {
public:
    explicit BreakoutStrategy(int lookback);
    std::optional<Order> onMarketData(const MarketBar& data, const Account& account);

private:
    static int buyVolume(double cash, double price);

    int lookback_ = 20;
    int nextOrderId_ = 1;
    long long lastSignalTimestamp_ = 0;
    std::deque<MarketBar> bars_;
};

class MeanReversionStrategy {
public:
    explicit MeanReversionStrategy(int window);
    std::optional<Order> onMarketData(const MarketBar& data, const Account& account);

private:
    double average() const;
    static int buyVolume(double cash, double price);

    int window_ = 20;
    int nextOrderId_ = 1;
    long long lastSignalTimestamp_ = 0;
    std::deque<double> prices_;
};

class MomentumStrategy {
public:
    explicit MomentumStrategy(int lookback);
    std::optional<Order> onMarketData(const MarketBar& data, const Account& account);

private:
    static int buyVolume(double cash, double price);

    int lookback_ = 20;
    int nextOrderId_ = 1;
    long long lastSignalTimestamp_ = 0;
    std::deque<double> prices_;
};

class RsiReversalStrategy {
public:
    explicit RsiReversalStrategy(int window);
    std::optional<Order> onMarketData(const MarketBar& data, const Account& account);

private:
    double rsi() const;
    static int buyVolume(double cash, double price);

    int window_ = 14;
    int nextOrderId_ = 1;
    long long lastSignalTimestamp_ = 0;
    std::deque<double> prices_;
};

class BollingerBandStrategy {
public:
    explicit BollingerBandStrategy(int window);
    std::optional<Order> onMarketData(const MarketBar& data, const Account& account);

private:
    double average() const;
    double stddev(double mean) const;
    static int buyVolume(double cash, double price);

    int window_ = 20;
    int nextOrderId_ = 1;
    long long lastSignalTimestamp_ = 0;
    std::deque<double> prices_;
};
