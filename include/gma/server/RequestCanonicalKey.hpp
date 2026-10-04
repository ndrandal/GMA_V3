// include/gma/server/RequestCanonicalKey.hpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>

namespace gma::server {

// ENC-1041. The EQUIVALENCE RELATION behind cross-connection subscription
// dedup, and the rules under which it refuses to apply.
//
// ------------------------------------------------------------------------
// WHAT MAKES TWO SUBSCRIPTIONS IDENTICAL
// ------------------------------------------------------------------------
// Two subscriptions share one computation iff `canonicalizeRequest` returns
// the same non-empty `key` for both. The key is a canonical serialization of
// the NORMALIZED request document that `ClientSession::handleSubscribe` builds
// — `streamKey`, `field`, and whichever of `pipeline` / `stages` / `node` the
// client supplied in the right JSON type — with:
//
//   * object members STABLE-sorted by name. JSON object member order carries
//     no meaning (rapidjson's `operator[]` finds a member by name, and
//     `TreeBuilder` only ever looks members up by name), so `{"type":"Worker",
//     "fn":"mean"}` and `{"fn":"mean","type":"Worker"}` are the same request.
//     The sort is STABLE so that a document with a REPEATED member name keeps
//     those members in their original relative order — rapidjson resolves a
//     repeated name to the FIRST occurrence, so `{"a":1,"a":2}` and
//     `{"a":2,"a":1}` build different trees and must not collide.
//   * arrays left in order. Array order IS meaning here: `pipeline`/`stages`
//     is an ordered chain and a fan-in's `inputs` is positional (`Pack` and
//     `Aggregate` hand their children's values downstream by position). No
//     commutativity normalisation is attempted even where the reducer happens
//     to be commutative.
//   * scalars written through rapidjson's own writer, i.e. in the form implied
//     by the type the value PARSED to.
//
// ------------------------------------------------------------------------
// WHAT IT DELIBERATELY DOES NOT MERGE
// ------------------------------------------------------------------------
// Every one of these is a MISSED merge, never a wrong one — the refusal path
// is the pre-dedup behaviour (build your own DAG), which is always correct.
//
//  N1. Different numeric spelling. `20`, `20.0` and `2e1` are equal as JSON
//      numbers but parse to different rapidjson types and serialize
//      differently, so they do not merge. Measured on the 272-entry corpus:
//      collapsing them makes no difference to the key count at all.
//  N2. `streamKey` / `field` differing only in case or surrounding
//      whitespace. These are verbatim `AtomicStore` keys and the store is
//      case-sensitive; merging `"BTC"` with `"btc"` would feed a subscriber a
//      different stream than it asked for.
//  N3. Absent vs. present-and-empty: `{...}` and `{..., "pipeline": []}` are
//      distinct keys even where `buildForRequest` happens to treat them the
//      same, because that equality is TreeBuilder's business and not something
//      this file can assert without re-implementing it.
//  N4. Anything a client sent that `handleSubscribe` drops on the floor is
//      already gone before this is called — unknown top-level members, a
//      `pipeline` that is not an array — so two requests differing only in
//      junk DO merge. That is the normalisation `handleSubscribe` already did,
//      not an extra one taken here.
//  N5. A request whose tree declares an ATTACH-SENSITIVE node: see
//      `NodeShareClass` below. Refused, with the offending type named.
//  N6. A request whose tree declares a node type this build does not classify.
//      FAIL CLOSED: a node type added without a classification is refused, not
//      merged on the assumption that it is harmless.
//  N7. A document nested deeper than `kMaxCanonicalDepth`, or carrying a
//      `type` member that is not a string. Refused rather than throwing: these
//      requests may still BUILD fine, and dedup must not be able to reject a
//      subscription that the pre-dedup server would have accepted.
// ------------------------------------------------------------------------

// Sharing eligibility of one node type, which is the question "if a second
// subscriber attaches to a DAG containing this node at an arbitrary moment,
// can it observe a value sequence it could not have observed from its own
// freshly-built DAG?"
enum class NodeShareClass {
  // No. The node's output at a given moment is a function of the values
  // arriving then, plus immutable config — not of anything accumulated before
  // the subscriber attached.
  AttachInvariant,
  // Yes. Refuse to share a DAG containing one.
  AttachSensitive,
  // This build does not know. Refuse (N6).
  Unknown,
};

NodeShareClass classifyNodeType(std::string_view type) noexcept;

// Every node type this build classifies, with its class. Exposed so a test can
// assert the classification COVERS every buildable type rather than merely
// agreeing with itself — see tests/server/SharedSubscriptionTest.cpp.
struct ClassifiedNodeType {
  const char*    type;
  NodeShareClass cls;
};
const std::vector<ClassifiedNodeType>& classifiedNodeTypes();

// Why a request cannot be shared. `None` means it can.
enum class ShareRefusal {
  None,
  NotAnObject,       // the normalized request is not a JSON object
  DepthExceeded,     // N7
  MalformedType,     // N7 — a `type` member present but not a string
  AttachSensitiveNode,  // N5 — `detail` names the type
  UnclassifiedNode,     // N6 — `detail` names the type
};

struct CanonicalRequest {
  std::string  key;      // empty unless `refusal == None`
  ShareRefusal refusal{ShareRefusal::None};
  std::string  detail;   // the offending node type, when there is one

  bool shareable() const noexcept { return refusal == ShareRefusal::None; }
};

// Maximum object/array nesting `canonicalizeRequest` will walk. Independent of
// `JsonValidator::MAX_TREE_DEPTH` on purpose — this is a recursion bound on
// THIS function, not a restatement of what the server accepts, and exceeding it
// refuses to share rather than refusing the request.
inline constexpr int kMaxCanonicalDepth = 64;

// Canonicalize the normalized request document `handleSubscribe` built.
// Never throws.
CanonicalRequest canonicalizeRequest(const rapidjson::Value& rq) noexcept;

// Human-readable refusal reason, for the log line and for test messages.
const char* shareRefusalName(ShareRefusal r) noexcept;

} // namespace gma::server
