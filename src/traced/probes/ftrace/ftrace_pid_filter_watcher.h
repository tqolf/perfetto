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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_PID_FILTER_WATCHER_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_PID_FILTER_WATCHER_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "perfetto/ext/base/weak_ptr.h"

namespace perfetto {

namespace base {
class TaskRunner;
}

// Polls a dynamic pid-filter control file and rewrites the kernel
// set_event_pid filter live when the desired PID set changes. Decouples the
// mechanism (rewrite set_event_pid) from the interface (anyone — including an
// HTTP/gRPC service — just writes the control file). Dependencies are injected
// so the poll logic is unit-testable without a real file/tracefs/procfs.
class FtracePidFilterWatcher {
 public:
  // Reads the control file content into |*out|. Returns false if unavailable
  // (the last applied filter is then kept).
  using ReadFileFn = std::function<bool(std::string* out)>;
  // Expands a PID to its current thread ids (decimal strings).
  using ExpandFn = std::function<std::vector<std::string>(uint32_t pid)>;
  // Applies the given thread-id set to set_event_pid. Returns false on failure.
  using ApplyTidsFn = std::function<bool(const std::vector<std::string>&)>;

  FtracePidFilterWatcher(base::TaskRunner* task_runner,
                         std::string control_file,
                         std::vector<std::string> static_tids,
                         ReadFileFn read_file,
                         ExpandFn expand,
                         ApplyTidsFn apply_tids,
                         uint32_t poll_period_ms);
  ~FtracePidFilterWatcher();

  FtracePidFilterWatcher(const FtracePidFilterWatcher&) = delete;
  FtracePidFilterWatcher& operator=(const FtracePidFilterWatcher&) = delete;

  // Begins periodic polling on the task runner.
  void Start();

  // Runs one poll cycle: read the control file, recompute the (sorted, unique)
  // TID set from static TIDs + expanded PIDs, and apply it if it differs from
  // the last applied set. Returns true iff it applied a change. Public so tests
  // can drive it directly without the task runner.
  bool PollOnce();

 private:
  void SchedulePoll();

  base::TaskRunner* const task_runner_;
  const std::string control_file_;
  const std::vector<std::string> static_tids_;
  ReadFileFn read_file_;
  ExpandFn expand_;
  ApplyTidsFn apply_tids_;
  const uint32_t poll_period_ms_;
  std::vector<std::string> last_applied_;
  bool started_ = false;
  base::WeakPtrFactory<FtracePidFilterWatcher> weak_factory_;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_PID_FILTER_WATCHER_H_
