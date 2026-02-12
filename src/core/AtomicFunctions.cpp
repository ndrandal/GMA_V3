
// File: src/core/AtomicFunctions.cpp
#include "gma/AtomicFunctions.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>

namespace gma {

static double computeMedian(const SymbolHistory& prices) {
    if (prices.empty()) return 0.0;
    std::vector<double> vals;
    vals.reserve(prices.size());
    for (const auto& e : prices) vals.push_back(e.price);
    std::sort(vals.begin(), vals.end());
    size_t n = vals.size();
    if (n % 2 == 1) return vals[n/2];
    return (vals[n/2 - 1] + vals[n/2]) * 0.5;
}

// EMA over a raw double series
static double emaOverSeries(const std::vector<double>& series, size_t period) {
    if (series.size() < period || period == 0) return 0.0;
    double k = 2.0 / (period + 1);
    double val = series[series.size() - period];
    for (size_t i = series.size() - period + 1; i < series.size(); ++i)
        val = k * series[i] + (1 - k) * val;
    return val;
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
    for (const auto& e : hist) {
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

    if (n == 1) return;

    store.set(symbol, "prevClose", hist[n-2].price);

    // VWAP
    double cumPV = 0.0, cumVol = 0.0;
    for (const auto& e : hist) { cumPV += e.price * e.volume; cumVol += e.volume; }
    store.set(symbol, "vwap", cumVol > 0.0 ? (cumPV / cumVol) : 0.0);

    // SMA helper
    auto sma = [&](size_t period) -> double {
        if (n < period) return 0.0;
        double s = 0.0;
        for (size_t i = n - period; i < n; ++i) s += hist[i].price;
        return s / static_cast<double>(period);
    };

    // EMA over hist prices
    auto ema = [&](size_t period) -> double {
        if (n < period) return 0.0;
        double k = 2.0 / (period + 1);
        double val = hist[n - period].price;
        for (size_t i = n - period + 1; i < n; ++i)
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
        for (size_t i = n - 14; i < n; ++i) {
            double d = hist[i].price - hist[i-1].price;
            if (d > 0) gain += d;
            else loss += -d;
        }
        double rs = gain / (loss > 0.0 ? loss : 1e-6);
        store.set(symbol, "rsi_14", 100.0 - (100.0 / (1.0 + rs)));
    }

    // MACD: line = EMA(12) - EMA(26), signal = 9-period EMA of MACD line series
    if (n >= 26) {
        // Build the MACD line series for all bars where both EMAs are valid
        std::vector<double> macdSeries;
        macdSeries.reserve(n - 25);
        for (size_t end = 26; end <= n; ++end) {
            // Compute EMA(12) and EMA(26) over hist[0..end)
            auto emaAt = [&](size_t period, size_t count) -> double {
                if (count < period) return 0.0;
                double k = 2.0 / (period + 1);
                double val = hist[count - period].price;
                for (size_t i = count - period + 1; i < count; ++i)
                    val = k * hist[i].price + (1 - k) * val;
                return val;
            };
            double e12 = emaAt(12, end);
            double e26 = emaAt(26, end);
            macdSeries.push_back(e12 - e26);
        }

        double macdLine = macdSeries.back();
        store.set(symbol, "macd_line", macdLine);

        // Signal = 9-period EMA of the MACD line series
        double signal = emaOverSeries(macdSeries, 9);
        store.set(symbol, "macd_signal", signal);
        store.set(symbol, "macd_histogram", macdLine - signal);
    } else {
        double macdLine = ema(12) - ema(26);
        store.set(symbol, "macd_line", macdLine);
        store.set(symbol, "macd_signal", 0.0);
        store.set(symbol, "macd_histogram", 0.0);
    }

    // Bollinger Bands (20 period, 2 stddev)
    if (n >= 20) {
        double m20 = sma(20);
        double sumSq = 0.0;
        for (size_t i = n - 20; i < n; ++i) {
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
        for (size_t i = n - 14; i < n; ++i)
            trSum += std::abs(hist[i].price - hist[i-1].price);
        store.set(symbol, "atr_14", trSum / 14.0);
    }

    // Volume metrics
    store.set(symbol, "volume", hist.back().volume);
    if (n >= 20) {
        double vol20 = 0.0;
        for (size_t i = n - 20; i < n; ++i) vol20 += hist[i].volume;
        store.set(symbol, "volume_avg_20", vol20 / 20.0);
    }

    // On-balance volume
    double obv = 0.0;
    for (size_t i = 1; i < n; ++i)
        obv += (hist[i].price > hist[i-1].price ? hist[i].volume :
                (hist[i].price < hist[i-1].price ? -hist[i].volume : 0.0));
    store.set(symbol, "obv", obv);

    // Volatility rank (stddev/mean capped at 1)
    if (mean != 0.0 && n >= 20) {
        double m20 = sma(20);
        double ss = 0.0;
        for (size_t i = n - 20; i < n; ++i) {
            double d = hist[i].price - m20;
            ss += d * d;
        }
        double sd20 = std::sqrt(ss / 20.0);
        store.set(symbol, "volatility_rank", std::min(sd20 / std::abs(mean), 1.0));
    }
}

} // namespace gma
