#pragma once

#include <string>
#include <vector>

#include "gma/util/Config.hpp"

namespace gma {
namespace forum {

/// Synchronous one-shot HTTP(S) GET against forum's /api/connectors,
/// translating each connector row into a Config::Ingress entry that
/// the engine's IngressRegistry can dispatch on.
///
/// Lives in the engine library (no connector dep) because the call
/// produces ingress configuration *before* connectors register their
/// factories — it's part of the composition root's startup sequence.
///
/// Exits the process on failure (per spec AC-6, fail-fast). Use the
/// parser helper directly in tests to assert the JSON-to-ingress
/// mapping without touching a network.
class ConnectorsClient {
public:
  /// Fetch connectors for `tenantId` from `forumUrl`'s `/api/connectors`
  /// endpoint and translate each `protocol == "itch"` entry into a
  /// `market.wsclient` ingress with `url` / `adapter` / `symbols`
  /// params filled from the row. Connectors with empty `endpoint_url`
  /// or unknown protocols are skipped (logged at Warn).
  ///
  /// `forumUrl` accepts `http://` and `https://` schemes; the path is
  /// always `/api/connectors`. `tenantId` becomes the `X-Tenant-Id`
  /// header. `bearer` becomes `Authorization: Bearer <bearer>` —
  /// pass empty to skip auth (dev only).
  ///
  /// Throws `std::runtime_error` on transport / parse / auth failure.
  /// Caller (main.cpp) is expected to log + exit.
  static std::vector<gma::util::Config::Ingress>
  fetchIngresses(const std::string& forumUrl,
                 const std::string& tenantId,
                 const std::string& bearer);

  /// Pure parser entry point — consumes a JSON body shaped like
  /// `[{name, endpoint_url, protocol, symbols, ...}]` and returns
  /// the corresponding ingress entries. Used by both `fetchIngresses`
  /// (after the network round-trip) and by `ConnectorsClientTest` so
  /// the JSON-to-ingress mapping is exercised without a network.
  static std::vector<gma::util::Config::Ingress>
  parseConnectorsJson(const std::string& body);
};

} // namespace forum
} // namespace gma
