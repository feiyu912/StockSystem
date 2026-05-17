#pragma once

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

struct UiSnapshot {
    std::vector<MarketBar> bars;
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

class SimulationEngine {
public:
    ~SimulationEngine();

    void configure(double initialCash, int shortWindow, int longWindow);
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
    std::atomic<bool> running_ = false;
    std::atomic<bool> paused_ = false;
    HWND notifyWindow_ = nullptr;
    double initialCash_ = 100000.0;
    int shortWindow_ = 5;
    int longWindow_ = 30;
    double lastPrice_ = 100.0;
    Account account_;
    std::vector<MarketBar> bars_;
    std::vector<double> prices_;
    std::vector<double> equities_;
    std::vector<Order> orders_;
    std::vector<Trade> trades_;
    std::vector<std::wstring> logs_;
};
