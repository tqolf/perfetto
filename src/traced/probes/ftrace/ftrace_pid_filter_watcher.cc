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

#include <algorithm>
#include <utility>

#include "perfetto/base/task_runner.h"
#include "src/traced/probes/ftrace/ftrace_config_utils.h"

namespace perfetto {

FtracePidFilterWatcher::FtracePidFilterWatcher(
    base::TaskRunner* task_runner,
    std::string control_file,
    std::vector<std::string> static_tids,
    ReadFileFn read_file,
    ExpandFn expand,
    ApplyTidsFn apply_tids,
    uint32_t poll_period_ms)
    : task_runner_(task_runner),
      control_file_(std::move(control_file)),
      static_tids_(std::move(static_tids)),
      read_file_(std::move(read_file)),
      expand_(std::move(expand)),
      apply_tids_(std::move(apply_tids)),
      poll_period_ms_(poll_period_ms),
      weak_factory_(this) {}

FtracePidFilterWatcher::~FtracePidFilterWatcher() = default;

void FtracePidFilterWatcher::Start() {
  if (started_)
    return;
  started_ = true;
  SchedulePoll();
}

void FtracePidFilterWatcher::SchedulePoll() {
  auto weak_this = weak_factory_.GetWeakPtr();
  task_runner_->PostDelayedTask(
      [weak_this] {
        if (!weak_this)
          return;
        weak_this->PollOnce();
        weak_this->SchedulePoll();
      },
      poll_period_ms_);
}

bool FtracePidFilterWatcher::PollOnce() {
  std::string content;
  if (!read_file_(&content))
    return false;  // File unavailable: keep the last applied filter.

  std::vector<std::string> tids = static_tids_;
  for (uint32_t pid : ParsePidFilterFile(content)) {
    std::vector<std::string> expanded = expand_(pid);
    tids.insert(tids.end(), expanded.begin(), expanded.end());
  }
  std::sort(tids.begin(), tids.end());
  tids.erase(std::unique(tids.begin(), tids.end()), tids.end());

  if (tids == last_applied_)
    return false;
  apply_tids_(tids);
  last_applied_ = std::move(tids);
  return true;
}

}  // namespace perfetto
