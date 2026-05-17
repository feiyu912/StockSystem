#include "TradingCore.h"

#include <algorithm>
#include <cmath>
#include <numeric>

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
    if (order.isBuy && account.cash < order.price * order.volume) {
        return false;
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

std::optional<Order> MovingAverageStrategy::onMarketData(const MarketBar& data, int position)
{
    prices_.push_back(data.price);
    if (static_cast<int>(prices_.size()) > longWindow_) {
        prices_.pop_front();
    }
    if (static_cast<int>(prices_.size()) < longWindow_) {
        return std::nullopt;
    }

    const bool bullish = average(shortWindow_) > average(longWindow_);
    std::optional<Order> signal;
    if (bullish && !lastBullish_ && position <= 0) {
        signal = Order{ nextOrderId_++, true, data.price + 0.10, 100, data.timestamp };
    } else if (!bullish && lastBullish_ && position > 0) {
        signal = Order{ nextOrderId_++, false, data.price - 0.10, std::min(100, position), data.timestamp };
    }
    lastBullish_ = bullish;
    return signal;
}

double MovingAverageStrategy::average(int window) const
{
    const auto first = prices_.end() - window;
    return std::accumulate(first, prices_.end(), 0.0) / window;
}
