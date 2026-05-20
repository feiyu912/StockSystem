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
