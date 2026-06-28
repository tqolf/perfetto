/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_UTILS_H_
#define SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_UTILS_H_

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "protos/perfetto/config/ftrace/ftrace_config.gen.h"

namespace perfetto {

using FtraceConfig = protos::gen::FtraceConfig;

// 0 is invalid.
using FtraceConfigId = uint64_t;

// Returns true iff the config has any atrace categories or apps.
bool RequiresAtrace(const FtraceConfig&);

bool ValidConfig(const FtraceConfig& config);

// Expands a process id to all of its current thread ids (as decimal strings)
// by listing /proc/<pid>/task. Returns empty if the process is gone.
std::vector<std::string> ExpandPidToTids(uint32_t pid);

// Parses a dynamic pid-filter control file: the complete desired PID set, one
// PID per line. Empty lines and lines starting with '#' are ignored. Returns
// the deduplicated PID list (order preserved by first occurrence).
std::vector<uint32_t> ParsePidFilterFile(const std::string& content);

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FTRACE_CONFIG_UTILS_H_
