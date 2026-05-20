#pragma once

#include <cstddef>
#include <vector>

struct MacdValues {
    std::vector<double> dif;
    std::vector<double> dea;
    std::vector<double> hist;
};

struct KdjValues {
    std::vector<double> k;
    std::vector<double> d;
    std::vector<double> j;
};

std::vector<double> movingAverageParallel(const std::vector<double>& values, size_t window);
std::vector<double> rsi(const std::vector<double>& closes, size_t window);
KdjValues kdj(const std::vector<double>& highs, const std::vector<double>& lows, const std::vector<double>& closes, size_t window);
MacdValues macd(const std::vector<double>& closes);
