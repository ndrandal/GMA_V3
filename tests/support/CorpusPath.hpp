// tests/support/CorpusPath.hpp — ENC-1340
//
// ONE resolver for `corpus_requests.json`, and a diagnostic that names the
// problem instead of letting it wear a test failure's clothes.
//
// The failure this exists to kill: five test files each carried their own copy
// of the same three cwd-relative search paths
//
//     "corpus_requests.json"
//     "../tests/treebuilder/corpus_requests.json"
//     "tests/treebuilder/corpus_requests.json"
//
// all of which are relative to the CURRENT WORKING DIRECTORY. CMake's
// `add_test(... WORKING_DIRECTORY $<TARGET_FILE_DIR:gma_tests>)` makes that
// true under ctest, and a POST_BUILD step copies the corpus next to the binary
// — so `ctest` is always fine. Running the RAW binary by hand from anywhere
// else resolved none of the three, and the suite answered with **12 test
// failures and a sanitizer warning** — a false negative wearing a sanitizer's
// authority, which cost ENC-1338 an hour of hunting a bug that was not there.
//
// Two changes close it:
//
//   1. SELF-HEALING — the candidate list now also contains paths that do not
//      depend on the cwd at all: the directory of the running executable
//      (where POST_BUILD put the copy) and the configure-time source tree
//      (`GMA_TEST_SOURCE_DIR`, injected by CMake). A binary built by this
//      project therefore finds its corpus from ANY working directory.
//
//   2. SELF-DIAGNOSING — when it genuinely cannot be found, callers print
//      `corpusNotFoundDiagnostic()`, which names the cwd, every path tried,
//      and what to do. `test_bootstrap.cpp` also fails the run up front with
//      that same message, so the answer is one line at the top rather than a
//      dozen unrelated-looking reds further down.
//
// Header-only and test-only: nothing under include/ or src/ may depend on it.
#pragma once

#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
#  include <filesystem>
#endif

namespace gma::testsupport {

// Every path the corpus is looked for, in order. The first three are the
// historical cwd-relative ones (kept so a hand-run from the build dir or the
// source root behaves exactly as it always did); the rest are cwd-independent.
inline const std::vector<std::string>& corpusCandidates() {
  static const std::vector<std::string> paths = [] {
    std::vector<std::string> v{
      "corpus_requests.json",
      "../tests/treebuilder/corpus_requests.json",
      "tests/treebuilder/corpus_requests.json",
    };
#if defined(__linux__)
    // Next to the running binary — where the POST_BUILD copy lands. Resolved
    // from /proc/self/exe so it holds no matter what the cwd is.
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && exe.has_parent_path())
      v.push_back((exe.parent_path() / "corpus_requests.json").string());
#endif
#if defined(GMA_TEST_SOURCE_DIR)
    // The checked-in original, at its configure-time absolute path.
    v.push_back(std::string(GMA_TEST_SOURCE_DIR) +
                "/tests/treebuilder/corpus_requests.json");
#endif
    return v;
  }();
  return paths;
}

// The resolved corpus path, or an empty string when no candidate exists.
// Resolved once per process.
inline const std::string& corpusRequestsPath() {
  static const std::string resolved = [] {
    for (const auto& p : corpusCandidates()) {
      std::ifstream probe(p);
      if (probe.is_open()) return p;
    }
    return std::string{};
  }();
  return resolved;
}

inline bool corpusRequestsFound() { return !corpusRequestsPath().empty(); }

// An open stream on the corpus, or a closed one when it could not be found.
// Callers assert on is_open() and print corpusNotFoundDiagnostic().
inline std::ifstream openCorpusRequests() {
  const auto& p = corpusRequestsPath();
  return p.empty() ? std::ifstream{} : std::ifstream{p};
}

// The message. Names the cwd (the usual culprit) and every path tried.
inline std::string corpusNotFoundDiagnostic() {
  std::string cwd = "<unknown>";
#if defined(__linux__)
  std::error_code ec;
  const auto p = std::filesystem::current_path(ec);
  if (!ec) cwd = p.string();
#endif
  std::string s =
      "corpus_requests.json could not be found.\n"
      "  This is almost always a WORKING-DIRECTORY problem, not a code "
      "problem: the corpus is copied next to the gma_tests binary at build "
      "time and `ctest` runs the binary from that directory "
      "(add_test sets WORKING_DIRECTORY). A hand-run of the raw binary from "
      "somewhere else is the failure mode — it produces a dozen unrelated-"
      "looking test failures that are ALL this one missing file.\n"
      "  Fix: run `ctest --test-dir build --output-on-failure`, or cd into the "
      "directory holding gma_tests before running it by hand.\n"
      "  cwd: " + cwd + "\n"
      "  searched:\n";
  for (const auto& c : corpusCandidates()) s += "    - " + c + "\n";
  return s;
}

} // namespace gma::testsupport
