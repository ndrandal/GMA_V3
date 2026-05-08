// gma-string-id-subscriptions phase 2 task 2 (ENC-388):
// std::hash<RequestKey> specialization unit tests.
//
// The variant carries either int or std::string. A naive hash that just
// forwards to the underlying alternative's hash collapses int(5) and
// string("5") onto the same bucket, which would silently break
// ClientSession::active_/chains_ when both an int-keyed and a string-keyed
// subscription with numerically-identical content coexist. The salt on
// the variant index guards against that.

#include "gma/server/RequestKey.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

using gma::server::RequestKey;
using gma::server::requestKeyInt;
using gma::server::requestKeyStr;

TEST(RequestKeyHashTest, IntAndStringWithSameTextHashDifferently) {
  RequestKey i = requestKeyInt(5);
  RequestKey s = requestKeyStr("5");
  std::hash<RequestKey> h;
  EXPECT_NE(h(i), h(s))
    << "int(5) and string(\"5\") must not share a hash bucket";
}

TEST(RequestKeyHashTest, SameAlternativeSameValueHashesEqual) {
  std::hash<RequestKey> h;
  EXPECT_EQ(h(requestKeyInt(7)),  h(requestKeyInt(7)));
  EXPECT_EQ(h(requestKeyStr("foo")), h(requestKeyStr("foo")));
}

TEST(RequestKeyHashTest, BothCoexistAsMapKeys) {
  std::unordered_map<RequestKey, std::string> m;
  m[requestKeyInt(5)]  = "int-value";
  m[requestKeyStr("5")] = "string-value";

  ASSERT_EQ(m.size(), 2u)
    << "int(5) and string(\"5\") collapsed to a single map slot";

  auto itInt = m.find(requestKeyInt(5));
  auto itStr = m.find(requestKeyStr("5"));
  ASSERT_NE(itInt, m.end());
  ASSERT_NE(itStr, m.end());
  EXPECT_EQ(itInt->second, "int-value");
  EXPECT_EQ(itStr->second, "string-value");
}

TEST(RequestKeyHashTest, EraseByEitherAlternativeIsIndependent) {
  std::unordered_map<RequestKey, int> m;
  m[requestKeyInt(42)]    = 1;
  m[requestKeyStr("42")]  = 2;

  EXPECT_EQ(m.erase(requestKeyInt(42)), 1u);
  EXPECT_EQ(m.size(), 1u);
  EXPECT_NE(m.find(requestKeyStr("42")), m.end());
  EXPECT_EQ(m[requestKeyStr("42")], 2);
}
