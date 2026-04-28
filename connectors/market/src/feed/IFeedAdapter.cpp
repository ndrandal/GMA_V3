#include "gma/feed/IFeedAdapter.hpp"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace gma::feed {

std::string IFeedAdapter::subscribeMessage(const std::vector<std::string>& symbols) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("action"); w.String("subscribe");
    w.Key("symbols");
    w.StartArray();
    for (const auto& s : symbols) w.String(s.c_str());
    w.EndArray();
    w.EndObject();
    return sb.GetString();
}

} // namespace gma::feed
