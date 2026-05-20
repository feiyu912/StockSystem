#pragma once

#include "MarketDataStore.h"
#include "TradingCore.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

struct OrderRecord {
    Order order;
    std::wstring status;
    int filledVolume = 0;
    double averageFillPrice = 0.0;
};

struct ConcurrencySnapshot {
    struct UserSession {
        int id = 0;
        double equity = 100000.0;
        int requests = 0;
        int latencyMs = 0;
        bool active = false;
    };

    std::wstring marketThreadId = L"-";
    std::wstring strategyThreadId = L"-";
    std::wstring matchingThreadId = L"-";
    std::wstring optimizationThreadId = L"-";
    std::wstring userLoadThreadId = L"-";
    bool marketActive = false;
    bool strategyActive = false;
    bool matchingActive = false;
    bool optimizationActive = false;
    bool userLoadActive = false;
    int optimizationTasks = 0;
    int activeUsers = 0;
    size_t marketEvents = 0;
    size_t strategySignals = 0;
    size_t matchedOrders = 0;
    size_t uiPostMessages = 0;
    size_t userRequests = 0;
    std::vector<UserSession> users;
};

struct UiSnapshot {
    std::vector<MarketBar> bars;
    std::vector<double> prices;
    std::vector<double> equities;
    std::vector<OrderRecord> orders;
    std::vector<Trade> trades;
    std::vector<std::wstring> logs;
    Account account;
    ConcurrencySnapshot concurrency;
    DataStoreSnapshot dataStore;
    double lastPrice = 100.0;
    bool running = false;
    bool paused = false;
};

class SimulationEngine {
public:
    ~SimulationEngine();

    void configure(double initialCash, int shortWindow, int longWindow);
    void setReplayDelay(int milliseconds);
    void start(HWND notifyWindow);
    void pauseOrResume();
    void stop();
    void reset();
    void optimize(HWND notifyWindow);
    UiSnapshot snapshot() const;

private:
    struct FastBacktestResult {
        int shortW = 0;
        int longW = 0;
        double totalReturn = 0.0;
        double drawdown = 0.0;
    };

    static FastBacktestResult runFastBacktest(int shortW, int longW);

    void marketLoop();
    void strategyLoop();
    void matchingLoop();
    void userLoadLoop();
    void recordOrder(const Order& order, const std::wstring& status, int filledVolume, double averageFillPrice);
    void applyTrade(const Trade& trade);
    void addLog(const std::wstring& text);
    void resetLocked();
    void notify() const;

    template <typename T>
    static void trim(std::vector<T>& values, size_t maxSize)
    {
        if (values.size() > maxSize) {
            values.erase(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() - maxSize));
        }
    }

    mutable std::mutex mutex_;
    BlockingQueue<MarketBar> marketQueue_;
    BlockingQueue<Order> orderQueue_;
    std::thread marketThread_;
    std::thread strategyThread_;
    std::thread matchingThread_;
    std::thread optimizationThread_;
    std::thread userLoadThread_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> paused_ = false;
    HWND notifyWindow_ = nullptr;
    double initialCash_ = 100000.0;
    int shortWindow_ = 5;
    int longWindow_ = 30;
    std::atomic<int> replayDelayMs_ = 90;
    double lastPrice_ = 100.0;
    mutable ConcurrencySnapshot concurrency_;
    mutable MarketDataStore dataStore_;
    Account account_;
    std::vector<MarketBar> bars_;
    std::vector<double> prices_;
    std::vector<double> equities_;
    std::vector<OrderRecord> orders_;
    std::vector<Trade> trades_;
    std::vector<std::wstring> logs_;
};
