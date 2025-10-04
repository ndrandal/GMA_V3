#include "gma/util/Config.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <cstdio>

// RapidJSON
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace gma::util {

static Config gCfg;

Config& Config::get() { return gCfg; }

std::optional<std::string> Config::env(const char* name) {
  const char* v = std::getenv(name);
  if (!v) return std::nullopt;
  return std::string(v);
}

int Config::envInt(const char* name, int def) {
  if (auto v = env(name)) {
    try {
      return std::stoi(*v);
    } catch (...) {
      return def;
    }
  }
  return def;
}

void Config::loadFromEnv(Config& out) {
  out.wsPort           = envInt("GMA_WS_PORT", out.wsPort);
  out.threadPoolSize   = envInt("GMA_THREADS", out.threadPoolSize);
  out.listenerQueueCap = envInt("GMA_LISTENER_Q", out.listenerQueueCap);

  if (auto v = env("GMA_LOG_LEVEL"))  out.logLevel  = *v;
  if (auto v = env("GMA_LOG_FORMAT")) out.logFormat = *v;
  if (auto v = env("GMA_LOG_FILE"))   out.logFile   = *v;

  out.metricsEnabled     = envInt("GMA_METRICS_ON", out.metricsEnabled?1:0) != 0;
  out.metricsIntervalSec = envInt("GMA_METRICS_EVERY", out.metricsIntervalSec);

  out.taHistoryMax       = envInt("GMA_TA_HISTORY_MAX", out.taHistoryMax);
}

static bool asBool(const rapidjson::Value& v, bool def=false){
  if (v.IsBool()) return v.GetBool();
  if (v.IsInt()) return v.GetInt()!=0;
  if (v.IsString()) {
    std::string s = v.GetString();
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return (s=="1" || s=="true" || s=="yes" || s=="on");
  }
  return def;
}

static int asInt(const rapidjson::Value& v, int def=0){
  if (v.IsInt()) return v.GetInt();
  if (v.IsUint()) return static_cast<int>(v.GetUint());
  if (v.IsInt64()) return static_cast<int>(v.GetInt64());
  if (v.IsDouble()) return static_cast<int>(v.GetDouble());
  return def;
}

static std::vector<int> asIntArray(const rapidjson::Value& v){
  std::vector<int> out;
  if (!v.IsArray()) return out;
  for (auto& x : v.GetArray()) {
    if (x.IsInt()) out.push_back(x.GetInt());
  }
  return out;
}

bool Config::loadFromFile(Config& out, const std::string& path, std::string* err) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) { if (err) *err = "cannot open file"; return false; }
  std::string data;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
  std::fclose(f);

  rapidjson::Document d;
  d.Parse(data.c_str());
  if (d.HasParseError()) {
    if (err) *err = std::string("json parse error: ") + rapidjson::GetParseError_En(d.GetParseError());
    return false;
  }
  if (!d.IsObject()) { if (err) *err = "root not an object"; return false; }

  if (auto it = d.FindMember("wsPort"); it != d.MemberEnd()) out.wsPort = asInt(it->value, out.wsPort);
  if (auto it = d.FindMember("threadPoolSize"); it != d.MemberEnd()) out.threadPoolSize = asInt(it->value, out.threadPoolSize);
  if (auto it = d.FindMember("listenerQueueCap"); it != d.MemberEnd()) out.listenerQueueCap = asInt(it->value, out.listenerQueueCap);

  if (auto it = d.FindMember("logLevel"); it != d.MemberEnd() && it->value.IsString()) out.logLevel = it->value.GetString();
  if (auto it = d.FindMember("logFormat"); it != d.MemberEnd() && it->value.IsString()) out.logFormat = it->value.GetString();
  if (auto it = d.FindMember("logFile"); it != d.MemberEnd() && it->value.IsString()) out.logFile = it->value.GetString();

  if (auto it = d.FindMember("metricsEnabled"); it != d.MemberEnd()) out.metricsEnabled = asBool(it->value, out.metricsEnabled);
  if (auto it = d.FindMember("metricsIntervalSec"); it != d.MemberEnd()) out.metricsIntervalSec = asInt(it->value, out.metricsIntervalSec);

  if (auto it = d.FindMember("taHistoryMax"); it != d.MemberEnd()) out.taHistoryMax = asInt(it->value, out.taHistoryMax);
  if (auto it = d.FindMember("taSMA"); it != d.MemberEnd()) out.taSMA = asIntArray(it->value);
  if (auto it = d.FindMember("taEMA"); it != d.MemberEnd()) out.taEMA = asIntArray(it->value);
  if (auto it = d.FindMember("taRSI"); it != d.MemberEnd()) out.taRSI = asInt(it->value, out.taRSI);
  if (auto it = d.FindMember("taMACD"); it != d.MemberEnd() && it->value.IsArray() && it->value.Size()>=2) {
    out.taMACD_fast = asInt(it->value[0], out.taMACD_fast);
    out.taMACD_slow = asInt(it->value[1], out.taMACD_slow);
  }
  if (auto it = d.FindMember("taBBands"); it != d.MemberEnd() && it->value.IsArray() && it->value.Size()>=2) {
    out.taBBands_n    = asInt(it->value[0], out.taBBands_n);
    out.taBBands_stdK = asInt(it->value[1], out.taBBands_stdK);
  }

  return true;
}

} // namespace gma::util
