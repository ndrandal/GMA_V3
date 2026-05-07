#include "gma/forum/ConnectorsClient.hpp"

#include "gma/util/Logger.hpp"

#include <chrono>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#ifdef GMA_HAS_SSL
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#endif

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace gma {
namespace forum {

namespace {

// ParsedURL: the bits of a forumUrl that the HTTP request needs.
struct ParsedURL {
  bool tls = false;
  std::string host;
  std::string port;
  std::string target = "/";
};

ParsedURL parseUrl(const std::string& url) {
  // Loose-but-sufficient regex; we only ever feed this user-controlled
  // INI strings, not arbitrary input. Path and query land in `target`.
  static const std::regex re(R"(^(http|https)://([^/:?]+)(?::(\d+))?(/[^?]*)?(\?.*)?$)",
                             std::regex::ECMAScript);
  std::smatch m;
  if (!std::regex_match(url, m, re)) {
    throw std::runtime_error("ConnectorsClient: malformed forumUrl: " + url);
  }
  ParsedURL out;
  out.tls = (m[1].str() == "https");
  out.host = m[2].str();
  if (m[3].matched && !m[3].str().empty()) {
    out.port = m[3].str();
  } else {
    out.port = out.tls ? "443" : "80";
  }
  std::string path = m[4].matched ? m[4].str() : std::string{"/"};
  if (path.empty()) path = "/";
  out.target = path;
  if (m[5].matched) out.target += m[5].str();
  return out;
}

// Build the HTTP GET request shared by both legs (plain + TLS).
http::request<http::empty_body>
buildRequest(const ParsedURL& url,
             const std::string& tenantId,
             const std::string& bearer) {
  std::string apiTarget = url.target;
  if (!apiTarget.empty() && apiTarget.back() == '/') {
    apiTarget.pop_back();
  }
  apiTarget += "/api/connectors";

  http::request<http::empty_body> req{http::verb::get, apiTarget, 11};
  req.set(http::field::host, url.host + (url.port == "80" || url.port == "443" ? "" : ":" + url.port));
  req.set(http::field::user_agent, std::string("gma_v3/") + BOOST_BEAST_VERSION_STRING);
  req.set(http::field::accept, "application/json");
  if (!tenantId.empty()) {
    req.set("X-Tenant-Id", tenantId);
  }
  if (!bearer.empty()) {
    req.set(http::field::authorization, "Bearer " + bearer);
  }
  return req;
}

// Synchronous plain-HTTP GET → response body.
std::string fetchPlain(const ParsedURL& url,
                       const std::string& tenantId,
                       const std::string& bearer) {
  asio::io_context ioc;
  tcp::resolver resolver{ioc};
  beast::tcp_stream stream{ioc};

  auto endpoints = resolver.resolve(url.host, url.port);
  stream.expires_after(std::chrono::seconds(5));
  stream.connect(endpoints);

  auto req = buildRequest(url, tenantId, bearer);
  http::write(stream, req);

  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  beast::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_both, ec);

  if (res.result() != http::status::ok) {
    throw std::runtime_error(
        "ConnectorsClient: HTTP " + std::to_string(static_cast<int>(res.result())) +
        " from forum: " + std::string(res.body()).substr(0, 200));
  }
  return std::string(res.body());
}

#ifdef GMA_HAS_SSL
// Synchronous TLS-HTTPS GET → response body.
std::string fetchTls(const ParsedURL& url,
                     const std::string& tenantId,
                     const std::string& bearer) {
  asio::io_context ioc;
  asio::ssl::context ctx{asio::ssl::context::tls_client};
  ctx.set_default_verify_paths();
  ctx.set_verify_mode(asio::ssl::verify_peer);

  tcp::resolver resolver{ioc};
  beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

  if (!SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str())) {
    beast::error_code ec{static_cast<int>(::ERR_get_error()), asio::error::get_ssl_category()};
    throw std::runtime_error("ConnectorsClient: SNI failure: " + ec.message());
  }

  auto endpoints = resolver.resolve(url.host, url.port);
  beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(10));
  beast::get_lowest_layer(stream).connect(endpoints);
  stream.handshake(asio::ssl::stream_base::client);

  auto req = buildRequest(url, tenantId, bearer);
  http::write(stream, req);

  beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  beast::error_code ec;
  stream.shutdown(ec);
  // SSL_R_SHORT_READ is a benign close per Beast docs.

  if (res.result() != http::status::ok) {
    throw std::runtime_error(
        "ConnectorsClient: HTTP " + std::to_string(static_cast<int>(res.result())) +
        " from forum: " + std::string(res.body()).substr(0, 200));
  }
  return std::string(res.body());
}
#endif

// Best-effort retry around a single fetch lambda. Total budget ~14s
// across 3 attempts (2s, 4s, 8s) — matches the spec's "small retry
// loop" risk mitigation.
template <typename F>
std::string fetchWithRetry(F&& fetch) {
  std::string lastErr;
  for (int attempt = 0; attempt < 3; ++attempt) {
    try {
      return fetch();
    } catch (const std::exception& ex) {
      lastErr = ex.what();
      gma::util::logger().log(
          gma::util::LogLevel::Warn,
          "forum.connectors.fetch_retry",
          {{"attempt", std::to_string(attempt + 1)}, {"err", lastErr}});
      std::this_thread::sleep_for(std::chrono::seconds(1 << (attempt + 1)));
    }
  }
  throw std::runtime_error("ConnectorsClient: exhausted retries: " + lastErr);
}

} // namespace

std::vector<gma::util::Config::Ingress>
ConnectorsClient::parseConnectorsJson(const std::string& body) {
  rapidjson::Document doc;
  if (doc.Parse(body.c_str()).HasParseError()) {
    throw std::runtime_error(
        std::string("ConnectorsClient: JSON parse error: ") +
        rapidjson::GetParseError_En(doc.GetParseError()));
  }
  if (!doc.IsArray()) {
    throw std::runtime_error("ConnectorsClient: expected JSON array at top level");
  }

  std::vector<gma::util::Config::Ingress> out;
  out.reserve(doc.Size());

  for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
    const auto& row = doc[i];
    if (!row.IsObject()) continue;

    const std::string name = (row.HasMember("name") && row["name"].IsString())
                                 ? row["name"].GetString() : std::string{};
    const std::string protocol = (row.HasMember("protocol") && row["protocol"].IsString())
                                     ? row["protocol"].GetString() : std::string{};
    const std::string endpoint = (row.HasMember("endpointUrl") && row["endpointUrl"].IsString())
                                     ? row["endpointUrl"].GetString() : std::string{};

    if (protocol.empty() || endpoint.empty()) {
      gma::util::logger().log(
          gma::util::LogLevel::Warn,
          "forum.connectors.skipped",
          {{"name", name}, {"reason", "missing protocol or endpoint_url"}});
      continue;
    }

    if (protocol != "itch") {
      gma::util::logger().log(
          gma::util::LogLevel::Warn,
          "forum.connectors.unknown_protocol",
          {{"name", name}, {"protocol", protocol}});
      continue;
    }

    // Symbols → comma-joined string param (matches the existing
    // gma.conf shape `ingress.1.symbols = NEXO,VALT,...` consumed
    // by IngressFactoryRegistry's market.wsclient.
    std::string symbolsCsv;
    if (row.HasMember("symbols") && row["symbols"].IsArray()) {
      const auto& syms = row["symbols"];
      for (rapidjson::SizeType s = 0; s < syms.Size(); ++s) {
        if (!syms[s].IsString()) continue;
        if (!symbolsCsv.empty()) symbolsCsv += ",";
        symbolsCsv += syms[s].GetString();
      }
    }
    if (symbolsCsv.empty()) {
      symbolsCsv = "*";  // wildcard — same default as Config::FeedConfig
    }

    gma::util::Config::Ingress ing;
    ing.kind = "market.wsclient";
    ing.params["url"] = endpoint;
    ing.params["adapter"] = protocol;  // "itch"
    ing.params["symbols"] = symbolsCsv;
    out.push_back(std::move(ing));

    gma::util::logger().log(
        gma::util::LogLevel::Info,
        "forum.connectors.mapped",
        {{"name", name}, {"endpoint", endpoint}, {"symbols", symbolsCsv}});
  }

  return out;
}

std::vector<gma::util::Config::Ingress>
ConnectorsClient::fetchIngresses(const std::string& forumUrl,
                                 const std::string& tenantId,
                                 const std::string& bearer) {
  ParsedURL url;
  try {
    url = parseUrl(forumUrl);
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string{"ConnectorsClient: bad URL: "} + ex.what());
  }

  std::string body = fetchWithRetry([&] {
    if (url.tls) {
#ifdef GMA_HAS_SSL
      return fetchTls(url, tenantId, bearer);
#else
      throw std::runtime_error(
          "ConnectorsClient: https forumUrl but binary built without GMA_HAS_SSL");
#endif
    }
    return fetchPlain(url, tenantId, bearer);
  });

  auto ingresses = parseConnectorsJson(body);
  gma::util::logger().log(
      gma::util::LogLevel::Info,
      "forum.connectors.fetched",
      {{"count", std::to_string(ingresses.size())}, {"url", forumUrl}});
  return ingresses;
}

} // namespace forum
} // namespace gma
