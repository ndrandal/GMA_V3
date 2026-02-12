
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
    size_t n = vals.size();
    auto mid = vals.begin() + static_cast<std::ptrdiff_t>(n / 2);
    std::nth_element(vals.begin(), mid, vals.end());
    if (n % 2 == 1) return *mid;
    // For even n, nth_element guarantees elements before mid are <= *mid
    // but we need the max of the left partition for the median average.
    double upper = *mid;
    double lower = *std::max_element(vals.begin(), mid);
    return (lower + upper) * 0.5;
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

    // Accumulate all results locally, then write via a single setBatch call.
    std::vector<std::pair<std::string, ArgType>> results;
    results.reserve(24); // typical max fields

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

    results.emplace_back("lastPrice", last);
    results.emplace_back("openPrice", open);
    results.emplace_back("highPrice", high);
    results.emplace_back("lowPrice", low);
    results.emplace_back("mean", mean);
    results.emplace_back("median", median);

    if (n == 1) {
        store.setBatch(symbol, results);
        return;
    }

    results.emplace_back("prevClose", hist[n-2].price);

    // VWAP
    double cumPV = 0.0, cumVol = 0.0;
    for (const auto& e : hist) { cumPV += e.price * e.volume; cumVol += e.volume; }
    results.emplace_back("vwap", cumVol > 0.0 ? (cumPV / cumVol) : 0.0);

    // SMA helper
    auto sma = [&](size_t period) -> double {
        if (n < period) return 0.0;
        double s = 0.0;
        for (size_t i = n - period; i < n; ++i) s += hist[i].price;
        return s / static_cast<double>(period);
    };

    // EMA over hist prices — full-history seeding (standard approach).
    // Seed at hist[0], iterate forward through all bars.
    // Consistent with the MACD incremental computation below.
    auto ema = [&](size_t period) -> double {
        if (n < period) return 0.0;
        double k = 2.0 / (period + 1);
        double val = hist[0].price;
        for (size_t i = 1; i < n; ++i)
            val = k * hist[i].price + (1 - k) * val;
        return val;
    };

    // Pre-compute SMA(20) once for storage, Bollinger, and volatility_rank
    double sma20 = sma(20);
    const bool have20 = (n >= 20);

    results.emplace_back("sma_5",  sma(5));
    results.emplace_back("sma_20", sma20);
    results.emplace_back("ema_12", ema(12));
    results.emplace_back("ema_26", ema(26));

    // RSI(14)
    if (n >= 15) {
        double gain = 0.0, loss = 0.0;
        for (size_t i = n - 14; i < n; ++i) {
            double d = hist[i].price - hist[i-1].price;
            if (d > 0) gain += d;
            else loss += -d;
        }
        double rs = gain / (loss > 0.0 ? loss : 1e-6);
        results.emplace_back("rsi_14", 100.0 - (100.0 / (1.0 + rs)));
    }

    // MACD: line = EMA(12) - EMA(26), signal = 9-period EMA of MACD line series
    // O(n) single-pass: seed both EMAs at hist[0], update incrementally.
    // Uses the same full-history seeding as the ema() lambda above.
    if (n >= 26) {
        const double k12 = 2.0 / (12 + 1);
        const double k26 = 2.0 / (26 + 1);

        double ema12val = hist[0].price;
        double ema26val = hist[0].price;

        std::vector<double> macdSeries;
        macdSeries.reserve(n - 25);

        for (size_t i = 1; i < n; ++i) {
            double p = hist[i].price;
            ema12val = k12 * p + (1.0 - k12) * ema12val;
            ema26val = k26 * p + (1.0 - k26) * ema26val;

            if (i >= 25) {
                macdSeries.push_back(ema12val - ema26val);
            }
        }

        double macdLine = macdSeries.back();
        results.emplace_back("macd_line", macdLine);

        double signal = emaOverSeries(macdSeries, 9);
        results.emplace_back("macd_signal", signal);
        results.emplace_back("macd_histogram", macdLine - signal);
    } else {
        double macdLine = ema(12) - ema(26);
        results.emplace_back("macd_line", macdLine);
        results.emplace_back("macd_signal", 0.0);
        results.emplace_back("macd_histogram", 0.0);
    }

    // Compute stddev(20) for Bollinger + volatility_rank (reuses sma20)
    double stddev20 = 0.0;
    if (have20) {
        double sumSq = 0.0;
        for (size_t i = n - 20; i < n; ++i) {
            double d = hist[i].price - sma20;
            sumSq += d * d;
        }
        stddev20 = std::sqrt(sumSq / 20.0);
    }

    // Bollinger Bands (20 period, 2 stddev)
    if (have20) {
        results.emplace_back("bollinger_upper", sma20 + 2.0 * stddev20);
        results.emplace_back("bollinger_lower", sma20 - 2.0 * stddev20);
    }

    // Momentum and ROC (10)
    if (n >= 11) {
        double prev10 = hist[n-11].price;
        results.emplace_back("momentum_10", last - prev10);
        results.emplace_back("roc_10", 100.0 * (last - prev10) / (prev10 != 0.0 ? prev10 : 1e-6));
    }

    // ATR(14)
    if (n >= 15) {
        double trSum = 0.0;
        for (size_t i = n - 14; i < n; ++i)
            trSum += std::abs(hist[i].price - hist[i-1].price);
        results.emplace_back("atr_14", trSum / 14.0);
    }

    // Volume metrics
    results.emplace_back("volume", hist.back().volume);
    if (have20) {
        double vol20 = 0.0;
        for (size_t i = n - 20; i < n; ++i) vol20 += hist[i].volume;
        results.emplace_back("volume_avg_20", vol20 / 20.0);
    }

    // On-balance volume
    double obv = 0.0;
    for (size_t i = 1; i < n; ++i)
        obv += (hist[i].price > hist[i-1].price ? hist[i].volume :
                (hist[i].price < hist[i-1].price ? -hist[i].volume : 0.0));
    results.emplace_back("obv", obv);

    // Volatility rank (stddev/mean capped at 1) — reuses sma20/stddev20
    if (mean != 0.0 && have20) {
        results.emplace_back("volatility_rank", std::min(stddev20 / std::abs(mean), 1.0));
    }

    // Single lock acquisition for all writes
    store.setBatch(symbol, results);
}

} // namespace gma
