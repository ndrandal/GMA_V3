#include "gma/ws/WSResponder.hpp"
#include "gma/util/Logger.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <variant>
#include <vector>

namespace gma::ws {

void WSResponder::onValue(const SymbolValue& sv) {
  if (!send_) return;

  try {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartObject();
    w.Key("type");   w.String("update");
    w.Key("id");     w.String(reqId_.c_str());
    w.Key("symbol"); w.String(sv.symbol.c_str());
    w.Key("value");

    // Serialize ArgType variant to JSON.
    std::visit([&](auto&& x) {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<T, bool>) {
        w.Bool(x);
      } else if constexpr (std::is_same_v<T, int>) {
        w.Int(x);
      } else if constexpr (std::is_same_v<T, double>) {
        w.Double(x);
      } else if constexpr (std::is_same_v<T, std::string>) {
        w.String(x.c_str());
      } else if constexpr (std::is_same_v<T, std::vector<int>>) {
        w.StartArray();
        for (int n : x) w.Int(n);
        w.EndArray();
      } else if constexpr (std::is_same_v<T, std::vector<double>>) {
        w.StartArray();
        for (double d : x) w.Double(d);
        w.EndArray();
      } else {
        w.Null();
      }
    }, sv.value);

    w.EndObject();

    send_(sb.GetString());
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "WSResponder::onValue failed",
                            {{"err", ex.what()}, {"reqId", reqId_}});
  }
}

} // namespace gma::ws
