#include "gma/util/Logger.hpp"
#include <cstdio>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <map>
#include <cctype>
#include <chrono>

namespace gma { namespace util {

static thread_local std::map<std::string, std::string> t_ctx;

static const char* lvlStr(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "INFO";
}

LogLevel parseLevel(const std::string& s) {
  std::string x = s;
  for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (x == "trace") return LogLevel::Trace;
  if (x == "debug") return LogLevel::Debug;
  if (x == "info")  return LogLevel::Info;
  if (x == "warn")  return LogLevel::Warn;
  if (x == "error") return LogLevel::Error;
  return LogLevel::Info;
}

Logger& logger() {
  static Logger L;
  return L;
}

Logger::Logger() {}

Logger::~Logger() {
  std::lock_guard<std::mutex> lk(mx_);
  if (file_ && file_ != stdout) {
    std::fclose(static_cast<FILE*>(file_));
    file_ = nullptr;
  }
}

void Logger::setLevel(LogLevel lvl) {
  lvl_.store(lvl, std::memory_order_release);
}

void Logger::setFormatJson(bool json) {
  json_.store(json, std::memory_order_release);
}

void Logger::setFile(const std::string& path) {
  std::lock_guard<std::mutex> lk(mx_);
  if (file_ && file_ != stdout) std::fclose(static_cast<FILE*>(file_));
  file_ = path.empty() ? stdout : static_cast<void*>(std::fopen(path.c_str(), "a"));
  if (!file_) file_ = stdout;
}

LogLevel Logger::level() const {
  return lvl_.load(std::memory_order_acquire);
}

void Logger::log(LogLevel lvl, const std::string& msg, const std::vector<Field>& fields) {
  if (static_cast<int>(lvl) < static_cast<int>(lvl_.load(std::memory_order_acquire)))
    return;
  writeLine(lvl, msg, fields);
}

static std::string nowIso() {
  using namespace std::chrono;
  auto tp = system_clock::now();
  auto t = system_clock::to_time_t(tp);
  auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
  std::tm tm;
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
      << '.' << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

void Logger::escapeJson(std::ostringstream& oss, const std::string& s) {
  for (char c : s) {
    switch (c) {
      case '"':  oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\n': oss << "\\n";  break;
      case '\r': oss << "\\r";  break;
      case '\t': oss << "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Control character — escape as \u00XX
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          oss << buf;
        } else {
          oss << c;
        }
        break;
    }
  }
}

void Logger::writeLine(LogLevel lvl, const std::string& msg, const std::vector<Field>& fields) {
  std::lock_guard<std::mutex> lk(mx_);
  FILE* f = file_ ? static_cast<FILE*>(file_) : stdout;

  if (json_.load(std::memory_order_acquire)) {
    std::ostringstream oss;
    oss << "{\"ts\":\"" << nowIso() << "\",\"lvl\":\"" << lvlStr(lvl) << "\",\"msg\":\"";
    escapeJson(oss, msg);
    oss << "\"";

    for (auto& kv : t_ctx) {
      oss << ",\"";
      escapeJson(oss, kv.first);
      oss << "\":\"";
      escapeJson(oss, kv.second);
      oss << "\"";
    }
    for (auto& kv : fields) {
      oss << ",\"";
      escapeJson(oss, kv.k);
      oss << "\":\"";
      escapeJson(oss, kv.v);
      oss << "\"";
    }
    oss << "}\n";
    auto str = oss.str();
    std::fwrite(str.c_str(), 1, str.size(), f);
  } else {
    std::fprintf(f, "[%s] %-5s %s", nowIso().c_str(), lvlStr(lvl), msg.c_str());
    for (auto& kv : t_ctx) std::fprintf(f, " %s=%s", kv.first.c_str(), kv.second.c_str());
    for (auto& kv : fields) std::fprintf(f, " %s=%s", kv.k.c_str(), kv.v.c_str());
    std::fputc('\n', f);
  }
  std::fflush(f);
}

Logger::Scoped::Scoped(const std::vector<Field>& add) {
  for (auto& kv : add) {
    addedKeys_.push_back(kv.k);
    t_ctx[kv.k] = kv.v;
  }
}

Logger::Scoped::~Scoped() {
  for (auto& k : addedKeys_) {
    t_ctx.erase(k);
  }
}

}} // namespace gma::util
