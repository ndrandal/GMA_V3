#include "gma/util/Config.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>

// RapidJSON
#include <rapidjson/document.h>

namespace gma::util {

static Config gCfg;

Config& Config::get() { return gCfg; }

std::optional<std::string> Config::env(const char* name) {
  if (const char* v = std::getenv(name)) return std::string(v);
  return std::nullopt;
}

int Config::envInt(const char* name, int def) {
  if (const char* v = std::getenv(name)) {
    try { return std::max(0, std::stoi(v)); } catch (...) {}
  }
  return def;
}

static void parseIntArray(const rapidjson::Value& v, std::vector<int>& out) {
  if (!v.IsArray()) return;
  std::vector<int> tmp;
  for (auto& x : v.GetArray()) if (x.IsInt()) tmp.push_back(x.GetInt());
  if (!tmp.empty()) out.swap(tmp);
}

bool Config::loadFromFile(Config& out, const std::string& path, std::string* err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { if (err) *err = "cannot open file"; return false; }
  std::string s; char buf[4096];
  size_t n;
  while ((n = std::fread(buf,1,sizeof(buf),f))>0) s.append(buf,n);
  std::fclose(f);

  rapidjson::Document d;
  if (d.Parse(s.c_str()).HasParseError()) { if (err) *err = "bad JSON"; return false; }

  auto setInt = [&](const char* k, int& ref){ if (d.HasMember(k) && d[k].IsInt()) ref = d[k].GetInt(); };
  auto setStr = [&](const char* k, std::string& ref){ if (d.HasMember(k) && d[k].IsString()) ref = d[k].GetString(); };
  auto setBool= [&](const char* k, bool& ref){ if (d.HasMember(k) && d[k].IsBool()) ref = d[k].GetBool(); };

  setInt("wsPort", out.wsPort);
  setInt("threadPoolSize", out.threadPoolSize);
  setInt("listenerQueueCap", out.listenerQueueCap);

  setStr("logLevel", out.logLevel);
  setStr("logFormat", out.logFormat);
  setStr("logFile", out.logFile);

  setBool("metricsEnabled", out.metricsEnabled);
  setInt("metricsIntervalSec", out.metricsIntervalSec);

  setInt("taHistoryMax", out.taHistoryMax);
  if (d.HasMember("taSMA"))  parseIntArray(d["taSMA"],  out.taSMA);
  if (d.HasMember("taEMA"))  parseIntArray(d["taEMA"],  out.taEMA);
  if (d.HasMember("taVWAP")) parseIntArray(d["taVWAP"], out.taVWAP);
  if (d.HasMember("taMED"))  parseIntArray(d["taMED"],  out.taMED);
  if (d.HasMember("taMIN"))  parseIntArray(d["taMIN"],  out.taMIN);
  if (d.HasMember("taMAX"))  parseIntArray(d["taMAX"],  out.taMAX);
  if (d.HasMember("taSTD"))  parseIntArray(d["taSTD"],  out.taSTD);
  setInt("taRSI", out.taRSI);

  return true;
}

void Config::loadFromEnv(Config& out) {
  if (auto v = env("GMA_PORT")) out.wsPort = std::stoi(*v);
  out.threadPoolSize   = envInt("GMA_THREADS", out.threadPoolSize);
  out.listenerQueueCap = envInt("GMA_LISTENER_Q", out.listenerQueueCap);

  if (auto v = env("GMA_LOG_LEVEL"))  out.logLevel  = *v;
  if (auto v = env("GMA_LOG_FORMAT")) out.logFormat = *v;
  if (auto v = env("GMA_LOG_FILE"))   out.logFile   = *v;

  out.metricsEnabled     = envInt("GMA_METRICS_ON", out.metricsEnabled?1:0) != 0;
  out.metricsIntervalSec = envInt("GMA_METRICS_EVERY", out.metricsIntervalSec);

  out.taHistoryMax       = envInt("GMA_TA_HISTORY_MAX", out.taHistoryMax);
}

} // namespace gma::util
