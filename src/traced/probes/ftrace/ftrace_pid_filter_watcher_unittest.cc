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

#include "src/traced/probes/ftrace/ftrace_pid_filter_watcher.h"

#include <string>
#include <vector>

#include "src/traced/probes/ftrace/ftrace_config_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

TEST(FtracePidFilterWatcherTest, ParsePidFilterFile) {
  EXPECT_EQ(ParsePidFilterFile("1\n2\n3\n"), (std::vector<uint32_t>{1, 2, 3}));
  // Trim, blank lines, '#' comments, dedup (first occurrence order).
  EXPECT_EQ(ParsePidFilterFile("# header\n10\n\n  20  \n10\n"),
            (std::vector<uint32_t>{10, 20}));
  EXPECT_TRUE(ParsePidFilterFile("\n# only comments\n").empty());
  // Non-numeric and non-positive ignored.
  EXPECT_EQ(ParsePidFilterFile("abc\n0\n-5\n7\n"), (std::vector<uint32_t>{7}));
}

// Fake expansion: pid -> {pid, pid+1} as strings.
std::vector<std::string> FakeExpand(uint32_t pid) {
  return {std::to_string(pid), std::to_string(pid + 1)};
}

TEST(FtracePidFilterWatcherTest, AppliesOnChangeOnly) {
  std::string file = "100\n";
  std::vector<std::vector<std::string>> applied;

  FtracePidFilterWatcher w(
      /*task_runner=*/nullptr, "/ignored", /*static_tids=*/{"5"},
      [&](std::string* out) {
        *out = file;
        return true;
      },
      &FakeExpand,
      [&](const std::vector<std::string>& tids) {
        applied.push_back(tids);
        return true;
      },
      /*poll_period_ms=*/1000);

  // First poll applies the union {5} ∪ {100,101}, sorted+unique (string order).
  EXPECT_TRUE(w.PollOnce());
  ASSERT_EQ(applied.size(), 1u);
  EXPECT_EQ(applied[0], (std::vector<std::string>{"100", "101", "5"}));

  // Unchanged content: no re-apply.
  EXPECT_FALSE(w.PollOnce());
  EXPECT_EQ(applied.size(), 1u);

  // Adding a PID changes the set: re-apply.
  file = "100\n200\n";
  EXPECT_TRUE(w.PollOnce());
  ASSERT_EQ(applied.size(), 2u);
  EXPECT_EQ(applied[1],
            (std::vector<std::string>{"100", "101", "200", "201", "5"}));

  // Removing back to original re-applies the smaller set.
  file = "100\n";
  EXPECT_TRUE(w.PollOnce());
  ASSERT_EQ(applied.size(), 3u);
  EXPECT_EQ(applied[2], (std::vector<std::string>{"100", "101", "5"}));
}

TEST(FtracePidFilterWatcherTest, MissingFileKeepsLastFilter) {
  bool readable = true;
  std::string file = "100\n";
  int applies = 0;
  FtracePidFilterWatcher w(
      nullptr, "/ignored", {},
      [&](std::string* out) {
        if (!readable)
          return false;
        *out = file;
        return true;
      },
      &FakeExpand,
      [&](const std::vector<std::string>&) {
        applies++;
        return true;
      },
      1000);

  EXPECT_TRUE(w.PollOnce());
  EXPECT_EQ(applies, 1);
  // File becomes unreadable: poll is a no-op, last filter kept.
  readable = false;
  EXPECT_FALSE(w.PollOnce());
  EXPECT_EQ(applies, 1);
}

}  // namespace
}  // namespace perfetto
