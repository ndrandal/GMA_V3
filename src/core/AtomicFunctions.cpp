
// File: src/core/AtomicFunctions.cpp
#include "gma/AtomicFunctions.hpp"
#include "gma/FunctionMap.hpp"
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
    AtomicStore& store,
    const util::Config& cfg
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
    auto ema = [&](size_t period) -> double {
        if (n < period) return 0.0;
        double k = 2.0 / (period + 1);
        double val = hist[0].price;
        for (size_t i = 1; i < n; ++i)
            val = k * hist[i].price + (1 - k) * val;
        return val;
    };

    // Pre-compute SMA(BBands_n) once for Bollinger and volatility_rank
    const size_t bbandsN = static_cast<size_t>(cfg.taBBands_n);
    double smaBB = sma(bbandsN);
    const bool haveBB = (n >= bbandsN);

    // SMA for each configured period
    for (int period : cfg.taSMA) {
        double val = sma(static_cast<size_t>(period));
        results.emplace_back("sma_" + std::to_string(period), val);
    }

    // EMA for each configured period
    for (int period : cfg.taEMA) {
        results.emplace_back("ema_" + std::to_string(period), ema(static_cast<size_t>(period)));
    }

    // RSI
    const size_t rsiP = static_cast<size_t>(cfg.taRSI);
    if (n >= rsiP + 1) {
        double gain = 0.0, loss = 0.0;
        for (size_t i = n - rsiP; i < n; ++i) {
            double d = hist[i].price - hist[i-1].price;
            if (d > 0) gain += d;
            else loss += -d;
        }
        double rs = gain / (loss > 0.0 ? loss : 1e-6);
        results.emplace_back("rsi_" + std::to_string(cfg.taRSI), 100.0 - (100.0 / (1.0 + rs)));
    }

    // MACD
    const size_t macdFast = static_cast<size_t>(cfg.taMACD_fast);
    const size_t macdSlow = static_cast<size_t>(cfg.taMACD_slow);
    const size_t macdSig  = static_cast<size_t>(cfg.taMACD_signal);
    if (n >= macdSlow) {
        const double kFast = 2.0 / (macdFast + 1);
        const double kSlow = 2.0 / (macdSlow + 1);

        double emaFastVal = hist[0].price;
        double emaSlowVal = hist[0].price;

        std::vector<double> macdSeries;
        macdSeries.reserve(n - macdSlow + 1);

        for (size_t i = 1; i < n; ++i) {
            double p = hist[i].price;
            emaFastVal = kFast * p + (1.0 - kFast) * emaFastVal;
            emaSlowVal = kSlow * p + (1.0 - kSlow) * emaSlowVal;

            if (i >= macdSlow - 1) {
                macdSeries.push_back(emaFastVal - emaSlowVal);
            }
        }

        double macdLine = macdSeries.back();
        results.emplace_back("macd_line", macdLine);

        double signal = emaOverSeries(macdSeries, macdSig);
        results.emplace_back("macd_signal", signal);
        results.emplace_back("macd_histogram", macdLine - signal);
    } else {
        double macdLine = ema(macdFast) - ema(macdSlow);
        results.emplace_back("macd_line", macdLine);
        results.emplace_back("macd_signal", 0.0);
        results.emplace_back("macd_histogram", 0.0);
    }

    // Compute stddev(BBands_n) for Bollinger + volatility_rank (reuses smaBB)
    double stddevBB = 0.0;
    if (haveBB) {
        double sumSq = 0.0;
        for (size_t i = n - bbandsN; i < n; ++i) {
            double d = hist[i].price - smaBB;
            sumSq += d * d;
        }
        stddevBB = std::sqrt(sumSq / static_cast<double>(bbandsN));
    }

    // Bollinger Bands
    if (haveBB) {
        results.emplace_back("bollinger_upper", smaBB + cfg.taBBands_stdK * stddevBB);
        results.emplace_back("bollinger_lower", smaBB - cfg.taBBands_stdK * stddevBB);
    }

    // Momentum and ROC
    const size_t momP = static_cast<size_t>(cfg.taMomentum);
    if (n >= momP + 1) {
        double prevM = hist[n - momP - 1].price;
        results.emplace_back("momentum_" + std::to_string(cfg.taMomentum), last - prevM);
        results.emplace_back("roc_" + std::to_string(cfg.taMomentum), 100.0 * (last - prevM) / (prevM != 0.0 ? prevM : 1e-6));
    }

    // ATR
    const size_t atrP = static_cast<size_t>(cfg.taATR);
    if (n >= atrP + 1) {
        double trSum = 0.0;
        for (size_t i = n - atrP; i < n; ++i)
            trSum += std::abs(hist[i].price - hist[i-1].price);
        results.emplace_back("atr_" + std::to_string(cfg.taATR), trSum / static_cast<double>(atrP));
    }

    // Volume metrics
    results.emplace_back("volume", hist.back().volume);
    const size_t volP = static_cast<size_t>(cfg.taVolAvg);
    if (n >= volP) {
        double vol = 0.0;
        for (size_t i = n - volP; i < n; ++i) vol += hist[i].volume;
        results.emplace_back("volume_avg_" + std::to_string(cfg.taVolAvg), vol / static_cast<double>(volP));
    }

    // On-balance volume
    double obv = 0.0;
    for (size_t i = 1; i < n; ++i)
        obv += (hist[i].price > hist[i-1].price ? hist[i].volume :
                (hist[i].price < hist[i-1].price ? -hist[i].volume : 0.0));
    results.emplace_back("obv", obv);

    // Volatility rank (stddev/mean capped at 1) — reuses smaBB/stddevBB
    if (mean != 0.0 && haveBB) {
        results.emplace_back("volatility_rank", std::min(stddevBB / std::abs(mean), 1.0));
    }

    // Single lock acquisition for all writes
    store.setBatch(symbol, results);
}

void registerBuiltinFunctions() {
    auto& fm = FunctionMap::instance();

    fm.registerFunction("mean", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double s = 0.0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    });

    fm.registerFunction("sum", [](const std::vector<double>& v) -> double {
        double s = 0.0;
        for (double x : v) s += x;
        return s;
    });

    fm.registerFunction("min", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double m = v[0];
        for (size_t i = 1; i < v.size(); ++i) m = std::min(m, v[i]);
        return m;
    });

    fm.registerFunction("max", [](const std::vector<double>& v) -> double {
        if (v.empty()) return 0.0;
        double m = v[0];
        for (size_t i = 1; i < v.size(); ++i) m = std::max(m, v[i]);
        return m;
    });

    fm.registerFunction("last", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : v.back();
    });

    fm.registerFunction("first", [](const std::vector<double>& v) -> double {
        return v.empty() ? 0.0 : v.front();
    });

    fm.registerFunction("count", [](const std::vector<double>& v) -> double {
        return static_cast<double>(v.size());
    });

    fm.registerFunction("stddev", [](const std::vector<double>& v) -> double {
        if (v.size() < 2) return 0.0;
        double n = static_cast<double>(v.size());
        double s = 0.0;
        for (double x : v) s += x;
        double mean = s / n;
        double ss = 0.0;
        for (double x : v) { double d = x - mean; ss += d * d; }
        return std::sqrt(ss / n);
    });
}

} // namespace gma
