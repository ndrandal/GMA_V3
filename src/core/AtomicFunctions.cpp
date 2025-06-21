
// File: src/core/AtomicFunctions.cpp
#include "gma/AtomicFunctions.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>

namespace gma {

static double computeMedian(std::deque<TickEntry> prices) {
    if (prices.empty()) return 0.0;
    std::vector<double> vals;
    vals.reserve(prices.size());
    for (auto& e : prices) vals.push_back(e.price);
    std::sort(vals.begin(), vals.end());
    size_t n = vals.size();
    if (n % 2 == 1) return vals[n/2];
    return (vals[n/2 - 1] + vals[n/2]) * 0.5;
}

void computeAllAtomicValues(
    const std::string& symbol,
    const SymbolHistory& hist,
    AtomicStore& store
) {
    const size_t n = hist.size();
    if (n == 0) return;

    // Basic prices
    double open = hist.front().price;
    double last = hist.back().price;
    double high = open;
    double low  = open;
    double sum = 0.0;
    for (auto& e : hist) {
        double p = e.price;
        high = std::max(high, p);
        low  = std::min(low,  p);
        sum += p;
    }
    double mean = sum / static_cast<double>(n);
    double median = computeMedian(hist);

    store.set(symbol, "lastPrice", last);
    store.set(symbol, "openPrice", open);
    store.set(symbol, "highPrice", high);
    store.set(symbol, "lowPrice", low);
    store.set(symbol, "mean", mean);
    store.set(symbol, "median", median);

    // If only one sample, stop here
    if (n == 1) return;

    // Additional indicators based on volume and multi-bar history
    // previous close
    store.set(symbol, "prevClose", hist[n-2].price);
    // VWAP
    double cumPV = 0.0, cumVol = 0.0;
    for (auto& e : hist) { cumPV += e.price * e.volume; cumVol += e.volume; }
    store.set(symbol, "vwap", cumVol > 0.0 ? (cumPV / cumVol) : 0.0);
    // SMA and EMA
    auto sma = [&](size_t period) {
        if (n < period) return 0.0;
        double s = 0.0;
        for (size_t i = n-period; i < n; ++i) s += hist[i].price;
        return s / static_cast<double>(period);
    };
    auto ema = [&](size_t period) {
        if (n < period) return 0.0;
        double k = 2.0 / (period + 1);
        double val = hist[n-period].price;
        for (size_t i = n-period+1; i < n; ++i)
            val = k * hist[i].price + (1 - k) * val;
        return val;
    };
    store.set(symbol, "sma_5",  sma(5));
    store.set(symbol, "sma_20", sma(20));
    store.set(symbol, "ema_12", ema(12));
    store.set(symbol, "ema_26", ema(26));

    // RSI(14)
    if (n >= 15) {
        double gain = 0.0, loss = 0.0;
        for (size_t i = n-14; i < n; ++i) {
            double d = hist[i].price - hist[i-1].price;
            if (d > 0) gain += d;
            else loss += -d;
        }
        double rs = gain / (loss > 0.0 ? loss : 1e-6);
        store.set(symbol, "rsi_14", 100.0 - (100.0 / (1.0 + rs)));
    }

    // MACD
    double macdLine = ema(12) - ema(26);
    store.set(symbol, "macd_line", macdLine);
    // signal (9-period EMA of MACD line)
    SymbolHistory macdHist = hist;
    for (auto& e : macdHist) e.price = macdLine;
    store.set(symbol, "macd_signal", ema(9));

    // Bollinger Bands (20 period, 2 stddev)
    if (n >= 20) {
        double m20 = sma(20);
        double sumSq = 0.0;
        for (size_t i = n-20; i < n; ++i) {
            double d = hist[i].price - m20;
            sumSq += d * d;
        }
        double sd = std::sqrt(sumSq / 20.0);
        store.set(symbol, "bollinger_upper", m20 + 2.0 * sd);
        store.set(symbol, "bollinger_lower", m20 - 2.0 * sd);
    }

    // Momentum and ROC (10)
    if (n >= 11) {
        double prev10 = hist[n-11].price;
        store.set(symbol, "momentum_10", last - prev10);
        store.set(symbol, "roc_10", 100.0 * (last - prev10) / (prev10 != 0.0 ? prev10 : 1e-6));
    }

    // ATR(14)
    if (n >= 15) {
        double trSum = 0.0;
        for (size_t i = n-14; i < n; ++i)
            trSum += std::abs(hist[i].price - hist[i-1].price);
        store.set(symbol, "atr_14", trSum / 14.0);
    }

    // Volume metrics
    store.set(symbol, "volume", hist.back().volume);
    if (n >= 20) {
        double vol20 = 0.0;
        for (size_t i = n-20; i < n; ++i) vol20 += hist[i].volume;
        store.set(symbol, "volume_avg_20", vol20 / 20.0);
    }

    // On-balance volume
    double obv = 0.0;
    for (size_t i = 1; i < n; ++i)
        obv += (hist[i].price > hist[i-1].price ? hist[i].volume :
                (hist[i].price < hist[i-1].price ? -hist[i].volume : 0.0));
    store.set(symbol, "obv", obv);

    // Volatility rank (stddev/mean capped at 1)
    if (mean != 0.0) {
        double sd20 = 0.0;
        if (n >= 20) {
            double m20 = sma(20);
            double ss = 0.0;
            for (size_t i = n-20; i < n; ++i) {
                double d = hist[i].price - m20;
                ss += d * d;
            }
            sd20 = std::sqrt(ss / 20.0);
        }
        store.set(symbol, "volatility_rank", std::min(sd20 / std::abs(mean), 1.0));
    }

    // Static placeholders
    store.set(symbol, "isHalted", 0);
    store.set(symbol, "marketState", std::string("Open"));
    store.set(symbol, "timeSinceOpen", 60);
    store.set(symbol, "timeUntilClose", 300);
}

} // namespace gma
