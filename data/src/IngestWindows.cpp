//
// IngestWindows — see IngestWindows.hpp for the rule and why each clause is
// load-bearing.
//

#include "gma/data/IngestWindows.hpp"

#include <algorithm>
#include <map>

namespace gma::data {

std::vector<Interval> mergeIntervals(std::vector<Interval> in) {
  // Drop empty and inverted intervals: a recorded [t, t) covers nothing, and an
  // inverted one is a caller bug.
  //
  // HONEST ABOUT WHAT THIS BUYS, because the first version of this comment
  // claimed more. It said an empty interval "would be a free adjacency bridge
  // between two real windows", and a falsification sweep showed that is FALSE:
  // removing the filter leaves every `isComplete` verdict unchanged, because an
  // empty [t, t) can only merge where the intervals are already adjacent and it
  // extends nothing (`toNs = max(back.toNs, t)` with `t <= back.toNs`). The
  // filter is belt-and-braces for the `covered` REPORT, which a reader looks
  // at, not a safety property of the verdict. It is kept because a zero-width
  // interval in a human-facing coverage list is noise, and deleted claims are
  // better than impressive ones.
  //
  // The same sweep found that the ADJACENCY JOIN below is likewise not
  // load-bearing for the verdict: the coverage walk in `isComplete` advances a
  // cursor across sorted intervals and therefore joins adjacent ones itself. It
  // IS load-bearing for `covered`, and `IngestWindowsMerge` is where both are
  // asserted directly. Neither is dead code; both are over-claimed if described
  // as protecting the answer.
  in.erase(std::remove_if(in.begin(), in.end(),
                          [](const Interval& i) { return i.toNs <= i.fromNs; }),
           in.end());
  std::sort(in.begin(), in.end(), [](const Interval& a, const Interval& b) {
    return a.fromNs != b.fromNs ? a.fromNs < b.fromNs : a.toNs < b.toNs;
  });
  std::vector<Interval> out;
  for (const auto& i : in) {
    // `<=` rather than `<`: these are half-open, so [a, b) and [b, c) are
    // ADJACENT and join. That is what makes a seven-day request over seven
    // daily windows complete.
    if (!out.empty() && i.fromNs <= out.back().toNs) {
      out.back().toNs = std::max(out.back().toNs, i.toNs);
    } else {
      out.push_back(i);
    }
  }
  return out;
}

Completeness isComplete(const std::vector<IngestWindowRow>& rows,
                        std::int64_t reqFromNs, std::int64_t reqToNs) {
  Completeness v;

  // ---- step 1: resolve the newest row PER RECORDED WINDOW ----------------
  //
  // Keyed on (from_ts, to_ts). `to_ts` is in the key because resolving per
  // `from_ts` alone would let a recorded [00:00, 12:00) and a recorded
  // [00:00, 24:00) resolve against each other, and the newer of the two would
  // decide the coverage of a range the older one never claimed.
  struct Resolved {
    std::int64_t updatedAtMs = 0;
    std::uint8_t state       = 0;
  };
  std::map<std::pair<std::int64_t, std::int64_t>, Resolved> newest;
  for (const auto& r : rows) {
    const std::pair<std::int64_t, std::int64_t> key{r.fromTsNs, r.toTsNs};
    const auto it = newest.find(key);
    if (it == newest.end()) {
      newest.emplace(key, Resolved{r.updatedAtMs, r.state});
      continue;
    }
    if (r.updatedAtMs > it->second.updatedAtMs) {
      it->second = Resolved{r.updatedAtMs, r.state};
    } else if (r.updatedAtMs == it->second.updatedAtMs) {
      // A TIE. See the header: the lowest state wins, so a tie between
      // complete and in-flight reads as in-flight. Pessimistic in the
      // direction that fails the query rather than the one that answers it
      // over data that may be gone.
      it->second.state = std::min(it->second.state, r.state);
    }
  }
  v.resolved = newest.size();

  // ---- step 2: only now does state=1 select ------------------------------
  std::vector<Interval> w;
  for (const auto& [key, res] : newest) {
    if (res.state == 1) {
      w.push_back(Interval{key.first, key.second});
      ++v.survived;
    }
  }
  v.survivingWindows = w;
  v.covered = mergeIntervals(std::move(w));

  // ---- step 3: coverage of the REQUEST, not of a row ---------------------
  if (reqToNs <= reqFromNs) {
    // An empty or inverted request is not "trivially complete". It is a caller
    // bug, and answering `true` would make a mis-ordered range read as loaded.
    v.complete = false;
    v.gap      = Interval{reqFromNs, reqToNs};
    return v;
  }
  std::int64_t cursor = reqFromNs;
  for (const auto& c : v.covered) {
    if (c.fromNs > cursor) break;        // a hole starts at `cursor`
    cursor = std::max(cursor, c.toNs);
    if (cursor >= reqToNs) break;
  }
  if (cursor >= reqToNs) {
    v.complete = true;
    return v;
  }
  // The hole runs from `cursor` to the start of the next covering window, or
  // to the end of the request if nothing else covers any of it.
  std::int64_t holeEnd = reqToNs;
  for (const auto& c : v.covered) {
    if (c.fromNs > cursor) {
      holeEnd = std::min(holeEnd, c.fromNs);
      break;
    }
  }
  v.complete = false;
  v.gap      = Interval{cursor, holeEnd};
  return v;
}

}  // namespace gma::data
