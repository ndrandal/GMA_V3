#pragma once

#include <string>
#include <vector>
#include <mutex>

namespace gma {
namespace util {

enum class LogLevel : int {
  Trace = 0,
  Debug = 1,
  Info  = 2,
  Warn  = 3,
  Error = 4
};

struct Field {
  std::string k;
  std::string v;
};

LogLevel parseLevel(const std::string& s);

class Logger {
public:
  Logger();

  void setLevel(LogLevel lvl);
  void setFormatJson(bool json);
  void setFile(const std::string& path); // empty -> stdout

  LogLevel level() const;

  void log(LogLevel lvl, const std::string& msg, const std::vector<Field>& fields = {});

  // Thread-local context helper
  class Scoped {
  public:
    explicit Scoped(const std::vector<Field>& add);
    ~Scoped();
  };

private:
  void writeLine(LogLevel lvl, const std::string& msg, const std::vector<Field>& fields);

private:
  mutable std::mutex mx_;
  void* file_ = nullptr;            // FILE* stored as void* to avoid <cstdio> in header
  LogLevel lvl_ = LogLevel::Info;
  bool json_ = false;
};

Logger& logger();

} // namespace util
} // namespace gma
