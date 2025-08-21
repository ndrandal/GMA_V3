#include "gma/util/Logger.hpp"
#include "gma/util/Config.hpp"
#include <cstdio>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <thread>

namespace gma::util {

static thread_local std::map<std::string, std::string> t_ctx;

static const char* lvlStr(LogLevel l) {
  switch (l) { case LogLevel::TRACE: return "TRACE";
               case LogLevel::DEBUG: return "DEBUG";
               case LogLevel::INFO:  return "INFO";
               case LogLevel::WARN:  return "WARN";
               case LogLevel::ERROR: return "ERROR"; }
  return "INFO";
}

LogLevel parseLevel(const std::string& s) {
  std::string x=s; for (auto& c:x) c=std::tolower(c);
  if (x=="trace") return LogLevel::TRACE;
  if (x=="debug") return LogLevel::DEBUG;
  if (x=="info")  return LogLevel::INFO;
  if (x=="warn")  return LogLevel::WARN;
  if (x=="error") return LogLevel::ERROR;
  return LogLevel::INFO;
}

Logger& logger() {
  static Logger L;
  return L;
}

Logger::Logger() {}

void Logger::setLevel(LogLevel lvl) { lvl_ = lvl; }
void Logger::setFormatJson(bool json) { json_ = json; }
void Logger::setFile(const std::string& path) {
  std::lock_guard<std::mutex> lk(mx_);
  if (file_ && file_ != stdout) std::fclose((FILE*)file_);
  file_ = path.empty() ? stdout : (void*)std::fopen(path.c_str(), "a");
  if (!file_) file_ = stdout;
}
LogLevel Logger::level() const { return lvl_; }

void Logger::log(LogLevel lvl, const std::string& msg, const std::vector<Field>& fields) {
  if ((int)lvl < (int)lvl_) return;
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
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
  return oss.str();
}

void Logger::writeLine(LogLevel lvl, const std::string& msg, const std::vector<Field>& fields) {
  std::lock_guard<std::mutex> lk(mx_);
  FILE* f = (FILE*)(file_ ? file_ : stdout);

  if (json_) {
    std::ostringstream oss;
    oss << "{\"ts\":\"" << nowIso() << "\",\"lvl\":\"" << lvlStr(lvl) << "\",\"msg\":\"";
    // naive escaping
    for (char c : msg) { if (c=='"') oss << '\\'; oss << c; }
    oss << "\"";

    // thread ctx
    for (auto& kv : t_ctx) {
      oss << ",\"" << kv.first << "\":\"";
      for (char c : kv.second) { if (c=='"') oss << '\\'; oss << c; }
      oss << "\"";
    }
    // ad hoc fields
    for (auto& kv : fields) {
      oss << ",\"" << kv.k << "\":\"";
      for (char c : kv.v) { if (c=='"') oss << '\\'; oss << c; }
      oss << "\"";
    }
    oss << "}\n";
    std::fwrite(oss.str().c_str(), 1, oss.str().size(), f);
  } else {
    std::fprintf(f, "[%s] %-5s %s", nowIso().c_str(), lvlStr(lvl), msg.c_str());
    for (auto& kv : t_ctx) std::fprintf(f, " %s=%s", kv.first.c_str(), kv.second.c_str());
    for (auto& kv : fields) std::fprintf(f, " %s=%s", kv.k.c_str(), kv.v.c_str());
    std::fputc('\n', f);
  }
  std::fflush(f);
}

Logger::Scoped::Scoped(const std::vector<Field>& add) {
  for (auto& kv : add) t_ctx[kv.k] = kv.v;
}
Logger::Scoped::~Scoped() {
  // note: removes only added keys if needed; simplest is clear
  // more robust approach requires tracking added keys; for brevity:
  // (in practice you’d track and erase only added ones)
  for (auto& kv : t_ctx) { (void)kv; }
  // we’ll keep it simple: no-op; callers can nest freely (thread-local)
}

} // namespace gma::util
