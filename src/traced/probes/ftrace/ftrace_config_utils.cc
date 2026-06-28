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

#include "src/traced/probes/ftrace/ftrace_config_utils.h"

#include <dirent.h>

#include <optional>
#include <unordered_set>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto {
namespace {

// Excludes \r, \n, escape sequences and control chars.
bool IsPrintableASCIIOrSpace(char c) {
  return c >= 32 && c < 127;
}

bool IsValidAtraceEventName(const std::string& str) {
  for (size_t i = 0; i < str.size(); i++) {
    if (!IsPrintableASCIIOrSpace(str[i]))
      return false;
  }
  return true;
}

bool IsValidFtraceEventName(const std::string& str) {
  int slash_count = 0;
  for (size_t i = 0; i < str.size(); i++) {
    if (!IsPrintableASCIIOrSpace(str[i]))
      return false;
    if (str[i] == '/') {
      slash_count++;
      // At most one '/' allowed and not at the beginning or end.
      if (slash_count > 1 || i == 0 || i == str.size() - 1)
        return false;
    }
    if (str[i] == '*' && i != str.size() - 1)
      return false;
  }
  return true;
}

}  // namespace

bool RequiresAtrace(const FtraceConfig& config) {
  return !config.atrace_categories().empty() || !config.atrace_apps().empty();
}

bool ValidConfig(const FtraceConfig& config) {
  for (const std::string& event_name : config.ftrace_events()) {
    if (!IsValidFtraceEventName(event_name)) {
      PERFETTO_ELOG("Bad event name '%s'", event_name.c_str());
      return false;
    }
  }
  for (const std::string& category : config.atrace_categories()) {
    if (!IsValidAtraceEventName(category)) {
      PERFETTO_ELOG("Bad category name '%s'", category.c_str());
      return false;
    }
  }
  for (const std::string& app : config.atrace_apps()) {
    if (!IsValidAtraceEventName(app)) {
      PERFETTO_ELOG("Bad app '%s'", app.c_str());
      return false;
    }
  }
  return true;
}

std::vector<std::string> ExpandPidToTids(uint32_t pid) {
  std::vector<std::string> tids;
  std::string path = "/proc/" + std::to_string(pid) + "/task";
  DIR* dir = opendir(path.c_str());
  if (!dir)
    return tids;
  while (struct dirent* ent = readdir(dir)) {
    if (ent->d_name[0] == '.')
      continue;
    std::optional<int32_t> tid = base::StringToInt32(ent->d_name);
    if (tid.has_value())
      tids.push_back(std::to_string(*tid));
  }
  closedir(dir);
  return tids;
}

std::vector<uint32_t> ParsePidFilterFile(const std::string& content) {
  std::vector<uint32_t> pids;
  std::unordered_set<uint32_t> seen;
  for (const std::string& raw : base::SplitString(content, "\n")) {
    std::string line = base::TrimWhitespace(raw);
    if (line.empty() || line[0] == '#')
      continue;
    std::optional<int32_t> pid = base::StringToInt32(line);
    if (pid.has_value() && *pid > 0 &&
        seen.insert(static_cast<uint32_t>(*pid)).second)
      pids.push_back(static_cast<uint32_t>(*pid));
  }
  return pids;
}

}  // namespace perfetto
