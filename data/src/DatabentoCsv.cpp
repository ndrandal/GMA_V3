//
// DatabentoCsv — see DatabentoCsv.hpp for why this stops at the vendor's PODs.
//

#include "gma/data/DatabentoCsv.hpp"

#include "gma/data/DbnEncoder.hpp"
#include "gma/data/Verdict.hpp"

#include <unistd.h>

#include <rapidjson/document.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace gma::data::csv {
namespace {

// ---------------------------------------------------------------------------
// Scalar parsing. `from_chars`, not `strtoull`: no locale, no errno, and a
// trailing non-digit is an ERROR rather than a silently shorter number — which
// matters because every field here is a price, a size or a nanosecond
// timestamp and a half-read one is a plausible wrong value.
// ---------------------------------------------------------------------------

template <typename T>
bool parseInt(std::string_view s, T& out) {
  if (s.empty()) return false;
  const char* first = s.data();
  const char* last  = s.data() + s.size();
  if constexpr (std::is_signed_v<T>) {
    // from_chars handles a leading '-' for signed types itself.
  } else {
    if (s.front() == '-') return false;
  }
  const auto r = std::from_chars(first, last, out);
  return r.ec == std::errc{} && r.ptr == last;
}

std::vector<std::string_view> splitLine(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == ',') {
      out.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

std::string_view trimCr(std::string_view s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}

// ---------------------------------------------------------------------------
// Header -> column index, by NAME. See the header comment: a positional reader
// built from the `trades` layout as described in the ticket would have read
// `depth` as `price`.
// ---------------------------------------------------------------------------
class Columns {
 public:
  arrow::Status parse(std::string_view headerLine,
                      const std::vector<std::string>& required,
                      const std::string& path) {
    const auto names = splitLine(trimCr(headerLine));
    for (std::size_t i = 0; i < names.size(); ++i) {
      index_[std::string(names[i])] = i;
    }
    width_ = names.size();
    std::vector<std::string> missing;
    for (const auto& r : required) {
      if (!index_.count(r)) missing.push_back(r);
    }
    if (!missing.empty()) {
      std::ostringstream os;
      os << "Databento CSV " << path << " is missing required column(s):";
      for (const auto& m : missing) os << ' ' << m;
      os << ". Columns present:";
      for (const auto& n : names) os << ' ' << n;
      os << ". Columns are resolved BY NAME here on purpose — a positional "
            "reader would mis-assign silently.";
      return arrow::Status::Invalid(os.str());
    }
    return arrow::Status::OK();
  }

  std::size_t at(const char* name) const { return index_.at(name); }
  std::size_t width() const { return width_; }

 private:
  std::unordered_map<std::string, std::size_t> index_;
  std::size_t width_ = 0;
};

// One char, refusing "" and "AB" rather than taking s[0].
arrow::Result<char> oneChar(std::string_view s, const char* field,
                            std::size_t lineNo) {
  if (s.size() != 1) {
    return arrow::Status::Invalid("CSV line ", lineNo, ": column `", field,
                                  "` is ", s.size(),
                                  " character(s) (\"", std::string(s),
                                  "\"); DBN spells it as exactly one");
  }
  return s.front();
}

// Shared across both schemas: the header fields, truncation bookkeeping and
// the symbology harvest.
struct Common {
  Transcoded out;
  std::map<std::uint32_t, std::map<std::string, std::pair<std::uint32_t, std::uint32_t>>>
      seen;  // instrument_id -> raw_symbol -> (firstDay, lastDay)
  std::map<char, std::size_t> actions;

  // ⚠ CODE-REVIEW FINDING #10, AND IT SAT DIRECTLY UNDER THE COMPLETENESS
  // CLAIM. `deliveredEnd()` publishes `lastTsEvent` as the EXCLUSIVE end written
  // to `gma.ingest_windows`, and the justification for taking the last record's
  // timestamp was "the vendor stops at `limit` records in time order" — an
  // UNCHECKED ASSUMPTION ABOUT THE INPUT. If one row arrives out of order, the
  // last row's `ts_event` is not the maximum, `recordsAtLastTsEvent` resets and
  // miscounts the final group, and the recorded window claims coverage it does
  // not have. An unasserted assumption underneath a completeness claim is the
  // same shape of defect as everything else `gma.ingest_windows` exists to stop.
  //
  // So: the maximum is tracked independently of the last row, monotonicity is
  // ASSERTED, and a violation is a refusal rather than a smaller number. The
  // two are then cross-checked in `finish()` — under monotonicity they must be
  // equal, and if they ever are not, the check that was supposed to guarantee it
  // is itself broken.
  std::uint64_t maxTsEvent = 0;
  std::size_t   recordsAtMaxTsEvent = 0;
  bool          sawDescent = false;
  std::uint64_t descentPrev = 0;
  std::uint64_t descentAt = 0;
  std::size_t   descentLine = 0;

  void note(std::uint64_t tsEvent, std::uint32_t instrumentId,
            std::string_view symbol, char action, std::uint32_t sequence,
            std::int64_t price, std::uint64_t tsRecv, std::int32_t tsInDelta,
            std::size_t lineNo) {
    if (out.records == 0) {
      out.firstTsEvent = tsEvent;
    } else if (tsEvent < out.lastTsEvent && !sawDescent) {
      sawDescent  = true;
      descentPrev = out.lastTsEvent;
      descentAt   = tsEvent;
      descentLine = lineNo;
    }
    if (tsEvent == out.lastTsEvent) {
      ++out.recordsAtLastTsEvent;
    } else {
      out.lastTsEvent = tsEvent;
      out.recordsAtLastTsEvent = 1;
    }
    if (out.records == 0 || tsEvent > maxTsEvent) {
      maxTsEvent          = tsEvent;
      recordsAtMaxTsEvent = 1;
    } else if (tsEvent == maxTsEvent) {
      ++recordsAtMaxTsEvent;
    }
    ++out.records;
    ++actions[action];
    out.probes.push_back(Transcoded::Probe{tsEvent, sequence, price, action});
    if (static_cast<std::int64_t>(tsRecv) ==
        static_cast<std::int64_t>(tsEvent) + tsInDelta) {
      ++out.tsRecvMatchesDelta;
    }
    const std::uint32_t day = yyyymmddUtc(tsEvent);
    auto& bySym = seen[instrumentId];
    auto it = bySym.find(std::string(symbol));
    if (it == bySym.end()) {
      bySym.emplace(std::string(symbol), std::pair{day, day});
    } else {
      it->second.first  = std::min(it->second.first, day);
      it->second.second = std::max(it->second.second, day);
    }
  }

  arrow::Status finish(const PullMetadata& md) {
    // THE ASSERTION #10 ASKED FOR, and it fails loudly rather than quietly
    // publishing a smaller window.
    if (sawDescent) {
      return asFinding(arrow::Status::Invalid(
          "ts_event went BACKWARDS at line ", descentLine, ": ", descentPrev,
          " -> ", descentAt,
          ". The recorded window's exclusive end is the LAST record's ts_event, "
          "and that is only the delivered end if the vendor emits in time order "
          "- which this file does not. Publishing it anyway would write a "
          "`gma.ingest_windows` claim that is short at one end and silently "
          "wrong at the other (SPEC D11). Refused rather than guessed; if "
          "out-of-order delivery is real for this dataset, the window end has "
          "to become the running maximum AND the completeness claim has to say "
          "which it is."));
    }
    // Cross-check: under monotonicity the last record IS the maximum. If these
    // ever disagree, the check above is broken rather than the data.
    if (out.lastTsEvent != maxTsEvent ||
        out.recordsAtLastTsEvent != recordsAtMaxTsEvent) {
      return arrow::Status::UnknownError(
          "internal: the monotonicity check passed but the last ts_event (",
          out.lastTsEvent, ", ", out.recordsAtLastTsEvent,
          " record(s)) is not the maximum (", maxTsEvent, ", ",
          recordsAtMaxTsEvent, " record(s))");
    }
    out.truncatedByLimit = md.limit != 0 && out.records == md.limit;
    for (const auto& [action, n] : actions) out.actionCounts.emplace_back(action, n);
    for (const auto& [id, bySym] : seen) {
      if (bySym.size() != 1) {
        std::ostringstream os;
        os << "instrument_id " << id << " carries " << bySym.size()
           << " different tickers in one file:";
        for (const auto& [s, _] : bySym) os << ' ' << s;
        os << ". `symbol` is L1's leading key column (SPEC D7) and the DBN "
              "symbology section this builds is a half-open interval per "
              "ticker; two tickers for one id on overlapping days cannot be "
              "expressed and must not be collapsed.";
        // A FINDING (exit 1), not a could-not-run: the file was read fine and
        // what it says is wrong. See Verdict.hpp.
        return asFinding(arrow::Status::Invalid(os.str()));
      }
      const auto& [sym, days] = *bySym.begin();
      out.mappings.push_back(
          Transcoded::Mapping{id, sym, days.first, days.second});
    }
    if (out.mappings.empty()) {
      return arrow::Status::Invalid(
          "CSV held no rows, so no instrument_id -> ticker mapping could be "
          "harvested. An empty pull is refused rather than written as an "
          "ingested window: `gma.ingest_windows` would then assert coverage of "
          "a window with nothing in it (docs/data-layer.md §8.8).");
    }
    return arrow::Status::OK();
  }
};

dbn::enc::DbnWriter makeWriter(const PullMetadata& md, const Transcoded& t,
                              std::uint8_t version, dbn::Schema schema) {
  dbn::enc::DbnWriter w;
  w.version = version;
  w.dataset = md.dataset;
  w.schema  = static_cast<std::uint16_t>(schema);
  w.start   = md.start;
  w.end     = md.end;
  // THE FIELD THAT MAKES TRUNCATION DETECTABLE FROM THE DBN ALONE. Without it
  // the retained file (D6) could not tell a later reload that the pull was cut
  // at its limit, and the reload would record the requested window as covered.
  w.limit         = md.limit;
  w.stypeIn       = static_cast<std::uint8_t>(dbn::SType::RawSymbol);
  w.stypeOut      = static_cast<std::uint8_t>(dbn::SType::InstrumentId);
  w.tsOut         = 0;
  w.symbolCstrLen = 71;
  w.symbols       = md.symbols;
  w.partial.clear();
  w.notFound.clear();
  w.mappings.clear();
  // stype_out == instrument_id, so `rawSymbol` is the TICKER and each
  // interval's `symbol` is the numeric id — the direction
  // DatabentoAdapter::parseMetadata reads under `outIsId`, and the direction
  // the committed vendor fixtures carry.
  for (const auto& m : t.mappings) {
    w.mappings.push_back(dbn::enc::Mapping{
        m.rawSymbol,
        {dbn::enc::Interval{m.firstDay, m.lastDay + 1, std::to_string(m.instrumentId)}}});
  }
  return w;
}

}  // namespace

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------
//
// Civil-from-days / days-from-civil, which are exact integer identities over
// the proleptic Gregorian calendar. Deliberately not `gmtime`/`timegm`: those
// are locale- and TZ-env-sensitive in ways a symbology interval must not be,
// and `timegm` is not standard C++.

std::uint32_t yyyymmddUtc(std::uint64_t tsNs) {
  const auto days = static_cast<std::int64_t>(tsNs / 86400000000000ULL);
  std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = yoe + era * 400;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t m = mp + (mp < 10 ? 3 : -9);
  const std::int64_t year = y + (m <= 2 ? 1 : 0);
  return static_cast<std::uint32_t>(year * 10000 + m * 100 + d);
}

std::uint64_t midnightUtcNs(std::uint32_t yyyymmdd) {
  std::int64_t y = yyyymmdd / 10000;
  const std::int64_t m = (yyyymmdd / 100) % 100;
  const std::int64_t d = yyyymmdd % 100;
  y -= m <= 2 ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yoe = y - era * 400;
  const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = era * 146097 + doe - 719468;
  return static_cast<std::uint64_t>(days) * 86400000000000ULL;
}

// ---------------------------------------------------------------------------
// metadata.json
// ---------------------------------------------------------------------------

arrow::Result<PullMetadata> readPullMetadata(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return arrow::Status::IOError(
        "cannot open the vendor metadata at ", path,
        ". It is retained beside the CSVs and is the only record of the "
        "REQUESTED window and the pull `limit`; neither is recoverable from "
        "the rows (docs/data-layer.md §8.8).");
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  rapidjson::Document doc;
  doc.Parse(text.c_str());
  if (doc.HasParseError() || !doc.IsObject()) {
    return arrow::Status::Invalid("metadata.json at ", path, " is not a JSON object");
  }

  PullMetadata md;
  const auto str = [](const rapidjson::Value& v, const char* k, std::string& out) {
    if (v.HasMember(k) && v[k].IsString()) out = v[k].GetString();
  };
  const auto u64 = [](const rapidjson::Value& v, const char* k, std::uint64_t& out) {
    if (v.HasMember(k) && v[k].IsUint64()) out = v[k].GetUint64();
  };

  str(doc, "job_id", md.jobId);
  if (!doc.HasMember("query") || !doc["query"].IsObject()) {
    return arrow::Status::Invalid("metadata.json at ", path, " has no `query` object");
  }
  const auto& q = doc["query"];
  str(q, "dataset", md.dataset);
  str(q, "schema", md.schema);
  str(q, "stype_in", md.stypeIn);
  str(q, "stype_out", md.stypeOut);
  u64(q, "start", md.start);
  u64(q, "end", md.end);
  u64(q, "limit", md.limit);
  if (q.HasMember("symbols") && q["symbols"].IsArray()) {
    for (const auto& s : q["symbols"].GetArray()) {
      if (s.IsString()) md.symbols.emplace_back(s.GetString());
    }
  }
  if (doc.HasMember("customizations") && doc["customizations"].IsObject()) {
    const auto& c = doc["customizations"];
    if (c.HasMember("pretty_px") && c["pretty_px"].IsBool()) {
      md.prettyPx = c["pretty_px"].GetBool();
    }
    if (c.HasMember("pretty_ts") && c["pretty_ts"].IsBool()) {
      md.prettyTs = c["pretty_ts"].GetBool();
    }
  }

  // SPEC D8 is "an assignment, not a conversion": DBN is Int64 at 1e-9 and so
  // are we. A pretty CSV carries `265.76` and `2022-06-10T12:30:01.100846675Z`
  // instead, and reading those would introduce a decimal/parse path with its
  // own rounding — a second numeric representation of a price, which is the
  // thing D1/D8 exist to prevent. Refused, not converted.
  if (md.prettyPx || md.prettyTs) {
    return arrow::Status::Invalid(
        "metadata.json at ", path, " declares pretty_px=",
        md.prettyPx ? "true" : "false", " pretty_ts=",
        md.prettyTs ? "true" : "false",
        ". Only the RAW form is accepted: prices must be Int64 at 1e-9 and "
        "timestamps raw nanoseconds (SPEC D8 — an assignment, not a "
        "conversion). Re-request the batch with both false.");
  }
  if (md.dataset.empty() || md.schema.empty()) {
    return arrow::Status::Invalid("metadata.json at ", path,
                                  " names no dataset or no schema");
  }
  if (md.end <= md.start) {
    return arrow::Status::Invalid("metadata.json at ", path, " has end (", md.end,
                                  ") <= start (", md.start, ")");
  }
  return md;
}

// ---------------------------------------------------------------------------
// mbo
// ---------------------------------------------------------------------------

arrow::Result<Transcoded> transcodeMboCsv(const std::string& csvPath,
                                          const PullMetadata& md,
                                          std::uint8_t dbnVersion) {
  std::ifstream in(csvPath, std::ios::binary);
  if (!in) return arrow::Status::IOError("cannot open ", csvPath);

  std::string line;
  if (!std::getline(in, line)) {
    return arrow::Status::Invalid(csvPath, " is empty (no header line)");
  }
  Columns cols;
  ARROW_RETURN_NOT_OK(cols.parse(
      line,
      {"ts_recv", "ts_event", "rtype", "publisher_id", "instrument_id", "action",
       "side", "price", "size", "channel_id", "order_id", "flags", "ts_in_delta",
       "sequence", "symbol"},
      csvPath));

  Common common;
  std::vector<dbn::MboMsg> msgs;
  std::size_t lineNo = 1;
  while (std::getline(in, line)) {
    ++lineNo;
    const auto raw = trimCr(line);
    if (raw.empty()) continue;
    const auto f = splitLine(raw);
    if (f.size() != cols.width()) {
      return arrow::Status::Invalid(csvPath, " line ", lineNo, " has ", f.size(),
                                    " field(s), header has ", cols.width());
    }

    dbn::MboMsg m;
    std::uint8_t rtype = 0;
    const auto num = [&](const char* name, auto& dst) -> arrow::Status {
      if (!parseInt(f[cols.at(name)], dst)) {
        return arrow::Status::Invalid(csvPath, " line ", lineNo, ": column `", name,
                                      "` is not an integer (\"",
                                      std::string(f[cols.at(name)]), "\")");
      }
      return arrow::Status::OK();
    };
    ARROW_RETURN_NOT_OK(num("rtype", rtype));
    ARROW_RETURN_NOT_OK(num("publisher_id", m.hd.publisherId));
    ARROW_RETURN_NOT_OK(num("instrument_id", m.hd.instrumentId));
    ARROW_RETURN_NOT_OK(num("ts_event", m.hd.tsEvent));
    ARROW_RETURN_NOT_OK(num("order_id", m.orderId));
    ARROW_RETURN_NOT_OK(num("price", m.price));
    ARROW_RETURN_NOT_OK(num("size", m.size));
    ARROW_RETURN_NOT_OK(num("flags", m.flags));
    ARROW_RETURN_NOT_OK(num("channel_id", m.channelId));
    ARROW_RETURN_NOT_OK(num("ts_recv", m.tsRecv));
    ARROW_RETURN_NOT_OK(num("ts_in_delta", m.tsInDelta));
    ARROW_RETURN_NOT_OK(num("sequence", m.sequence));

    // The CSV's own `rtype` must be the one this schema's record layout
    // belongs to. Writing an `MboMsg` body under a `TradeMsg` rtype (or vice
    // versa) produces a file that frames correctly and decodes every field
    // from the wrong offset.
    if (rtype != static_cast<std::uint8_t>(dbn::RType::Mbo)) {
      return arrow::Status::Invalid(
          csvPath, " line ", lineNo, ": rtype is ", static_cast<int>(rtype),
          " but the `mbo` schema is rtype ",
          static_cast<int>(dbn::RType::Mbo),
          ". The record layout is chosen by rtype; encoding this body under a "
          "different one would decode every field at the wrong offset.");
    }

    ARROW_ASSIGN_OR_RAISE(const char action, oneChar(f[cols.at("action")], "action", lineNo));
    ARROW_ASSIGN_OR_RAISE(const char side, oneChar(f[cols.at("side")], "side", lineNo));
    m.action = action;
    m.side   = side;
    if (!dbn::isDeclaredAction(m.action)) {
      return arrow::Status::Invalid(csvPath, " line ", lineNo,
                                    ": undeclared DBN action '", std::string(1, m.action),
                                    "' (declared: A C M T F R N)");
    }
    if (!dbn::isDeclaredSide(m.side)) {
      return arrow::Status::Invalid(csvPath, " line ", lineNo,
                                    ": undeclared DBN side '", std::string(1, m.side),
                                    "' (declared: A B N)");
    }

    common.note(m.hd.tsEvent, m.hd.instrumentId, f[cols.at("symbol")], m.action,
                m.sequence, m.price, m.tsRecv, m.tsInDelta, lineNo);
    msgs.push_back(m);
  }

  ARROW_RETURN_NOT_OK(common.finish(md));
  auto w = makeWriter(md, common.out, dbnVersion, dbn::Schema::Mbo);
  for (const auto& m : msgs) w.addMbo(m);
  common.out.dbn = w.build();
  return std::move(common.out);
}

// ---------------------------------------------------------------------------
// trades
// ---------------------------------------------------------------------------

arrow::Result<Transcoded> transcodeTradesCsv(const std::string& csvPath,
                                             const PullMetadata& md,
                                             std::uint8_t dbnVersion) {
  std::ifstream in(csvPath, std::ios::binary);
  if (!in) return arrow::Status::IOError("cannot open ", csvPath);

  std::string line;
  if (!std::getline(in, line)) {
    return arrow::Status::Invalid(csvPath, " is empty (no header line)");
  }
  Columns cols;
  ARROW_RETURN_NOT_OK(cols.parse(
      line,
      {"ts_recv", "ts_event", "rtype", "publisher_id", "instrument_id", "action",
       "side", "depth", "price", "size", "flags", "ts_in_delta", "sequence",
       "symbol"},
      csvPath));

  Common common;
  std::vector<dbn::TradeMsg> msgs;
  std::size_t lineNo = 1;
  while (std::getline(in, line)) {
    ++lineNo;
    const auto raw = trimCr(line);
    if (raw.empty()) continue;
    const auto f = splitLine(raw);
    if (f.size() != cols.width()) {
      return arrow::Status::Invalid(csvPath, " line ", lineNo, " has ", f.size(),
                                    " field(s), header has ", cols.width());
    }

    dbn::TradeMsg m;
    std::uint8_t rtype = 0;
    const auto num = [&](const char* name, auto& dst) -> arrow::Status {
      if (!parseInt(f[cols.at(name)], dst)) {
        return arrow::Status::Invalid(csvPath, " line ", lineNo, ": column `", name,
                                      "` is not an integer (\"",
                                      std::string(f[cols.at(name)]), "\")");
      }
      return arrow::Status::OK();
    };
    ARROW_RETURN_NOT_OK(num("rtype", rtype));
    ARROW_RETURN_NOT_OK(num("publisher_id", m.hd.publisherId));
    ARROW_RETURN_NOT_OK(num("instrument_id", m.hd.instrumentId));
    ARROW_RETURN_NOT_OK(num("ts_event", m.hd.tsEvent));
    ARROW_RETURN_NOT_OK(num("price", m.price));
    ARROW_RETURN_NOT_OK(num("size", m.size));
    ARROW_RETURN_NOT_OK(num("flags", m.flags));
    ARROW_RETURN_NOT_OK(num("depth", m.depth));
    ARROW_RETURN_NOT_OK(num("ts_recv", m.tsRecv));
    ARROW_RETURN_NOT_OK(num("ts_in_delta", m.tsInDelta));
    ARROW_RETURN_NOT_OK(num("sequence", m.sequence));

    if (rtype != static_cast<std::uint8_t>(dbn::RType::Mbp0)) {
      return arrow::Status::Invalid(
          csvPath, " line ", lineNo, ": rtype is ", static_cast<int>(rtype),
          " but the `trades` schema is rtype ",
          static_cast<int>(dbn::RType::Mbp0),
          ". The record layout is chosen by rtype; encoding this body under a "
          "different one would decode every field at the wrong offset.");
    }

    ARROW_ASSIGN_OR_RAISE(const char action, oneChar(f[cols.at("action")], "action", lineNo));
    ARROW_ASSIGN_OR_RAISE(const char side, oneChar(f[cols.at("side")], "side", lineNo));
    m.action = action;
    m.side   = side;
    if (!dbn::isDeclaredAction(m.action)) {
      return arrow::Status::Invalid(csvPath, " line ", lineNo,
                                    ": undeclared DBN action '", std::string(1, m.action),
                                    "' (declared: A C M T F R N)");
    }
    if (!dbn::isDeclaredSide(m.side)) {
      return arrow::Status::Invalid(csvPath, " line ", lineNo,
                                    ": undeclared DBN side '", std::string(1, m.side),
                                    "' (declared: A B N)");
    }

    common.note(m.hd.tsEvent, m.hd.instrumentId, f[cols.at("symbol")], m.action,
                m.sequence, m.price, m.tsRecv, m.tsInDelta, lineNo);
    msgs.push_back(m);
  }

  ARROW_RETURN_NOT_OK(common.finish(md));
  auto w = makeWriter(md, common.out, dbnVersion, dbn::Schema::Trades);
  for (const auto& m : msgs) w.addTrade(m);
  common.out.dbn = w.build();
  return std::move(common.out);
}

arrow::Result<Transcoded> transcodeCsv(const std::string& csvPath,
                                       const PullMetadata& md,
                                       std::uint8_t dbnVersion) {
  if (md.schema == "mbo") return transcodeMboCsv(csvPath, md, dbnVersion);
  if (md.schema == "trades") return transcodeTradesCsv(csvPath, md, dbnVersion);
  return arrow::Status::Invalid(
      "schema `", md.schema,
      "` has no transcoder here. Only `mbo` and `trades` are in the first "
      "slice's plan (SPEC §6), and inventing a record layout for another "
      "schema is how a decoder reads every field at the wrong offset.");
}

// ---------------------------------------------------------------------------
// Retention (SPEC D6)
// ---------------------------------------------------------------------------

arrow::Result<std::vector<std::byte>> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return arrow::Status::IOError("cannot open ", path);
  std::vector<std::byte> out;
  char chunk[65536];
  while (in.read(chunk, sizeof chunk) || in.gcount() > 0) {
    const auto n = static_cast<std::size_t>(in.gcount());
    const auto* p = reinterpret_cast<const std::byte*>(chunk);
    out.insert(out.end(), p, p + n);
    if (!in) break;
  }
  return out;
}

arrow::Result<RetentionResult> retain(const std::string& path,
                                      const std::vector<std::byte>& bytes) {
  RetentionResult r;
  r.bytes = bytes.size();
  {
    std::ifstream probe(path, std::ios::binary);
    r.existed = probe.good();
  }
  if (r.existed) {
    ARROW_ASSIGN_OR_RAISE(const auto have, readFile(path));
    if (have == bytes) {
      r.changed = false;
      return r;
    }
  }
  // Write to a sibling temp and rename, so an interrupted write never leaves a
  // half-written file that `ingest` would later read as the retained raw. D6
  // makes this file the thing L1 is rebuilt FROM; a torn one is worse than an
  // absent one.
  //
  // ⚠ THE TEMP NAME IS UNIQUE PER PROCESS, AND THAT IS NOT PEDANTRY
  // (code-review finding #9). The data root is `~/market-data/...` —
  // deliberately OUTSIDE every git working tree, which makes it the one
  // directory EVERY parallel session shares, and this workspace runs many
  // sessions by design. With a fixed `<path>.partial`, two runs interleave
  // their writes into one file and both `rename()` it over the real `.dbn`:
  // the torn retained raw this atomic rename is here to prevent, produced by
  // the rename itself. That is the ENC-1183/ENC-1185 shared-scratchpad hazard
  // (one agent's backgrounded job ran the other's script) in a new place, and
  // the mitigation recorded there is the same one used here: never a bare
  // shared name. `mkstemp` rather than pid-plus-counter because it CREATES the
  // file atomically, so two processes cannot even agree on a name by accident.
  std::string tmpl = path + ".partial.XXXXXX";
  const int fd = ::mkstemp(tmpl.data());
  if (fd < 0) {
    return arrow::Status::IOError("cannot create a temporary beside ", path, ": ",
                                  std::strerror(errno));
  }
  {
    bool wrote = true;
    std::size_t off = 0;
    while (off < bytes.size()) {
      const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
      if (n <= 0) {
        wrote = false;
        break;
      }
      off += static_cast<std::size_t>(n);
    }
    // fsync before the rename: the rename is only atomic with respect to the
    // DIRECTORY entry, and a crash between write and flush would publish a
    // name pointing at a short file.
    if (wrote && ::fsync(fd) != 0) wrote = false;
    ::close(fd);
    if (!wrote) {
      ::unlink(tmpl.c_str());
      return arrow::Status::IOError("short write to ", tmpl, ": ",
                                    std::strerror(errno));
    }
  }
  if (std::rename(tmpl.c_str(), path.c_str()) != 0) {
    const std::string why = std::strerror(errno);
    ::unlink(tmpl.c_str());
    return arrow::Status::IOError("cannot rename ", tmpl, " -> ", path, ": ", why);
  }
  r.changed = true;
  return r;
}

}  // namespace gma::data::csv
