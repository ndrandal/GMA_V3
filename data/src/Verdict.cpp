//
// Verdict — see Verdict.hpp for why the classification is explicit.
//

#include "gma/data/Verdict.hpp"

#include <cstring>

namespace gma::data {

arrow::Status asFinding(arrow::Status s) {
  if (s.ok()) return s;
  // Deliberately replaces any existing detail rather than nesting: a status
  // reaching here has been classified by the site that knows, and a second
  // opinion further out is exactly the guess this file removes.
  return arrow::Status(s.code(), s.message(), std::make_shared<FindingDetail>());
}

bool isFinding(const arrow::Status& s) {
  if (s.ok()) return false;
  const auto& detail = s.detail();
  return detail != nullptr &&
         std::strcmp(detail->type_id(), FindingDetail::kTypeId) == 0;
}

int exitCodeFor(const arrow::Status& s) {
  if (s.ok()) return 0;
  return isFinding(s) ? 1 : 2;
}

}  // namespace gma::data
