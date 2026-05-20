#include "TradingCore.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

constexpr double kEstimatedBuyCostRate = 0.0003;
constexpr double kEstimatedTransferFeeRate = 0.00001;
constexpr double kEstimatedMinCommission = 5.0;
constexpr int kLotSize = 100;
constexpr int kSignalCooldownBars = 6;

int affordableRoundLot(double cash, double price, double cashUsage)
{
    if (cash <= 0.0 || price <= 0.0) {
        return 0;
    }
    const double usableCash = cash * cashUsage;
    int volume = static_cast<int>(usableCash / (price * (1.0 + kEstimatedBuyCostRate + kEstimatedTransferFeeRate))) / kLotSize * kLotSize;
    while (volume > 0) {
        const double notional = price * volume;
        const double fee = std::max(kEstimatedMinCommission, notional * kEstimatedBuyCostRate) + notional * kEstimatedTransferFeeRate;
        if (notional + fee <= cash) {
            return volume;
        }
        volume -= kLotSize;
    }
    return 0;
}

bool cooldownPassed(long long now, long long lastSignal)
{
    return lastSignal == 0 || now - lastSignal >= kSignalCooldownBars;
}

} // namespace

void Account::reset(double initialCash)
{
    cash = initialCash;
    position = 0;
    equity = initialCash;
    peakEquity = initialCash;
    maxDrawdown = 0.0;
}

void Account::markToMarket(double lastPrice)
{
    equity = cash + position * lastPrice;
    peakEquity = std::max(peakEquity, equity);
    maxDrawdown = std::max(maxDrawdown, 1.0 - equity / std::max(1.0, peakEquity));
}

bool RiskManager::check(const Order& order, const Account& account, double lastPrice) const
{
    if (order.volume <= 0 || order.volume > maxOrderVolume_) {
        return false;
    }
    if (order.isBuy) {
        const double notional = order.price * order.volume;
        const double fee = std::max(kEstimatedMinCommission, notional * kEstimatedBuyCostRate) + notional * kEstimatedTransferFeeRate;
        if (account.cash < notional + fee) {
            return false;
        }
    }
    if (!order.isBuy && account.position < order.volume) {
        return false;
    }
    const double deviation = std::abs(order.price - lastPrice) / std::max(1.0, lastPrice);
    return deviation <= maxPriceDeviation_;
}

std::vector<Trade> OrderBook::addOrder(Order order)
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

void OrderBook::seedLiquidity(double midPrice, long long timestamp)
{
    if (!bids_.empty() || !asks_.empty()) {
        return;
    }
    for (int i = 1; i <= 3; ++i) {
        bids_[midPrice - i * 0.05].push_back(Order{ nextOrderId_++, true, midPrice - i * 0.05, 500, timestamp });
        asks_[midPrice + i * 0.05].push_back(Order{ nextOrderId_++, false, midPrice + i * 0.05, 500, timestamp });
    }
}

void OrderBook::matchBuy(Order& order, std::vector<Trade>& trades)
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

void OrderBook::matchSell(Order& order, std::vector<Trade>& trades)
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

MovingAverageStrategy::MovingAverageStrategy(int shortWindow, int longWindow)
    : shortWindow_(std::max(1, shortWindow)),
      longWindow_(std::max(shortWindow_ + 1, longWindow))
{
}

std::optional<Order> MovingAverageStrategy::onMarketData(const MarketBar& data, const Account& account)
{
    prices_.push_back(data.price);
    if (static_cast<int>(prices_.size()) > longWindow_) {
        prices_.pop_front();
    }
    if (static_cast<int>(prices_.size()) < longWindow_) {
        return std::nullopt;
    }

    const double shortAverage = average(shortWindow_);
    const double longAverage = average(longWindow_);
    const bool bullish = shortAverage > longAverage * 1.001;
    const bool bearish = shortAverage < longAverage * 0.999;
    std::optional<Order> signal;
    if (cooldownPassed(data.timestamp, lastSignalTimestamp_) && bullish && !lastBullish_ && account.position <= 0) {
        const int volume = buyVolume(account.cash, data.price);
        if (volume > 0) {
            signal = Order{ nextOrderId_++, true, data.price + 0.10, volume, data.timestamp };
            lastSignalTimestamp_ = data.timestamp;
        }
    } else if (cooldownPassed(data.timestamp, lastSignalTimestamp_) && bearish && lastBullish_ && account.position > 0) {
        signal = Order{ nextOrderId_++, false, data.price - 0.10, account.position, data.timestamp };
        lastSignalTimestamp_ = data.timestamp;
    }
    lastBullish_ = bullish;
    return signal;
}

double MovingAverageStrategy::average(int window) const
{
    const auto first = prices_.end() - window;
    return std::accumulate(first, prices_.end(), 0.0) / window;
}

int MovingAverageStrategy::buyVolume(double cash, double price)
{
    return affordableRoundLot(cash, price, 0.80);
}

BreakoutStrategy::BreakoutStrategy(int lookback)
    : lookback_(std::max(5, lookback))
{
}

std::optional<Order> BreakoutStrategy::onMarketData(const MarketBar& data, const Account& account)
{
    if (static_cast<int>(bars_.size()) < lookback_) {
        bars_.push_back(data);
        return std::nullopt;
    }

    const auto highIt = std::max_element(bars_.begin(), bars_.end(),
        [](const MarketBar& a, const MarketBar& b) { return a.high < b.high; });
    const auto lowIt = std::min_element(bars_.begin(), bars_.end(),
        [](const MarketBar& a, const MarketBar& b) { return a.low < b.low; });

    std::optional<Order> signal;
    if (cooldownPassed(data.timestamp, lastSignalTimestamp_) && data.close > highIt->high && account.position <= 0) {
        const int volume = buyVolume(account.cash, data.price);
        if (volume > 0) {
            signal = Order{ nextOrderId_++, true, data.price + 0.10, volume, data.timestamp };
            lastSignalTimestamp_ = data.timestamp;
        }
    } else if (cooldownPassed(data.timestamp, lastSignalTimestamp_) && data.close < lowIt->low && account.position > 0) {
        signal = Order{ nextOrderId_++, false, data.price - 0.10, account.position, data.timestamp };
        lastSignalTimestamp_ = data.timestamp;
    }

    bars_.push_back(data);
    if (static_cast<int>(bars_.size()) > lookback_) {
        bars_.pop_front();
    }
    return signal;
}

int BreakoutStrategy::buyVolume(double cash, double price)
{
    return affordableRoundLot(cash, price, 0.70);
}

MeanReversionStrategy::MeanReversionStrategy(int window)
    : window_(std::max(5, window))
{
}

std::optional<Order> MeanReversionStrategy::onMarketData(const MarketBar& data, const Account& account)
{
    prices_.push_back(data.price);
    if (static_cast<int>(prices_.size()) > window_) {
        prices_.pop_front();
    }
    if (static_cast<int>(prices_.size()) < window_) {
        return std::nullopt;
    }

    const double mean = average();
    if (!cooldownPassed(data.timestamp, lastSignalTimestamp_)) {
        return std::nullopt;
    }
    if (data.price < mean * 0.975 && account.position <= 0) {
        const int volume = buyVolume(account.cash, data.price);
        if (volume > 0) {
            lastSignalTimestamp_ = data.timestamp;
            return Order{ nextOrderId_++, true, data.price + 0.10, volume, data.timestamp };
        }
    }
    if (data.price > mean * 1.02 && account.position > 0) {
        lastSignalTimestamp_ = data.timestamp;
        return Order{ nextOrderId_++, false, data.price - 0.10, account.position, data.timestamp };
    }
    return std::nullopt;
}

double MeanReversionStrategy::average() const
{
    return std::accumulate(prices_.begin(), prices_.end(), 0.0) / prices_.size();
}

int MeanReversionStrategy::buyVolume(double cash, double price)
{
    return affordableRoundLot(cash, price, 0.45);
}

MomentumStrategy::MomentumStrategy(int lookback)
    : lookback_(std::max(5, lookback))
{
}

std::optional<Order> MomentumStrategy::onMarketData(const MarketBar& data, const Account& account)
{
    prices_.push_back(data.price);
    if (static_cast<int>(prices_.size()) > lookback_ + 1) {
        prices_.pop_front();
    }
    if (static_cast<int>(prices_.size()) <= lookback_ || !cooldownPassed(data.timestamp, lastSignalTimestamp_)) {
        return std::nullopt;
    }

    const double reference = prices_.front();
    const double momentum = data.price / std::max(0.0001, reference) - 1.0;
    if (momentum > 0.035 && account.position <= 0) {
        const int volume = buyVolume(account.cash, data.price);
        if (volume > 0) {
            lastSignalTimestamp_ = data.timestamp;
            return Order{ nextOrderId_++, true, data.price + 0.10, volume, data.timestamp };
        }
    }
    if (momentum < -0.018 && account.position > 0) {
        lastSignalTimestamp_ = data.timestamp;
        return Order{ nextOrderId_++, false, data.price - 0.10, account.position, data.timestamp };
    }
    return std::nullopt;
}

int MomentumStrategy::buyVolume(double cash, double price)
{
    return affordableRoundLot(cash, price, 0.65);
}

RsiReversalStrategy::RsiReversalStrategy(int window)
    : window_(std::max(6, window))
{
}

std::optional<Order> RsiReversalStrategy::onMarketData(const MarketBar& data, const Account& account)
{
    prices_.push_back(data.price);
    if (static_cast<int>(prices_.size()) > window_ + 1) {
        prices_.pop_front();
    }
    if (static_cast<int>(prices_.size()) <= window_ || !cooldownPassed(data.timestamp, lastSignalTimestamp_)) {
        return std::nullopt;
    }

    const double value = rsi();
    if (value < 30.0 && account.position <= 0) {
        const int volume = buyVolume(account.cash, data.price);
        if (volume > 0) {
            lastSignalTimestamp_ = data.timestamp;
            return Order{ nextOrderId_++, true, data.price + 0.10, volume, data.timestamp };
        }
    }
    if (value > 68.0 && account.position > 0) {
        lastSignalTimestamp_ = data.timestamp;
        return Order{ nextOrderId_++, false, data.price - 0.10, account.position, data.timestamp };
    }
    return std::nullopt;
}

double RsiReversalStrategy::rsi() const
{
    double gains = 0.0;
    double losses = 0.0;
    for (size_t i = 1; i < prices_.size(); ++i) {
        const double diff = prices_[i] - prices_[i - 1];
        if (diff >= 0.0) {
            gains += diff;
        } else {
            losses -= diff;
        }
    }
    if (losses <= 0.0001) {
        return 100.0;
    }
    const double rs = gains / losses;
    return 100.0 - 100.0 / (1.0 + rs);
}

int RsiReversalStrategy::buyVolume(double cash, double price)
{
    return affordableRoundLot(cash, price, 0.50);
}

BollingerBandStrategy::BollingerBandStrategy(int window)
    : window_(std::max(10, window))
{
}

std::optional<Order> BollingerBandStrategy::onMarketData(const MarketBar& data, const Account& account)
{
    prices_.push_back(data.price);
    if (static_cast<int>(prices_.size()) > window_) {
        prices_.pop_front();
    }
    if (static_cast<int>(prices_.size()) < window_ || !cooldownPassed(data.timestamp, lastSignalTimestamp_)) {
        return std::nullopt;
    }

    const double mean = average();
    const double sigma = stddev(mean);
    const double lower = mean - 2.0 * sigma;
    const double upper = mean + 2.0 * sigma;
    if (data.price < lower && account.position <= 0) {
        const int volume = buyVolume(account.cash, data.price);
        if (volume > 0) {
            lastSignalTimestamp_ = data.timestamp;
            return Order{ nextOrderId_++, true, data.price + 0.10, volume, data.timestamp };
        }
    }
    if ((data.price >= mean || data.price > upper) && account.position > 0) {
        lastSignalTimestamp_ = data.timestamp;
        return Order{ nextOrderId_++, false, data.price - 0.10, account.position, data.timestamp };
    }
    return std::nullopt;
}

double BollingerBandStrategy::average() const
{
    return std::accumulate(prices_.begin(), prices_.end(), 0.0) / prices_.size();
}

double BollingerBandStrategy::stddev(double mean) const
{
    double variance = 0.0;
    for (double price : prices_) {
        const double diff = price - mean;
        variance += diff * diff;
    }
    return std::sqrt(variance / prices_.size());
}

int BollingerBandStrategy::buyVolume(double cash, double price)
{
    return affordableRoundLot(cash, price, 0.55);
}
