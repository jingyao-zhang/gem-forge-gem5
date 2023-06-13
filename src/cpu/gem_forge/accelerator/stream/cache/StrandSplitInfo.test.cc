
#include <gtest/gtest.h>

#include <cassert>
#include <iostream>
#include <type_traits>

#include "StrandSplitInfo.hh"
#include "base/cprintf.hh"

using namespace gem5;

namespace {

class StrandSplitInfo2D : public testing::Test {
protected:
  std::unique_ptr<StrandSplitInfo> splitInfo;

  void SetUp() override {

    std::vector<int64_t> trips{1024, 1024};
    std::vector<StrandSplitInfo::SplitDim> splitDims{
        {0, 8, 2},
        {1, 4, 16},
    };

    /**
     * Original loop:
     * for i = 0: 1024
     *   for j = 0: 1024
     *      a[i * 1024 + j]
     *
     * New split loop (ti and tj are strand id).
     *
     * for ti = 0 : 4
     *   for tj = 0 : 8
     *     for i = 0 : 16
     *       for j = 0 : 64
     *         for ii = 0 : 16
     *           for jj = 0 : 2
     *              a[(i * 64 + ti * 16 + ii) * 1024 +
     *                (j * 16 + tj * 2 + jj)]
     *
     * Essentially breaks 1k square into 2x16 tiles,
     * and 8x4 strands.
     *
     */
    splitInfo = std::make_unique<StrandSplitInfo>(trips, splitDims);
  }
};

TEST_F(StrandSplitInfo2D, StreamToStrand) {
  EXPECT_EQ(splitInfo->mapStreamToStrand(0).strandIdx, 0);
  EXPECT_EQ(splitInfo->mapStreamToStrand(0).elemIdx, 0);

  EXPECT_EQ(splitInfo->mapStreamToStrand(1).strandIdx, 0);
  EXPECT_EQ(splitInfo->mapStreamToStrand(1).elemIdx, 1);

  EXPECT_EQ(splitInfo->mapStreamToStrand(2).strandIdx, 1);
  EXPECT_EQ(splitInfo->mapStreamToStrand(2).elemIdx, 0);

  EXPECT_EQ(splitInfo->mapStreamToStrand(4).strandIdx, 2);
  EXPECT_EQ(splitInfo->mapStreamToStrand(4).elemIdx, 0);

  EXPECT_EQ(splitInfo->mapStreamToStrand(16).strandIdx, 0);
  EXPECT_EQ(splitInfo->mapStreamToStrand(16).elemIdx, 2);

  // The start of second row.
  EXPECT_EQ(splitInfo->mapStreamToStrand(1024).strandIdx, 0);
  EXPECT_EQ(splitInfo->mapStreamToStrand(1024).elemIdx, 128);

  // The last one.
  EXPECT_EQ(splitInfo->mapStreamToStrand(1024 * 1024 - 1).strandIdx, 31);
  EXPECT_EQ(splitInfo->mapStreamToStrand(1024 * 1024 - 1).elemIdx,
            32 * 1024 - 1);
}

TEST_F(StrandSplitInfo2D, StrandTrip) {
  auto streamTrip = 1024 * 1024;
  auto strandTrip = streamTrip / 32;
  EXPECT_EQ(splitInfo->getStrandTripCount(streamTrip, 0), strandTrip);
  EXPECT_EQ(splitInfo->getStrandTripCount(streamTrip, 1), strandTrip);
  EXPECT_EQ(splitInfo->getStrandTripCount(streamTrip, 15), strandTrip);
  EXPECT_EQ(splitInfo->getStrandTripCount(streamTrip, 31), strandTrip);
}

TEST_F(StrandSplitInfo2D, StrandToStream) {
  EXPECT_EQ(splitInfo->mapStrandToStream(StrandElemSplitIdx(0, 0)), 0);
  EXPECT_EQ(splitInfo->mapStrandToStream(StrandElemSplitIdx(0, 1)), 1);
  EXPECT_EQ(splitInfo->mapStrandToStream(StrandElemSplitIdx(1, 0)), 2);
  EXPECT_EQ(splitInfo->mapStrandToStream(StrandElemSplitIdx(2, 0)), 4);
  EXPECT_EQ(splitInfo->mapStrandToStream(StrandElemSplitIdx(0, 2)), 16);

  // The start of second row.
  EXPECT_EQ(splitInfo->mapStrandToStream(StrandElemSplitIdx(0, 128)), 1024);

  // The last one.
  EXPECT_EQ(splitInfo->mapStrandToStream(StrandElemSplitIdx(31, 32 * 1024 - 1)),
            1024 * 1024 - 1);
}

TEST_F(StrandSplitInfo2D, PrevStrand) {
  // The last elem.
  auto prevStrands = splitInfo->mapStreamToPrevStrand(1024 * 1024 - 1);
  EXPECT_EQ(prevStrands.size(), 32);
  for (int strand = 0; strand < prevStrands.size(); ++strand) {
    const auto &prev = prevStrands.at(strand);
    EXPECT_EQ(prev.strandIdx, strand);
    EXPECT_EQ(prev.elemIdx, 32 * 1024 - 1);
  }
  // The first elem.
  prevStrands = splitInfo->mapStreamToPrevStrand(0);
  EXPECT_EQ(prevStrands.size(), 1);
  EXPECT_EQ(prevStrands.front().strandIdx, 0);
  EXPECT_EQ(prevStrands.front().elemIdx, 0);
  // The middle elem.
  auto streamElemIdx = 511 * 1024 + 511;
  prevStrands = splitInfo->mapStreamToPrevStrand(streamElemIdx);
  EXPECT_EQ(prevStrands.size(), 32);
  // Check the condition that if convert PrevStrand to Stream, it's larger.
  for (int strand = 0; strand < prevStrands.size(); ++strand) {
    const auto &prev = prevStrands.at(strand);
    EXPECT_EQ(prev.strandIdx, strand);
    auto prevStreamElemIdx = splitInfo->mapStrandToStream(prev);
    EXPECT_LE(prevStreamElemIdx, streamElemIdx);
    auto nextStreamElemIdx = splitInfo->mapStrandToStream(
        StrandElemSplitIdx(prev.strandIdx, prev.elemIdx + 1));
    EXPECT_GT(nextStreamElemIdx, streamElemIdx);
  }
}

} // namespace