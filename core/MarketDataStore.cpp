#include "MarketDataStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

bool MarketDataStore::loadOrCreate(const std::wstring& path, const std::string& symbol)
{
    namespace fs = std::filesystem;
    std::vector<MarketBar> loaded;
    if (!fs::exists(path)) {
        loaded = generateSampleBars(1200, symbol);
        writeCsv(path, loaded);
    }

    loaded = readCsv(path, symbol);
    if (loaded.empty()) {
        loaded = generateSampleBars(1200, symbol);
        writeCsv(path, loaded);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    cache_ = std::move(loaded);
    stats_.path = path;
    stats_.cacheReady = true;
    stats_.rows = cache_.size();
    ++stats_.diskReads;
    return !cache_.empty();
}

std::vector<MarketBar> MarketDataStore::allBars() const
{
    const auto begin = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.cacheHits;
    ++stats_.rangeQueries;
    const auto end = std::chrono::steady_clock::now();
    stats_.lastQueryMs = std::chrono::duration<double, std::milli>(end - begin).count();
    return cache_;
}

std::vector<MarketBar> MarketDataStore::queryRange(size_t endIndex, size_t count) const
{
    const auto begin = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.cacheHits;
    ++stats_.rangeQueries;
    if (cache_.empty()) {
        return {};
    }
    endIndex = std::min(endIndex, cache_.size() - 1);
    const size_t first = endIndex > count ? endIndex - count : 0;
    std::vector<MarketBar> result(cache_.begin() + static_cast<std::ptrdiff_t>(first),
        cache_.begin() + static_cast<std::ptrdiff_t>(endIndex + 1));
    const auto end = std::chrono::steady_clock::now();
    stats_.lastQueryMs = std::chrono::duration<double, std::milli>(end - begin).count();
    return result;
}

DataStoreSnapshot MarketDataStore::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::vector<MarketBar> MarketDataStore::generateSampleBars(size_t count, const std::string& symbol)
{
    std::vector<MarketBar> bars;
    bars.reserve(count);
    std::mt19937 rng(20260520);
    std::normal_distribution<double> noise(0.00015, 0.007);
    double price = 100.0;
    for (size_t i = 0; i < count; ++i) {
        const double open = price;
        const double close = std::max(5.0, open * (1.0 + noise(rng)));
        const double shadow = std::abs(close - open) + 0.12 + static_cast<double>(rng() % 35) / 100.0;
        MarketBar bar;
        bar.symbol = symbol;
        bar.open = open;
        bar.close = close;
        bar.high = std::max(open, close) + shadow * 0.55;
        bar.low = std::max(1.0, std::min(open, close) - shadow * 0.45);
        bar.price = close;
        bar.volume = 300 + static_cast<int>(rng() % 3000);
        bar.timestamp = static_cast<long long>(i + 1);
        bars.push_back(bar);
        price = close;
    }
    return bars;
}

bool MarketDataStore::writeCsv(const std::wstring& path, const std::vector<MarketBar>& bars)
{
    namespace fs = std::filesystem;
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out{ fs::path(path) };
    if (!out) {
        return false;
    }
    out << "symbol,timestamp,open,high,low,close,volume\n";
    for (const auto& bar : bars) {
        out << bar.symbol << ','
            << bar.timestamp << ','
            << bar.open << ','
            << bar.high << ','
            << bar.low << ','
            << bar.close << ','
            << bar.volume << '\n';
    }
    return true;
}

std::vector<MarketBar> MarketDataStore::readCsv(const std::wstring& path, const std::string& fallbackSymbol)
{
    namespace fs = std::filesystem;
    std::ifstream in{ fs::path(path) };
    std::vector<MarketBar> bars;
    if (!in) {
        return bars;
    }

    std::string line;
    std::getline(in, line);
    const bool hasSymbol = line.find("symbol") != std::string::npos;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::stringstream ss(line);
        std::string cell;
        MarketBar bar;
        bar.symbol = fallbackSymbol;
        if (hasSymbol) {
            std::getline(ss, cell, ',');
            if (!cell.empty()) {
                bar.symbol = cell;
            }
        }
        std::getline(ss, cell, ',');
        bar.timestamp = std::stoll(cell);
        std::getline(ss, cell, ',');
        bar.open = std::stod(cell);
        std::getline(ss, cell, ',');
        bar.high = std::stod(cell);
        std::getline(ss, cell, ',');
        bar.low = std::stod(cell);
        std::getline(ss, cell, ',');
        bar.close = std::stod(cell);
        std::getline(ss, cell, ',');
        bar.volume = std::stoi(cell);
        bar.price = bar.close;
        bars.push_back(bar);
    }
    return bars;
}
