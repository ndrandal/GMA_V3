// Market technical-analysis computations. Moves into
// libgma_connector_market in Step 7.
#include "gma/MarketTA.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Logger.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>

namespace gma {

// Minimum threshold for denominators to avoid division by near-zero values.
static constexpr double EPSILON = 1e-6;

static double computeMedian(const std::vector<TickEntry>& prices) {
    if (prices.empty()) return std::numeric_limits<double>::quiet_NaN();
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
    if (series.size() < period || period == 0) return std::numeric_limits<double>::quiet_NaN();
    double k = 2.0 / (period + 1);
    double val = series[series.size() - period];
    for (size_t i = series.size() - period + 1; i < series.size(); ++i)
        val = k * series[i] + (1 - k) * val;
    return val;
}

std::vector<std::pair<std::string, ArgType>> computeAllAtomicValues(
    const std::string& symbol,
    const std::vector<TickEntry>& hist,
    AtomicStore& store,
    const util::Config& cfg
) {
    const size_t n = hist.size();
    if (n == 0) return {};

    // Validate all TA period config values — a negative value cast to size_t
    // wraps to a huge number, causing out-of-bounds reads.  A zero period
    // causes divide-by-zero in SMA/EMA.  Skip the entire computation for
    // obviously broken configs rather than producing garbage.
    if (cfg.taBBands_n <= 0 || cfg.taRSI <= 0 ||
        cfg.taMACD_fast <= 0 || cfg.taMACD_slow <= 0 || cfg.taMACD_signal <= 0 ||
        cfg.taMomentum <= 0 || cfg.taATR <= 0 || cfg.taVolAvg <= 0) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
        "computeAllAtomicValues: skipping — invalid TA config (zero or negative period)");
      return {};
    }

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
        return results;
    }

    results.emplace_back("prevClose", hist[n-2].price);

    // VWAP
    double cumPV = 0.0, cumVol = 0.0;
    for (const auto& e : hist) { cumPV += e.price * e.volume; cumVol += e.volume; }
    results.emplace_back("vwap", cumVol > 0.0 ? (cumPV / cumVol) : std::numeric_limits<double>::quiet_NaN());

    // SMA helper
    auto sma = [&](size_t period) -> double {
        if (n < period) return std::numeric_limits<double>::quiet_NaN();
        double s = 0.0;
        for (size_t i = n - period; i < n; ++i) s += hist[i].price;
        return s / static_cast<double>(period);
    };

    // EMA over hist prices — full-history seeding (standard approach).
    // NOTE: This uses hist[0] as the initial seed and iterates from index 1,
    // whereas emaOverSeries() seeds with series[size-period] and iterates from
    // there. Both are valid EMA implementations; this one includes all history
    // while emaOverSeries() only uses the last `period` data points.
    auto ema = [&](size_t period) -> double {
        if (n < period) return std::numeric_limits<double>::quiet_NaN();
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

    // SMA for each configured period (skip invalid entries)
    for (int period : cfg.taSMA) {
        if (period <= 0) continue;
        double val = sma(static_cast<size_t>(period));
        results.emplace_back("sma_" + std::to_string(period), val);
    }

    // EMA for each configured period (skip invalid entries)
    for (int period : cfg.taEMA) {
        if (period <= 0) continue;
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
        double avgGain = gain / static_cast<double>(rsiP);
        double avgLoss = loss / static_cast<double>(rsiP);
        double rs = avgGain / (avgLoss > EPSILON ? avgLoss : EPSILON);
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

        if (!macdSeries.empty()) {
            double macdLine = macdSeries.back();
            results.emplace_back("macd_line", macdLine);

            double signal = emaOverSeries(macdSeries, macdSig);
            results.emplace_back("macd_signal", signal);
            results.emplace_back("macd_histogram", macdLine - signal);
        } else {
            results.emplace_back("macd_line", 0.0);
            results.emplace_back("macd_signal", 0.0);
            results.emplace_back("macd_histogram", 0.0);
        }
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
        results.emplace_back("roc_" + std::to_string(cfg.taMomentum),
            std::abs(prevM) > EPSILON ? 100.0 * (last - prevM) / prevM
                                      : std::numeric_limits<double>::quiet_NaN());
    }

    // ATR
    const size_t atrP = static_cast<size_t>(cfg.taATR);
    if (n >= atrP + 1) {
        double trSum = 0.0;
        for (size_t i = n - atrP; i < n; ++i)
            trSum += std::abs(hist[i].price - hist[i-1].price);
        // Simplified ATR using |close-to-close| deltas (no high/low data available).
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
    return results;
}

// ---------- MarketTickComputer ----------

MarketTickComputer::MarketTickComputer(const util::Config& cfg)
  : _cfg(cfg)
  , _profile(cfg.sourceProfile)
  , _maxHistory(static_cast<std::size_t>(std::max(1, cfg.taHistoryMax)))
  , _maxSymbols(static_cast<std::size_t>(std::max(1, cfg.maxSymbols)))
{
  // Raw price/volume fields already notified on the raw-tick path in
  // Dispatcher::onTick, or by computeAndStoreAtomics for FunctionMap
  // builtins. Skip re-notifying them from the TA path to avoid duplicate
  // listener fires. TA indicators (sma_5, rsi_14, …) are NOT in this set —
  // they are only notified from here.
  for (const auto& f : {"lastPrice", "openPrice", "highPrice", "lowPrice",
                        "mean", "median", "prevClose", "vwap",
                        "volume", "obv", "volatility_rank"}) {
    _skipFields.insert(f);
  }
  FunctionMap::instance().forEach([this](const std::string& name, const auto&) {
    _skipFields.insert(name);
  });
}

void MarketTickComputer::compute(const Event& tick, engine::ComputeContext& ctx) {
  if (!ctx.store || !tick.payload) return;

  const auto& doc = *tick.payload;

  // Extract price via configured field aliases.
  double price = 0.0;
  bool hasPrice = false;
  for (const auto& pf : _profile.priceFields) {
    if (doc.HasMember(pf.c_str()) && doc[pf.c_str()].IsNumber()) {
      price = doc[pf.c_str()].GetDouble();
      hasPrice = true;
      break;
    }
  }
  if (!hasPrice) return;

  // Extract optional volume / bid / ask / timestamp.
  double volume = 0.0;
  for (const auto& vf : _profile.volumeFields) {
    if (doc.HasMember(vf.c_str()) && doc[vf.c_str()].IsNumber()) {
      volume = doc[vf.c_str()].GetDouble();
      break;
    }
  }
  double bid = 0.0, ask = 0.0;
  std::uint64_t tsNs = 0;
  for (const auto& bf : _profile.bidFields) {
    if (doc.HasMember(bf.c_str()) && doc[bf.c_str()].IsNumber()) {
      bid = doc[bf.c_str()].GetDouble();
      break;
    }
  }
  for (const auto& af : _profile.askFields) {
    if (doc.HasMember(af.c_str()) && doc[af.c_str()].IsNumber()) {
      ask = doc[af.c_str()].GetDouble();
      break;
    }
  }
  if (!_profile.timestampField.empty() &&
      doc.HasMember(_profile.timestampField.c_str()) &&
      doc[_profile.timestampField.c_str()].IsUint64()) {
    tsNs = doc[_profile.timestampField.c_str()].GetUint64();
  }

  if (bid > 0.0) ctx.store->set(tick.symbol, "bid", bid);
  if (ask > 0.0) ctx.store->set(tick.symbol, "ask", ask);
  if (bid > 0.0 && ask > 0.0) ctx.store->set(tick.symbol, "spread", ask - bid);
  if (tsNs > 0) ctx.store->set(tick.symbol, "timestamp", std::to_string(tsNs));

  // Update symbol history under lock; snapshot into contiguous vec for TA.
  std::vector<TickEntry> histVec;
  {
    std::unique_lock<std::shared_mutex> lock(_histMutex);
    if (_symbolHistories.find(tick.symbol) == _symbolHistories.end() &&
        _symbolHistories.size() >= _maxSymbols) {
      return;
    }
    auto& hist = _symbolHistories[tick.symbol];
    hist.push_back(TickEntry{price, volume, bid, ask, tsNs});
    if (hist.size() > _maxHistory) hist.pop_front();
    histVec.assign(hist.begin(), hist.end());
  }

  // Run TA suite or fall back to lightweight base metrics.
  std::vector<std::pair<std::string, ArgType>> taResults;
  if (_profile.taEnabled) {
    taResults = computeAllAtomicValues(tick.symbol, histVec, *ctx.store, _cfg);
  } else {
    taResults.emplace_back("lastPrice", price);
    taResults.emplace_back("volume", volume);
    if (!histVec.empty()) {
      taResults.emplace_back("openPrice", histVec.front().price);
      double high = histVec.front().price, low = high;
      for (const auto& e : histVec) {
        high = std::max(high, e.price);
        low  = std::min(low, e.price);
      }
      taResults.emplace_back("highPrice", high);
      taResults.emplace_back("lowPrice", low);
    }
    ctx.store->setBatch(tick.symbol, taResults);
  }

  if (taResults.empty() || !ctx.dispatcher) return;

  // Notify TA-indicator listeners (sma_N, rsi_N, macd_*, bollinger_*, …).
  // When TA is enabled we skip raw/FunctionMap names to avoid double-notify;
  // when disabled the lightweight path IS the only source, so notify those.
  for (const auto& [key, val] : taResults) {
    if (_profile.taEnabled && _skipFields.count(key)) continue;
    double v = std::visit([](auto&& x) -> double {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<T, double>) return x;
      else if constexpr (std::is_same_v<T, int>) return static_cast<double>(x);
      else return 0.0;
    }, val);
    ctx.dispatcher->notifyListeners(tick.symbol, key, v);
  }
}

} // namespace gma
