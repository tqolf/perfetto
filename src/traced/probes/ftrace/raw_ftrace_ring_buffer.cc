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

#include <cstring>

#include "perfetto/base/logging.h"

namespace perfetto {

RawFtraceRingBuffer::RawFtraceRingBuffer(size_t capacity_pages,
                                         size_t page_size)
    : capacity_pages_(capacity_pages),
      page_size_(page_size),
      storage_(base::PagedMemory::Allocate(capacity_pages * page_size)),
      page_ts_(capacity_pages, 0) {
  PERFETTO_CHECK(capacity_pages_ > 0);
  PERFETTO_CHECK(page_size_ > 0);
}

RawFtraceRingBuffer::~RawFtraceRingBuffer() = default;

void RawFtraceRingBuffer::PushPage(const uint8_t* page, uint64_t page_ts) {
  memcpy(SlotDataMut(head_), page, page_size_);
  page_ts_[head_] = page_ts;
  head_ = (head_ + 1) % capacity_pages_;
  if (count_ < capacity_pages_)
    count_++;
}

void RawFtraceRingBuffer::ForEachPageSince(
    uint64_t cutoff_ts,
    const std::function<void(const uint8_t*, uint64_t)>& fn) const {
  // Oldest valid slot is head_ - count_ (mod capacity).
  size_t oldest = (head_ + capacity_pages_ - count_) % capacity_pages_;
  for (size_t i = 0; i < count_; i++) {
    size_t slot = (oldest + i) % capacity_pages_;
    if (page_ts_[slot] >= cutoff_ts)
      fn(SlotData(slot), page_ts_[slot]);
  }
}

}  // namespace perfetto
