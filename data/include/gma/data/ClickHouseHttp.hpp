#pragma once
//
// A minimal ClickHouse HTTP client for the ingest job (ENC-1483).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D2, D11, D14;
//         docs/data-layer.md §5.2, §8.8.
//
// WHY HTTP AND WHY HAND-ROLLED
// ----------------------------
// `clickhouse-cpp` (the native-protocol client) is the right dependency for
// B1's transport benchmark, which is ENC-1485's job and whose entire question
// is native-vs-HTTP. It is the wrong dependency for the INGEST job, which
// moves 2000 records once and whose cost is dominated by a `DROP PARTITION`.
// Pulling a client library in here would pre-answer ENC-1485's measurement by
// making one of its two arms the only one in the build.
//
// What this needs is a POST with a body and a status code, and Boost.Beast is
// already in this build (gma_engine exposes Boost's include dirs and
// Boost::system PUBLIC for the WebSocket server), so the HTTP half costs
// nothing new. SPEC D14's "Arrow and the ClickHouse client must never become
// engine dependencies" is unaffected: this header lives in `gma_data`, and
// `tools/check_engine_isolation.py` check A refuses `clickhouse/` includes in
// engine code, which this is not.
//
// THE STATEMENT AND THE DATA GO IN ONE BODY, not a `?query=` parameter.
// Measured against the running server: `INSERT INTO t (cols) FORMAT
// TabSeparated\n<rows>` as the whole POST body is accepted, which means no
// percent-encoder has to be written and no 8 KB URL limit applies to a
// statement naming sixteen columns.
//
// `date_time_input_format=basic` IS PINNED ON EVERY REQUEST. `DateTime64(9)`
// accepts a bare integer as a raw tick count under `basic` — measured:
// 1654864201100846675 round-trips through `toUnixTimestamp64Nano` exactly — and
// that is the only form in which a nanosecond timestamp reaches the server
// without a decimal string in between. Under `best_effort` the same digits are
// a candidate for several other interpretations. Pinning it per-request means
// a server-side default change cannot silently reinterpret every timestamp in
// L1.
//

#include <arrow/result.h>
#include <arrow/status.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gma::data::ch {

// ---------------------------------------------------------------------------
// WHICH SERVER — the same precedence `data/schema/apply.py` resolves, and for
// the same reason, which is safety rather than configuration.
//
//     explicit url  >  CH_HTTP_URL  >  CH_HTTP_PORT  >  8123 + STACK_PORT_OFFSET
//
// The last term is how `tools/stack.sh` derives `CH_HTTP_PORT`, so a worktree
// running at STACK_PORT_OFFSET=160 resolves to 8283 and reaches its OWN server
// rather than the offset-zero store. apply.py's docstring records what the
// hardcoded `http://127.0.0.1:8123/` cost: `apply --fresh` from an offset
// worktree dropped somebody else's `gma` database. This job writes rows and
// drops partitions, so it resolves the target the same way and PRINTS the
// resolution.
// ---------------------------------------------------------------------------
struct Target {
  std::string url;
  std::string how;              // human-readable provenance of `url`
  std::int64_t stackOffset = 0;  // STACK_PORT_OFFSET, or 0 if unset
  bool offsetWasSet = false;
  bool explicitUrl  = false;
};

arrow::Result<Target> resolveTarget(const std::optional<std::string>& explicitUrl);

// ---------------------------------------------------------------------------
// The client. One short-lived connection per request: this job issues a few
// dozen, and a pool would be a cache with an invalidation story for no gain.
// ---------------------------------------------------------------------------
class Client {
 public:
  explicit Client(Target target);

  // Runs `sql` and returns the response body verbatim. A non-2xx status is an
  // error carrying the server's own message, which is the only useful thing in
  // a ClickHouse failure.
  arrow::Result<std::string> exec(std::string_view sql) const;

  // `exec`, then split the body into rows of fields on '\n' and '\t'. For
  // `FORMAT TabSeparated` reads. A trailing newline yields no empty row.
  arrow::Result<std::vector<std::vector<std::string>>> select(
      std::string_view sql) const;

  // `exec`, then require the body to be exactly one field.
  arrow::Result<std::string> scalar(std::string_view sql) const;

  const Target& target() const noexcept { return target_; }

 private:
  Target target_;
  std::string host_;
  std::string port_;
  std::string path_;  // includes the pinned settings query string
};

// TabSeparated escaping, for the string columns (`symbol`, `venue`, `kind`,
// `source`, `source_ref`). Every other column this job writes is an integer.
// Applied rather than skipped because "none of our values contain a tab" is
// luck, not an invariant, and a tab inside a `source_ref` would shift every
// subsequent column of that row by one.
std::string tsvEscape(std::string_view s);

// A SINGLE-QUOTED SQL STRING LITERAL, escaped — `sqlQuote("a'b")` is `'a\'b'`.
//
// Code-review finding #5, and the argument is the one `tsvEscape` already
// makes one paragraph up, which makes its absence here the worse omission:
// every `symbol`, `kind`, `source_ref`, `dataset` and `raw_symbol` was
// concatenated straight between two quote characters. **These values are not
// only CLI arguments** — `raw_symbol` and `dataset` come out of the vendor
// DBN's symbology section and out of `metadata.json`, i.e. out of a file this
// process did not write. And the CLI half was directly exploitable:
// `verify --symbol "x' OR 1=1 --"` made the completeness read answer over
// every symbol in the table, which in a tool whose entire job is to refuse a
// query it cannot vouch for is the worst possible shape of bug.
//
// Escapes backslash and single-quote, which is what ClickHouse's string-literal
// grammar needs. It does NOT attempt to validate that the value is a plausible
// ticker: escaping is a property of the encoding and is always correct, while a
// whitelist is a guess about the vendor's symbology that would reject a legal
// symbol one day.
std::string sqlQuote(std::string_view s);

}  // namespace gma::data::ch
