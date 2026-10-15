//
// ClickHouseHttp — see ClickHouseHttp.hpp.
//

#include "gma/data/ClickHouseHttp.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <charconv>
#include <cstdlib>
#include <sstream>

namespace gma::data::ch {
namespace {

namespace beast = boost::beast;
namespace http  = beast::http;
using tcp       = boost::asio::ip::tcp;

const char* envOrNull(const char* name) {
  const char* v = std::getenv(name);
  return (v != nullptr && *v != '\0') ? v : nullptr;
}

// The settings pinned on every request. See the header for why
// `date_time_input_format` is one of them.
constexpr std::string_view kPinnedSettings = "?date_time_input_format=basic";

}  // namespace

arrow::Result<Target> resolveTarget(const std::optional<std::string>& explicitUrl) {
  Target t;
  if (const char* off = envOrNull("STACK_PORT_OFFSET")) {
    std::int64_t n = 0;
    const auto r = std::from_chars(off, off + std::strlen(off), n);
    if (r.ec != std::errc{} || r.ptr != off + std::strlen(off) || n < 0) {
      return arrow::Status::Invalid("STACK_PORT_OFFSET must be a non-negative "
                                    "integer (got \"", off, "\")");
    }
    t.stackOffset  = n;
    t.offsetWasSet = true;
  }

  if (explicitUrl.has_value() && !explicitUrl->empty()) {
    t.url         = *explicitUrl;
    t.how         = "--url";
    t.explicitUrl = true;
    return t;
  }
  if (const char* u = envOrNull("CH_HTTP_URL")) {
    t.url         = u;
    t.how         = "CH_HTTP_URL";
    t.explicitUrl = true;
    return t;
  }
  if (const char* p = envOrNull("CH_HTTP_PORT")) {
    std::int64_t n = 0;
    const auto r = std::from_chars(p, p + std::strlen(p), n);
    if (r.ec != std::errc{} || r.ptr != p + std::strlen(p) || n <= 0 || n > 65535) {
      return arrow::Status::Invalid("CH_HTTP_PORT must be a port number (got \"",
                                    p, "\")");
    }
    t.url         = "http://127.0.0.1:" + std::to_string(n) + "/";
    t.how         = "CH_HTTP_PORT";
    t.explicitUrl = true;
    return t;
  }
  const std::int64_t port = 8123 + t.stackOffset;
  t.url = "http://127.0.0.1:" + std::to_string(port) + "/";
  t.how = t.offsetWasSet
              ? "8123 + STACK_PORT_OFFSET(" + std::to_string(t.stackOffset) + ")"
              : "default 8123";
  t.explicitUrl = false;
  return t;
}

Client::Client(Target target) : target_(std::move(target)) {
  // Deliberately a small hand parse rather than a URL library: the only form
  // this accepts is http://host:port/ and anything else should fail loudly on
  // connect rather than be normalized into something that works.
  std::string rest = target_.url;
  const std::string scheme = "http://";
  if (rest.rfind(scheme, 0) == 0) rest = rest.substr(scheme.size());
  const auto slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  path_ = slash == std::string::npos ? "/" : rest.substr(slash);
  if (path_.empty()) path_ = "/";
  const auto colon = authority.find(':');
  if (colon == std::string::npos) {
    host_ = authority;
    port_ = "8123";
  } else {
    host_ = authority.substr(0, colon);
    port_ = authority.substr(colon + 1);
  }
  // Append the pinned settings. If the caller's URL already had a query
  // string, join with '&' instead of starting a second one.
  path_ += path_.find('?') == std::string::npos
               ? std::string(kPinnedSettings)
               : "&" + std::string(kPinnedSettings.substr(1));
}

arrow::Result<std::string> Client::exec(std::string_view sql) const {
  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);
    const auto endpoints = resolver.resolve(host_, port_);
    stream.connect(endpoints);

    http::request<http::string_body> req{http::verb::post, path_, 11};
    req.set(http::field::host, host_ + ":" + port_);
    req.set(http::field::user_agent, "gma_ingest/ENC-1483");
    req.body() = std::string(sql);
    req.prepare_payload();
    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    const auto status = static_cast<unsigned>(res.result_int());
    if (status < 200 || status >= 300) {
      // The server's own message is the only diagnostic worth having; a bare
      // "HTTP 400" sends the reader to the wrong place every time.
      std::string head(sql.substr(0, std::min<std::size_t>(sql.size(), 400)));
      return arrow::Status::IOError("ClickHouse at ", target_.url, " returned HTTP ",
                                    status, ":\n", res.body(), "\n--- statement ---\n",
                                    head);
    }
    return res.body();
  } catch (const std::exception& e) {
    return arrow::Status::IOError(
        "cannot reach ClickHouse at ", target_.url, " (", target_.how, "): ", e.what(),
        ".\nThis is a COULD-NOT-RUN, not a finding: nothing was read and "
        "nothing was written. Start the server (tools/stack.sh up clickhouse) or "
        "point --url / CH_HTTP_URL at the right one.");
  }
}

arrow::Result<std::vector<std::vector<std::string>>> Client::select(
    std::string_view sql) const {
  ARROW_ASSIGN_OR_RAISE(const std::string body, exec(sql));
  std::vector<std::vector<std::string>> rows;
  std::size_t start = 0;
  while (start < body.size()) {
    const auto nl = body.find('\n', start);
    const auto end = nl == std::string::npos ? body.size() : nl;
    const std::string_view line(body.data() + start, end - start);
    if (!line.empty()) {
      std::vector<std::string> fields;
      std::size_t fs = 0;
      for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == '\t') {
          fields.emplace_back(line.substr(fs, i - fs));
          fs = i + 1;
        }
      }
      rows.push_back(std::move(fields));
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return rows;
}

arrow::Result<std::string> Client::scalar(std::string_view sql) const {
  ARROW_ASSIGN_OR_RAISE(auto rows, select(sql));
  if (rows.size() != 1 || rows[0].size() != 1) {
    return arrow::Status::Invalid("expected exactly one field from:\n", sql);
  }
  return rows[0][0];
}

std::string tsvEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

}  // namespace gma::data::ch
