#pragma once

#include "TradingCore.h"

#include <mutex>
#include <string>
#include <vector>

struct DataStoreSnapshot {
    std::wstring path = L"data/akshare_export_TEST_SH.csv";
    bool cacheReady = false;
    size_t rows = 0;
    size_t diskReads = 0;
    size_t cacheHits = 0;
    size_t rangeQueries = 0;
    double lastQueryMs = 0.0;
};

class MarketDataStore {
public:
    bool loadOrCreate(const std::wstring& path, const std::string& symbol);
    std::vector<MarketBar> allBars() const;
    std::vector<MarketBar> queryRange(size_t endIndex, size_t count) const;
    DataStoreSnapshot snapshot() const;

private:
    static std::vector<MarketBar> generateSampleBars(size_t count, const std::string& symbol);
    static bool writeCsv(const std::wstring& path, const std::vector<MarketBar>& bars);
    static std::vector<MarketBar> readCsv(const std::wstring& path, const std::string& fallbackSymbol);

    mutable std::mutex mutex_;
    mutable DataStoreSnapshot stats_;
    std::vector<MarketBar> cache_;
};
