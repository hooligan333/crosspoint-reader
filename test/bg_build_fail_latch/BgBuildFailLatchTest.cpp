// Failure bookkeeping of the core-0 background build task
// (activities/reader/BgBuildFailLatch.h).
#include <gtest/gtest.h>

#include "activities/reader/BgBuildFailLatch.h"

namespace {

using bg_build_fail::appliesTo;
using bg_build_fail::Latch;

TEST(BgBuildFailLatch, ReportAppliesOnlyToTheSpineStillOpen) {
  EXPECT_TRUE(appliesTo(4, true, 4));
  // The reader turned into the next chapter before the loop consumed the report.
  EXPECT_FALSE(appliesTo(4, true, 5));
  // No section (already reset by a render or another path).
  EXPECT_FALSE(appliesTo(4, false, 4));
  // No spine recorded.
  EXPECT_FALSE(appliesTo(-1, true, -1));
}

TEST(BgBuildFailLatch, FirstFailureRetriesSecondOfSameSpineLatches) {
  Latch latch;
  EXPECT_FALSE(latch.onFailure(3));
  EXPECT_EQ(latch.lastFailedSpine(), 3);
  EXPECT_TRUE(latch.onFailure(3));
}

TEST(BgBuildFailLatch, FailuresOfDifferentSpinesNeverLatch) {
  Latch latch;
  EXPECT_FALSE(latch.onFailure(3));
  EXPECT_FALSE(latch.onFailure(4));
  EXPECT_FALSE(latch.onFailure(3));
}

TEST(BgBuildFailLatch, CompletedBuildForgetsTheFailure) {
  Latch latch;
  EXPECT_FALSE(latch.onFailure(3));
  latch.onBuildComplete(3);
  EXPECT_EQ(latch.lastFailedSpine(), -1);
  EXPECT_FALSE(latch.onFailure(3));
  // Completion of another spine does not clear it.
  latch.onBuildComplete(7);
  EXPECT_TRUE(latch.onFailure(3));
}

TEST(BgBuildFailLatch, ChapterChangeForgetsTheFailure) {
  Latch latch;
  EXPECT_FALSE(latch.onFailure(3));
  latch.onSpine(3);  // still in the chapter: remembered
  EXPECT_EQ(latch.lastFailedSpine(), 3);
  latch.onSpine(4);  // left it
  EXPECT_EQ(latch.lastFailedSpine(), -1);
  latch.onSpine(3);  // came back: a fresh first failure
  EXPECT_FALSE(latch.onFailure(3));
}

}  // namespace
