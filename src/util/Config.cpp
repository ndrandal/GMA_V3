#include "gma/util/Config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sstream>
#include <algorithm>

#include "gma/engine/ConfigNamespaceRegistry.hpp"
#include "gma/util/Logger.hpp"

#ifdef _WIN32
  #define NOMINMAX
  #include <windows.h>
#endif

namespace gma {
namespace util {

std::string Config::trim(const std::string& s) {
  const auto is_ws = [](unsigned char c){ return std::isspace(c) != 0; };
  auto b = std::find_if_not(s.begin(), s.end(), is_ws);
  auto e = std::find_if_not(s.rbegin(), s.rend(), is_ws).base();
  if (b >= e) return {};
  return std::string(b, e);
}

bool Config::parseLineKV(const std::string& line, std::string& k, std::string& v) {
  auto pos = line.find('=');
  if (pos == std::string::npos) return false;
  k = trim(line.substr(0, pos));
  v = trim(line.substr(pos + 1));
  if (k.empty()) return false;
  return true;
}

bool Config::loadFromFile(const std::string& path) {
  // Simple INI-ish parser: key=value per line, '#' or ';' start comments.
  // Unknown keys are ignored so you can add new knobs without breaking older builds.

  FILE* f = nullptr;

#ifdef _WIN32
  if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
    return false;
  }
#else
  f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
#endif

  auto closeFile = [](FILE* fp) { if (fp) std::fclose(fp); };
  std::unique_ptr<FILE, decltype(closeFile)> fileGuard(f, closeFile);

  std::string line;
  line.reserve(1024);

  // Read whole file
  while (true) {
    char tmp[1024];
    if (!std::fgets(tmp, sizeof(tmp), f)) break;
    line.assign(tmp);

    // Detect truncated lines (fgets fills the buffer without a newline).
    // Discard the rest of the line to avoid silent data corruption.
    if (line.size() == sizeof(tmp) - 1 && !line.empty() && line.back() != '\n') {
      // Consume remainder of the oversized line
      while (std::fgets(tmp, sizeof(tmp), f)) {
        if (std::strchr(tmp, '\n')) break;
      }
      continue; // skip this line entirely
    }

    // Strip CR/LF
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    auto s = trim(line);
    if (s.empty()) continue;
    if (s[0] == '#' || s[0] == ';') continue; // comment

    std::string key, val;
    if (!parseLineKV(s, key, val)) continue;

    // NOTE: std::atoi returns 0 for non-numeric input. All period values are
    // guarded by std::max(1, ...) and port/size values have explicit range
    // checks, so invalid input produces safe defaults rather than UB.
    if      (key == "taMACD_fast")    taMACD_fast    = std::max(1, std::atoi(val.c_str()));
    else if (key == "taMACD_slow")    taMACD_slow    = std::max(1, std::atoi(val.c_str()));
    else if (key == "taBBands_n")     taBBands_n     = std::max(1, std::atoi(val.c_str()));
    else if (key == "taBBands_stdK")  taBBands_stdK  = std::atof(val.c_str());
    else if (key == "taRSI")          taRSI          = std::max(1, std::atoi(val.c_str()));
    else if (key == "taATR")          taATR          = std::max(1, std::atoi(val.c_str()));
    else if (key == "taMomentum")     taMomentum     = std::max(1, std::atoi(val.c_str()));
    else if (key == "taMACD_signal")  taMACD_signal  = std::max(1, std::atoi(val.c_str()));
    else if (key == "taVolAvg")       taVolAvg       = std::max(1, std::atoi(val.c_str()));
    else if (key == "wsPort")        { int p = std::atoi(val.c_str()); if (p > 0 && p <= 65535) wsPort = p; }
    else if (key == "feedPort")      { int p = std::atoi(val.c_str()); if (p > 0 && p <= 65535) feedPort = p; }
    else if (key == "threadPoolSize") { int v = std::atoi(val.c_str()); if (v >= 0) threadPoolSize = v; }
    else if (key == "taHistoryMax") { int v = std::atoi(val.c_str()); if (v > 0) taHistoryMax = v; }
    else if (key == "maxSymbols") { int v = std::atoi(val.c_str()); if (v > 0) maxSymbols = v; }
    else if (key == "maxFieldsPerSymbol") { int v = std::atoi(val.c_str()); if (v > 0) maxFieldsPerSymbol = v; }
    else if (key == "allowNegativePrices") { allowNegativePrices = (val == "true" || val == "1" || val == "yes"); }
    // Canonical ingress entries: ingress.N.kind = ..., ingress.N.<param> = ...
    else if (key.size() > 8 && key.substr(0, 8) == "ingress.") {
      auto dot2 = key.find('.', 8);
      if (dot2 != std::string::npos) {
        std::string idxStr = key.substr(8, dot2 - 8);
        if (!idxStr.empty() && std::isdigit(static_cast<unsigned char>(idxStr[0]))) {
          int idx = std::atoi(idxStr.c_str());
          if (idx >= 0 && idx < 64) {
            while (static_cast<int>(ingress.size()) <= idx) ingress.emplace_back();
            std::string field = key.substr(dot2 + 1);
            if (field == "kind") {
              ingress[idx].kind = val;
            } else {
              ingress[idx].params[field] = val;
            }
          }
        }
      }
    }
    // Multi-feed config: feed.0.url, feed.0.adapter, feed.0.symbols, etc.
    else if (key.substr(0, 5) == "feed." && key.size() > 5) {
      // Parse "feed.N.field"
      auto dot2 = key.find('.', 5);
      if (dot2 != std::string::npos) {
        std::string idxStr = key.substr(5, dot2 - 5);
        if (idxStr.empty() || !std::isdigit(static_cast<unsigned char>(idxStr[0]))) continue;
        int idx = std::atoi(idxStr.c_str());
        std::string field = key.substr(dot2 + 1);
        if (idx >= 0 && idx < 64) {
          while (static_cast<int>(feeds.size()) <= idx) feeds.emplace_back();
          auto& fc = feeds[static_cast<size_t>(idx)];
          if (field == "url") fc.url = val;
          else if (field == "adapter") fc.adapter = val;
          else if (field == "symbols") {
            fc.symbols.clear();
            std::istringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
              auto t = trim(tok);
              if (!t.empty()) fc.symbols.push_back(t);
            }
            if (fc.symbols.empty()) fc.symbols = {"*"};
          }
        }
      }
    }
    else if (key == "metricsEnabled") { metricsEnabled = (val == "true" || val == "1" || val == "yes"); }
    else if (key == "metricsIntervalSec") { int v = std::atoi(val.c_str()); if (v > 0) metricsIntervalSec = v; }
    else if (key == "logLevel") { logLevel = val; }
    else if (key == "forumUrl") { forumUrl = val; }
    else if (key == "forumTenantId") { forumTenantId = val; }
    else if (key == "forumAuthToken") { forumAuthToken = val; }
    else if (key == "feedUrl") { feedUrl = val; }
    else if (key == "feedSymbols") {
      feedSymbols.clear();
      std::istringstream ss(val);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        auto t = trim(tok);
        if (!t.empty()) feedSymbols.push_back(t);
      }
      if (feedSymbols.empty()) feedSymbols = {"*"};
    }
    else if (key == "taSMA") {
      taSMA.clear();
      std::istringstream ss(val);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        int v = std::atoi(trim(tok).c_str());
        if (v > 0) taSMA.push_back(v);
      }
      if (taSMA.empty()) taSMA = {5, 20};
    }
    else if (key == "taEMA") {
      taEMA.clear();
      std::istringstream ss(val);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        int v = std::atoi(trim(tok).c_str());
        if (v > 0) taEMA.push_back(v);
      }
      if (taEMA.empty()) taEMA = {12, 26};
    }
    else {
      // Unknown to the engine — park for later replay through
      // ConfigNamespaceRegistry once connectors have registered their
      // readers. See dispatchPendingKeys().
      _pendingKeys.emplace_back(std::move(key), std::move(val));
    }
  }

  // fileGuard closes f automatically via RAII.

  synthesizeIngressFromLegacy();

  // Basic sanity: slow >= fast for MACD; stdK positive.
  if (taMACD_slow < taMACD_fast) std::swap(taMACD_slow, taMACD_fast);
  if (taBBands_stdK <= 0.0) taBBands_stdK = 2.0;

  return true;
}

void Config::synthesizeIngressFromLegacy() {
  if (!ingress.empty()) return;

  Ingress fsEntry;
  fsEntry.kind = "market.feedserver";
  fsEntry.params["port"] = std::to_string(feedPort);
  ingress.push_back(std::move(fsEntry));

  bool synthesizedWS = false;

  if (!feedUrl.empty()) {
    Ingress wsEntry;
    wsEntry.kind = "market.wsclient";
    wsEntry.params["url"] = feedUrl;
    wsEntry.params["adapter"] = "itch";
    if (!feedSymbols.empty()) {
      std::string joined;
      for (size_t i = 0; i < feedSymbols.size(); ++i) {
        if (i) joined += ",";
        joined += feedSymbols[i];
      }
      wsEntry.params["symbols"] = joined;
    }
    ingress.push_back(std::move(wsEntry));
    synthesizedWS = true;
  }
  for (const auto& fc : feeds) {
    if (fc.url.empty()) continue;
    Ingress wsEntry;
    wsEntry.kind = "market.wsclient";
    wsEntry.params["url"] = fc.url;
    wsEntry.params["adapter"] = fc.adapter;
    if (!fc.symbols.empty()) {
      std::string joined;
      for (size_t i = 0; i < fc.symbols.size(); ++i) {
        if (i) joined += ",";
        joined += fc.symbols[i];
      }
      wsEntry.params["symbols"] = joined;
    }
    ingress.push_back(std::move(wsEntry));
    synthesizedWS = true;
  }
  if (synthesizedWS) {
    gma::util::logger().log(
        gma::util::LogLevel::Warn,
        "config.deprecated_keys",
        {{"keys", "feedUrl/feedSymbols/feeds.N.*"},
         {"replacement", "ingress.N.kind = market.wsclient + ingress.N.url|adapter|symbols"},
         {"window", "one release"}});
  }
}

std::size_t Config::dispatchPendingKeys() {
  std::size_t consumed = 0;
  for (const auto& [k, v] : _pendingKeys) {
    if (gma::engine::ConfigNamespaceRegistry::dispatch(k, v)) {
      ++consumed;
    }
  }
  _pendingKeys.clear();
  return consumed;
}

} // namespace util
} // namespace gma
