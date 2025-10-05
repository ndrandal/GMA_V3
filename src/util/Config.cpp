#include "gma/util/Config.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>

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

  std::string buf;
  buf.resize(8 * 1024);

  std::string line;
  line.reserve(1024);

  bool ok = true;

  // Read whole file
  while (true) {
    char tmp[1024];
    if (!std::fgets(tmp, sizeof(tmp), f)) break;
    line.assign(tmp);

    // Strip CR/LF
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    auto s = trim(line);
    if (s.empty()) continue;
    if (s[0] == '#' || s[0] == ';') continue; // comment

    std::string key, val;
    if (!parseLineKV(s, key, val)) continue;

    if      (key == "taMACD_fast")    taMACD_fast    = std::max(1, std::atoi(val.c_str()));
    else if (key == "taMACD_slow")    taMACD_slow    = std::max(1, std::atoi(val.c_str()));
    else if (key == "taBBands_n")     taBBands_n     = std::max(1, std::atoi(val.c_str()));
    else if (key == "taBBands_stdK")  taBBands_stdK  = std::atof(val.c_str());
    else {
      // Unknown key; ignore to stay forward-compatible
    }
  }

  std::fclose(f);

  // Basic sanity: slow >= fast for MACD; stdK positive.
  if (taMACD_slow < taMACD_fast) std::swap(taMACD_slow, taMACD_fast);
  if (taBBands_stdK <= 0.0) taBBands_stdK = 2.0;

  return ok;
}

} // namespace util
} // namespace gma
