// File: src/main.cpp
#include "gma/AtomicStore.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/Config.hpp"
#include "gma/server/WebSocketServer.hpp"
#include "gma/server/FeedServer.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <string>
#include <numeric>

int main(int argc, char* argv[]) {
    // Default port for WebSocketServer

    unsigned short port = 9002;
    if (argc > 1) {
        try {
            port = static_cast<unsigned short>(std::stoul(argv[1]));
        } catch (const std::exception& ex) {
            std::cerr << "Invalid port '" << argv[1] << "': " << ex.what() << std::endl;
            return EXIT_FAILURE;
        }
    }

    // Core components
    gma::AtomicStore store;
    gma::ThreadPool pool(gma::Config::ThreadPoolSize);
    gma::ExecutionContext ctx(&store, &pool);
    gma::MarketDispatcher dispatcher(&pool, &store);

    auto& fm = gma::FunctionMap::instance();

    // ---- Price‑based metrics ----
    fm.registerFunction("lastPrice", [](auto const& h){
      return h.empty() ? 0.0 : h.back();
    });
    fm.registerFunction("openPrice", [](auto const& h){
      return h.empty() ? 0.0 : h.front();
    });
    fm.registerFunction("highPrice", [](auto const& h){
      return h.empty() ? 0.0 : *std::max_element(h.begin(), h.end());
    });
    fm.registerFunction("lowPrice", [](auto const& h){
      return h.empty() ? 0.0 : *std::min_element(h.begin(), h.end());
    });
    fm.registerFunction("mean", [](auto const& h){
      if (h.empty()) return 0.0;
      double sum = std::accumulate(h.begin(), h.end(), 0.0);
      return sum / h.size();
    });
    fm.registerFunction("median", [](auto h){
      if (h.empty()) return 0.0;
      std::sort(h.begin(), h.end());
      size_t n = h.size();
      return (n % 2 == 1)
            ? h[n/2]
            : (h[n/2 - 1] + h[n/2]) * 0.5;
    });
    fm.registerFunction("prevClose", [](auto const& h){
      return h.size() > 1 ? h[h.size()-2] : 0.0;
    });

    // ---- VWAP (stub) ----
    // Needs volume info; placeholder returns lastPrice
    fm.registerFunction("vwap", [](auto const& h){
      return h.empty() ? 0.0 : h.back();
    });

    // ---- Simple & Exponential MAs ----
    fm.registerFunction("sma_5", [](auto const& h){
      if (h.size() < 5) return 0.0;
      double sum = std::accumulate(h.end()-5, h.end(), 0.0);
      return sum / 5.0;
    });
    fm.registerFunction("sma_20", [](auto const& h){
      if (h.size() < 20) return 0.0;
      double sum = std::accumulate(h.end()-20, h.end(), 0.0);
      return sum / 20.0;
    });
    fm.registerFunction("ema_12", [](auto const& h){
      size_t n = h.size();
      if (n < 12) return 0.0;
      double k = 2.0 / (12 + 1), val = h[n-12];
      for (size_t i = n-11; i < n; ++i)
        val = k * h[i] + (1 - k) * val;
      return val;
    });
    fm.registerFunction("ema_26", [](auto const& h){
      size_t n = h.size();
      if (n < 26) return 0.0;
      double k = 2.0 / (26 + 1), val = h[n-26];
      for (size_t i = n-25; i < n; ++i)
        val = k * h[i] + (1 - k) * val;
      return val;
    });

    // ---- RSI 14 ----
    fm.registerFunction("rsi_14", [](auto const& h){
      size_t n = h.size();
      if (n < 15) return 0.0;
      double gain=0.0, loss=0.0;
      for (size_t i = n-14; i < n; ++i) {
        double d = h[i] - h[i-1];
        (d > 0 ? gain : loss) += std::abs(d);
      }
      double rs = gain / (loss > 0.0 ? loss : 1e-6);
      return 100.0 - (100.0 / (1.0 + rs));
    });

    // ---- MACD & Signal ----
    fm.registerFunction("macd_line", [](auto const& h){
      auto ema_f = [&](size_t p){
        size_t n = h.size();
        if (n < p) return 0.0;
        double k = 2.0/(p+1), v = h[n-p];
        for (size_t i = n-p+1; i < n; ++i)
          v = k*h[i] + (1-k)*v;
        return v;
      };
      return ema_f(12) - ema_f(26);
    });
    fm.registerFunction("macd_signal", [](auto const& h){
      // true 9‑period EMA on MACD history requires maintaining MACD series.
      // As a placeholder, we return the latest MACD_line.
      auto ema_f = [&](size_t p){
        size_t n = h.size();
        if (n < p) return 0.0;
        double k = 2.0/(p+1), v = h[n-p];
        for (size_t i = n-p+1; i < n; ++i)
          v = k*h[i] + (1-k)*v;
        return v;
      };
      double macd = ema_f(12) - ema_f(26);
      return macd;
    });

    // ---- Bollinger Bands ----
    fm.registerFunction("bollinger_upper", [](auto h){
      if (h.size() < 20) return 0.0;
      size_t n = h.size();
      double sum=0.0;
      for (size_t i=n-20; i<n; ++i) sum += h[i];
      double m = sum/20.0, ss=0.0;
      for (size_t i=n-20; i<n; ++i) ss += (h[i]-m)*(h[i]-m);
      double sd = std::sqrt(ss/20.0);
      return m + 2.0*sd;
    });
    fm.registerFunction("bollinger_lower", [](auto h){
      if (h.size() < 20) return 0.0;
      size_t n = h.size();
      double sum=0.0;
      for (size_t i=n-20; i<n; ++i) sum += h[i];
      double m = sum/20.0, ss=0.0;
      for (size_t i=n-20; i<n; ++i) ss += (h[i]-m)*(h[i]-m);
      double sd = std::sqrt(ss/20.0);
      return m - 2.0*sd;
    });

    // ---- Momentum & ROC ----
    fm.registerFunction("momentum_10", [](auto const& h){
      size_t n = h.size();
      return n > 10 ? h[n-1] - h[n-11] : 0.0;
    });
    fm.registerFunction("roc_10", [](auto const& h){
      size_t n = h.size();
      if (n <= 10) return 0.0;
      double prev = h[n-11];
      return prev != 0.0 ? 100.0 * (h[n-1]-prev)/prev : 0.0;
    });

    // ---- Stubs for volume & TR‑based metrics ----
    fm.registerFunction("vwap",           [](auto const&){ return 0.0; });
    fm.registerFunction("atr_14",         [](auto const&){ return 0.0; });
    fm.registerFunction("volume",         [](auto const&){ return 0.0; });
    fm.registerFunction("volume_avg_20",  [](auto const&){ return 0.0; });
    fm.registerFunction("obv",            [](auto const&){ return 0.0; });
    fm.registerFunction("volatility_rank",[](auto const&){ return 0.0; });

    // ---- Constant fields ----
    fm.registerFunction("isHalted",      [](auto const&){ return 0.0; });
    fm.registerFunction("timeSinceOpen", [](auto const&){ return 60.0; });
    fm.registerFunction("timeUntilClose",[](auto const&){ return 300.0; });

    // Launch server
    boost::asio::io_context ioc;
    gma::WebSocketServer server(ioc, &ctx, &dispatcher, port);
    server.run();

        // Tick server
    server::FeedServer feedSrv{ioc, dispatcher, 9001};
    feedSrv.run();

    std::cout << "gma WebSocket server listening on port " << port << std::endl;
    ioc.run();

    return EXIT_SUCCESS;
}
