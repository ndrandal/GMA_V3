#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <map>
#include <optional>

namespace gma::util {

enum class LogLevel { TRACE=0, DEBUG=1, INFO=2, WARN=3, ERROR=4 };

struct Field { std::string k; std::string v; };

class Logger {
public:
  Logger();

  void setLevel(LogLevel lvl);
  void setFormatJson(bool json);
  void setFile(const std::string& path); // "" => stdout
  LogLevel level() const;

  void log(LogLevel lvl, const std::string& msg,
           const std::vector<Field>& fields = {});

  // Thread-local scoped context (auto-popped on dtor)
  class Scoped {
  public:
    Scoped(const std::vector<Field>& add);
    ~Scoped();
  };

private:
  void writeLine(LogLevel lvl, const std::string& msg,
                 const std::vector<Field>& fields);

  LogLevel lvl_{LogLevel::INFO};
  bool json_{true};
  std::mutex mx_;
  void* file_{nullptr}; // FILE*
};

// Global logger
Logger& logger();

// Helpers
LogLevel parseLevel(const std::string& s);

} // namespace gma::util

// Convenience macros
#define GLOG_TRACE(msg, fields) ::gma::util::logger().log(::gma::util::LogLevel::TRACE, msg, fields)
#define GLOG_DEBUG(msg, fields) ::gma::util::logger().log(::gma::util::LogLevel::DEBUG, msg, fields)
#define GLOG_INFO(msg,  fields) ::gma::util::logger().log(::gma::util::LogLevel::INFO,  msg, fields)
#define GLOG_WARN(msg,  fields) ::gma::util::logger().log(::gma::util::LogLevel::WARN,  msg, fields)
#define GLOG_ERROR(msg, fields) ::gma::util::logger().log(::gma::util::LogLevel::ERROR, msg, fields)
