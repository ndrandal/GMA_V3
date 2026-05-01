// ENC-44: AtomicStore enforces maxStreamKeys / maxFieldsPerStreamKey
// directly so writes from any path (Dispatcher, IEventComputer::compute,
// or external test code) are bounded uniformly.

#include "gma/AtomicStore.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace gma;

TEST(AtomicStoreCapsTest, NewStreamKeyDroppedWhenAtCap) {
  AtomicStore store;
  store.setCaps(/*maxStreamKeys=*/2, /*maxFieldsPerStreamKey=*/0);

  store.set("A", "f1", 1.0);
  store.set("B", "f1", 2.0);
  store.set("C", "f1", 3.0);  // exceeds the 2-key cap; dropped silently.

  EXPECT_TRUE(store.get("A", "f1").has_value());
  EXPECT_TRUE(store.get("B", "f1").has_value());
  EXPECT_FALSE(store.get("C", "f1").has_value());
}

TEST(AtomicStoreCapsTest, NewFieldDroppedWhenAtCap) {
  AtomicStore store;
  store.setCaps(/*maxStreamKeys=*/0, /*maxFieldsPerStreamKey=*/2);

  store.set("A", "f1", 1.0);
  store.set("A", "f2", 2.0);
  store.set("A", "f3", 3.0);  // exceeds the 2-field cap; dropped.

  EXPECT_TRUE(store.get("A", "f1").has_value());
  EXPECT_TRUE(store.get("A", "f2").has_value());
  EXPECT_FALSE(store.get("A", "f3").has_value());
}

TEST(AtomicStoreCapsTest, ExistingEntriesAlwaysUpdate) {
  // Caps only block adding NEW streamKeys / fields; existing entries
  // stay updatable even when the store is at cap.
  AtomicStore store;
  store.setCaps(/*maxStreamKeys=*/1, /*maxFieldsPerStreamKey=*/1);

  store.set("A", "f1", 1.0);
  store.set("A", "f1", 99.0);   // same key+field — must update.

  auto v = store.get("A", "f1");
  ASSERT_TRUE(v.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(*v), 99.0);
}

TEST(AtomicStoreCapsTest, BatchHonorsFieldCapPerCall) {
  AtomicStore store;
  store.setCaps(/*maxStreamKeys=*/0, /*maxFieldsPerStreamKey=*/2);

  store.setBatch("A", {{"f1", 1.0}, {"f2", 2.0}, {"f3", 3.0}});

  EXPECT_TRUE(store.get("A", "f1").has_value());
  EXPECT_TRUE(store.get("A", "f2").has_value());
  EXPECT_FALSE(store.get("A", "f3").has_value());
}

TEST(AtomicStoreCapsTest, ZeroCapMeansUnlimited) {
  AtomicStore store;  // default-constructed = no caps.
  for (int i = 0; i < 50; ++i) {
    store.set(std::string("k") + std::to_string(i), "f", static_cast<double>(i));
  }
  EXPECT_TRUE(store.get("k0",  "f").has_value());
  EXPECT_TRUE(store.get("k49", "f").has_value());
}
