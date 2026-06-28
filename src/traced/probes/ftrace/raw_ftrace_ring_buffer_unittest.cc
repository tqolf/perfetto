/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/traced/probes/ftrace/raw_ftrace_ring_buffer.h"

#include <unistd.h>

#include <string>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

constexpr size_t kPageSize = 4096;

std::vector<uint8_t> MakePage(uint8_t fill) {
  return std::vector<uint8_t>(kPageSize, fill);
}

// A unique path for the disk-overflow file. EnableDiskOverflow uses
// open-then-unlink, so the file auto-removes; tests must not use base::TempFile
// (which would double-unlink and crash).
std::string DiskTempPath(int uniq) {
  return "/tmp/raw_ftrace_rb_test_" + std::to_string(getpid()) + "_" +
         std::to_string(uniq);
}

TEST(RawFtraceRingBufferTest, EmptyHasZeroSize) {
  RawFtraceRingBuffer buf(4, kPageSize);
  EXPECT_EQ(buf.size(), 0u);
  EXPECT_EQ(buf.capacity_pages(), 4u);
}

TEST(RawFtraceRingBufferTest, GrowsUntilCapacity) {
  RawFtraceRingBuffer buf(4, kPageSize);
  for (uint8_t i = 0; i < 3; i++)
    buf.PushPage(MakePage(i).data(), /*page_ts=*/i);
  EXPECT_EQ(buf.size(), 3u);
}

TEST(RawFtraceRingBufferTest, OverwritesOldestWhenFull) {
  RawFtraceRingBuffer buf(2, kPageSize);
  buf.PushPage(MakePage(10).data(), 10);
  buf.PushPage(MakePage(20).data(), 20);
  buf.PushPage(MakePage(30).data(), 30);  // Overwrites ts=10.
  EXPECT_EQ(buf.size(), 2u);

  std::vector<uint64_t> seen;
  buf.ForEachPageSince(0, [&](const uint8_t* p, uint64_t ts) {
    seen.push_back(ts);
    EXPECT_EQ(p[0], static_cast<uint8_t>(ts));  // Content tracks ts.
  });
  EXPECT_EQ(seen, (std::vector<uint64_t>{20, 30}));  // Oldest-to-newest.
}

TEST(RawFtraceRingBufferTest, ForEachPageSinceFiltersByCutoff) {
  RawFtraceRingBuffer buf(8, kPageSize);
  for (uint64_t ts : {5u, 15u, 25u, 35u})
    buf.PushPage(MakePage(static_cast<uint8_t>(ts)).data(), ts);

  std::vector<uint64_t> seen;
  buf.ForEachPageSince(
      20, [&](const uint8_t*, uint64_t ts) { seen.push_back(ts); });
  EXPECT_EQ(seen, (std::vector<uint64_t>{25, 35}));
}

TEST(RawFtraceRingBufferTest, ClearResetsSize) {
  RawFtraceRingBuffer buf(4, kPageSize);
  buf.PushPage(MakePage(1).data(), 1);
  buf.Clear();
  EXPECT_EQ(buf.size(), 0u);
}

TEST(RawFtraceRingBufferTest, DiskOverflowRetainsEvictedPages) {
  RawFtraceRingBuffer buf(/*capacity_pages=*/2, kPageSize);
  ASSERT_TRUE(buf.EnableDiskOverflow(DiskTempPath(__LINE__),
                                     /*disk_capacity_pages=*/4));
  // Mem holds newest 2 (ts 4,5); disk holds the evicted older 1,2,3.
  for (uint64_t ts = 1; ts <= 5; ts++)
    buf.PushPage(MakePage(static_cast<uint8_t>(ts)).data(), ts);

  std::vector<uint64_t> seen;
  buf.ForEachPageSince(0, [&](const uint8_t* p, uint64_t ts) {
    seen.push_back(ts);
    EXPECT_EQ(p[0],
              static_cast<uint8_t>(ts));  // Content survives disk round-trip.
  });
  EXPECT_EQ(seen,
            (std::vector<uint64_t>{1, 2, 3, 4, 5}));  // All retained, in order.
}

TEST(RawFtraceRingBufferTest, DiskOverflowDropsOldestWhenTotalFull) {
  RawFtraceRingBuffer buf(/*capacity_pages=*/2, kPageSize);
  ASSERT_TRUE(buf.EnableDiskOverflow(DiskTempPath(__LINE__),
                                     /*disk_capacity_pages=*/2));
  // Total capacity = mem 2 + disk 2 = 4. Push 6 -> keep newest 4 (ts 3..6).
  for (uint64_t ts = 1; ts <= 6; ts++)
    buf.PushPage(MakePage(static_cast<uint8_t>(ts)).data(), ts);

  std::vector<uint64_t> seen;
  buf.ForEachPageSince(
      0, [&](const uint8_t*, uint64_t ts) { seen.push_back(ts); });
  EXPECT_EQ(seen, (std::vector<uint64_t>{3, 4, 5, 6}));
}

TEST(RawFtraceRingBufferTest, DiskOverflowRespectsCutoff) {
  RawFtraceRingBuffer buf(/*capacity_pages=*/2, kPageSize);
  ASSERT_TRUE(buf.EnableDiskOverflow(DiskTempPath(__LINE__),
                                     /*disk_capacity_pages=*/4));
  for (uint64_t ts : {10u, 20u, 30u, 40u, 50u})
    buf.PushPage(MakePage(static_cast<uint8_t>(ts)).data(), ts);

  std::vector<uint64_t> seen;
  buf.ForEachPageSince(
      30, [&](const uint8_t*, uint64_t ts) { seen.push_back(ts); });
  EXPECT_EQ(seen, (std::vector<uint64_t>{30, 40, 50}));  // Spans disk + mem.
}

}  // namespace
}  // namespace perfetto
