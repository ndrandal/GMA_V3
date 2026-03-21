#include "gma/ws/WSResponder.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/JsonUtil.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <variant>
#include <vector>

namespace gma::ws {

void WSResponder::onValue(const SymbolValue& sv) {
  if (stopped_.load(std::memory_order_acquire)) return;

  // Copy send_ under lock to avoid TOCTOU race with shutdown().
  SendFn fn;
  {
    std::lock_guard<std::mutex> lk(mx_);
    fn = send_;
  }
  if (!fn) return;

  try {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartObject();
    w.Key("type");   w.String("update");
    w.Key("id");     w.String(reqId_.c_str());
    w.Key("symbol"); w.String(sv.symbol.c_str());
    w.Key("value");
    gma::util::writeArgTypeJson(w, sv.value);

    w.EndObject();

    fn(sb.GetString());
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "WSResponder::onValue failed",
                            {{"err", ex.what()}, {"reqId", reqId_}});
  }
}

void WSResponder::shutdown() noexcept {
  stopped_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  send_ = nullptr;
}

} // namespace gma::ws
