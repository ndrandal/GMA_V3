#pragma once
//
// Which exit code an error means (ENC-1483, code-review finding #1).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D11; `gma_ingest`'s documented
//         exit codes — 0 clean, 1 A FINDING, 2 COULD NOT RUN.
//
// THE DEFECT THIS EXISTS TO CLOSE, AND IT WAS THE INVERSE OF THE ONE GUARDED
// AGAINST. `gma_ingest`'s header promises three exit codes and explains why:
// "a tool that returns 1 for both makes 'the gate is red' and 'the gate never
// ran' the same observation". `cmdIngest` then mapped EVERY non-OK status to
// exit 2, so a round-trip mismatch, a retained DBN that would not read back,
// two tickers for one `instrument_id` and an empty delivered window — the four
// cases the job exists to catch — all reported as COULD NOT RUN. Reproduced
// against the binary: two tickers on one `instrument_id` printed its refusal
// and exited 2. **A red data gate reporting as not-run is worse than the
// conflation the comment warned about**, because a sweep that treats 2 as
// "environment, retry later" discards the finding entirely. This project has
// now fixed that same conflation three times (ENC-1480's `load_aggregates`,
// this tool's exit-code discipline, and this).
//
// WHY A `StatusDetail` AND NOT A STATUS CODE. `arrow::StatusCode` is a fixed
// enum, and the obvious mapping (`Invalid` -> finding, `IOError` -> could not
// run) is wrong in both directions here: a malformed CSV column list is
// naturally `Invalid` and is a could-not-run, while "the retained DBN does not
// read back" was naturally `IOError` and is a finding. Overloading the code
// would decide the question by whichever constructor read best at each site.
// `arrow::StatusDetail` is Arrow's own extension point, it survives
// `ARROW_RETURN_NOT_OK` propagation untouched, and it makes the classification
// an explicit act at the refusal site rather than a guess at the boundary.
//
// THE DEFAULT IS COULD-NOT-RUN, AND THAT IS A DELIBERATE ASYMMETRY. An
// untagged status is one nobody classified — including every status Arrow and
// the standard library raise — and those are overwhelmingly environmental. The
// cost of the default being wrong is bounded by the set of sites that can be
// findings being small, enumerable and TESTED: see
// `IngestVerdict.*` in data/tests/DatabentoCsvTest.cpp, which asserts both
// that every finding site is tagged and that an untagged status is not.
//

#include <arrow/status.h>

#include <memory>
#include <string>

namespace gma::data {

// Attached to a status that means "the gate is RED", as distinct from "the gate
// could not run".
class FindingDetail : public arrow::StatusDetail {
 public:
  const char* type_id() const override { return kTypeId; }
  std::string ToString() const override {
    return "a finding about the data or about L1's state (exit 1), not an "
           "inability to run (exit 2)";
  }
  static constexpr const char* kTypeId = "gma::data::FindingDetail";
};

// Tag `s` as a finding. Returns `s` unchanged when it is OK, so it can wrap a
// call site without a branch.
arrow::Status asFinding(arrow::Status s);

// True iff `s` is a non-OK status tagged by `asFinding`.
bool isFinding(const arrow::Status& s);

// The exit code `s` means: 0 OK, 1 tagged finding, 2 anything else.
int exitCodeFor(const arrow::Status& s);

}  // namespace gma::data
