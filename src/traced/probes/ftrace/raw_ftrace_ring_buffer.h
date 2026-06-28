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

#ifndef SRC_TRACED_PROBES_FTRACE_RAW_FTRACE_RING_BUFFER_H_
#define SRC_TRACED_PROBES_FTRACE_RAW_FTRACE_RING_BUFFER_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/base/scoped_file.h"

namespace perfetto {

// Per-cpu in-memory ring buffer holding *unparsed* raw ftrace pages during
// steady-state collection. Pages are parsed only when a snapshot is triggered
// (deferred-raw capture), which keeps resident CPU low (no steady-state parse).
// Fixed capacity; pushing into a full buffer overwrites the oldest page.
//
// Not thread-safe by design: in traced_probes both the ftrace read path (which
// pushes) and the flush/clone path (which reads) run on the same task runner
// thread.
class RawFtraceRingBuffer {
 public:
  RawFtraceRingBuffer(size_t capacity_pages, size_t page_size);
  ~RawFtraceRingBuffer();

  RawFtraceRingBuffer(const RawFtraceRingBuffer&) = delete;
  RawFtraceRingBuffer& operator=(const RawFtraceRingBuffer&) = delete;

  // Copies |page_size| bytes from |page| into the buffer, tagged with
  // |page_ts| (the page's base timestamp, used for time-windowed retrieval).
  void PushPage(const uint8_t* page, uint64_t page_ts);

  // Enables an on-disk overflow ring at |path| holding up to
  // |disk_capacity_pages| pages. Once enabled, pages evicted from the (newest)
  // memory ring are spilled to disk instead of dropped, extending the retained
  // window to (capacity_pages + disk_capacity_pages). Returns false if the
  // backing file could not be created/sized. Call once, before PushPage.
  bool EnableDiskOverflow(const std::string& path, size_t disk_capacity_pages);

  // Iterates retained pages with page_ts >= cutoff_ts, oldest-to-newest
  // (disk pages first, then memory pages).
  void ForEachPageSince(
      uint64_t cutoff_ts,
      const std::function<void(const uint8_t*, uint64_t)>& fn) const;

  size_t size() const { return count_; }
  size_t capacity_pages() const { return capacity_pages_; }
  void Clear() {
    count_ = 0;
    head_ = 0;
    disk_count_ = 0;
    disk_head_ = 0;
  }

 private:
  const uint8_t* SlotData(size_t slot) const {
    return reinterpret_cast<const uint8_t*>(storage_.Get()) + slot * page_size_;
  }
  uint8_t* SlotDataMut(size_t slot) {
    return reinterpret_cast<uint8_t*>(storage_.Get()) + slot * page_size_;
  }
  bool disk_enabled() const { return disk_capacity_pages_ > 0; }
  // Appends one page to the on-disk overflow ring (dropping its oldest if full).
  void DiskPush(const uint8_t* page, uint64_t page_ts);

  const size_t capacity_pages_;
  const size_t page_size_;
  base::PagedMemory storage_;      // capacity_pages_ * page_size_ bytes.
  std::vector<uint64_t> page_ts_;  // size capacity_pages_, indexed by slot.
  size_t head_ = 0;                // Slot index of the next write.
  size_t count_ = 0;               // Number of valid pages (<= capacity).

  // Optional on-disk overflow ring (older pages evicted from memory).
  base::ScopedFile disk_fd_;
  size_t disk_capacity_pages_ = 0;
  std::vector<uint64_t> disk_page_ts_;  // size disk_capacity_pages_.
  size_t disk_head_ = 0;
  size_t disk_count_ = 0;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_RAW_FTRACE_RING_BUFFER_H_
