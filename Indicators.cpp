#include "Indicators.h"

#include <algorithm>
#include <cstddef>
#include <execution>
#include <limits>
#include <numeric>

std::vector<double> movingAverageParallel(const std::vector<double>& values, size_t window)
{
    std::vector<double> ma(values.size(), std::numeric_limits<double>::quiet_NaN());
    if (values.empty() || window == 0) {
        return ma;
    }

    std::vector<size_t> indexes(values.size());
    std::iota(indexes.begin(), indexes.end(), 0);

    // Parallel-computing requirement: C++17 parallel STL computes MA values in batches.
    std::for_each(std::execution::par, indexes.begin(), indexes.end(), [&](size_t i) {
        if (i + 1 < window) {
            return;
        }
        const auto first = values.begin() + static_cast<std::ptrdiff_t>(i + 1 - window);
        const auto last = values.begin() + static_cast<std::ptrdiff_t>(i + 1);
        ma[i] = std::accumulate(first, last, 0.0) / static_cast<double>(window);
    });
    return ma;
}

std::vector<double> rsi(const std::vector<double>& closes, size_t window)
{
    std::vector<double> values(closes.size(), std::numeric_limits<double>::quiet_NaN());
    if (closes.size() <= window || window == 0) {
        return values;
    }

    for (size_t i = window; i < closes.size(); ++i) {
        double gain = 0.0;
        double loss = 0.0;
        for (size_t j = i + 1 - window; j <= i; ++j) {
            const double change = closes[j] - closes[j - 1];
            if (change >= 0.0) {
                gain += change;
            } else {
                loss -= change;
            }
        }
        const double total = gain + loss;
        values[i] = total <= 0.000001 ? 50.0 : 100.0 * gain / total;
    }
    return values;
}

KdjValues kdj(const std::vector<double>& highs, const std::vector<double>& lows, const std::vector<double>& closes, size_t window)
{
    KdjValues out;
    out.k.assign(closes.size(), std::numeric_limits<double>::quiet_NaN());
    out.d.assign(closes.size(), std::numeric_limits<double>::quiet_NaN());
    out.j.assign(closes.size(), std::numeric_limits<double>::quiet_NaN());
    if (closes.size() < window || window == 0) {
        return out;
    }

    double prevK = 50.0;
    double prevD = 50.0;
    for (size_t i = window - 1; i < closes.size(); ++i) {
        const auto highFirst = highs.begin() + static_cast<std::ptrdiff_t>(i + 1 - window);
        const auto highLast = highs.begin() + static_cast<std::ptrdiff_t>(i + 1);
        const auto lowFirst = lows.begin() + static_cast<std::ptrdiff_t>(i + 1 - window);
        const auto lowLast = lows.begin() + static_cast<std::ptrdiff_t>(i + 1);
        const double highest = *std::max_element(highFirst, highLast);
        const double lowest = *std::min_element(lowFirst, lowLast);
        const double rsv = highest <= lowest ? 50.0 : (closes[i] - lowest) * 100.0 / (highest - lowest);
        prevK = prevK * 2.0 / 3.0 + rsv / 3.0;
        prevD = prevD * 2.0 / 3.0 + prevK / 3.0;
        out.k[i] = prevK;
        out.d[i] = prevD;
        out.j[i] = 3.0 * prevK - 2.0 * prevD;
    }
    return out;
}

namespace {

std::vector<double> ema(const std::vector<double>& values, size_t window)
{
    std::vector<double> out(values.size(), std::numeric_limits<double>::quiet_NaN());
    if (values.empty() || window == 0) {
        return out;
    }
    const double alpha = 2.0 / (static_cast<double>(window) + 1.0);
    double current = values.front();
    out[0] = current;
    for (size_t i = 1; i < values.size(); ++i) {
        current = alpha * values[i] + (1.0 - alpha) * current;
        out[i] = current;
    }
    return out;
}

} // namespace

MacdValues macd(const std::vector<double>& closes)
{
    MacdValues out;
    out.dif.assign(closes.size(), std::numeric_limits<double>::quiet_NaN());
    out.dea.assign(closes.size(), std::numeric_limits<double>::quiet_NaN());
    out.hist.assign(closes.size(), std::numeric_limits<double>::quiet_NaN());
    if (closes.empty()) {
        return out;
    }

    const auto ema12 = ema(closes, 12);
    const auto ema26 = ema(closes, 26);
    std::vector<double> dif(closes.size(), 0.0);
    for (size_t i = 0; i < closes.size(); ++i) {
        dif[i] = ema12[i] - ema26[i];
        out.dif[i] = dif[i];
    }
    out.dea = ema(dif, 9);
    for (size_t i = 0; i < closes.size(); ++i) {
        out.hist[i] = (out.dif[i] - out.dea[i]) * 2.0;
    }
    return out;
}
