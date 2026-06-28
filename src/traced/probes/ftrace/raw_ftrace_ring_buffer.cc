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

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/utils.h"

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

bool RawFtraceRingBuffer::EnableDiskOverflow(const std::string& path,
                                             size_t disk_capacity_pages) {
  if (disk_capacity_pages == 0)
    return false;
  base::ScopedFile fd = base::OpenFile(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (!fd)
    return false;
  if (ftruncate(*fd, static_cast<off_t>(disk_capacity_pages) *
                         static_cast<off_t>(page_size_)) != 0)
    return false;
  // Open-then-unlink: the file is never reopened (this object owns the only
  // fd), so unlinking now prevents leaking the raw-data file on disk and lets
  // the kernel reclaim it automatically on close/exit.
  unlink(path.c_str());
  disk_fd_ = std::move(fd);
  disk_capacity_pages_ = disk_capacity_pages;
  disk_page_ts_.assign(disk_capacity_pages, 0);
  disk_head_ = 0;
  disk_count_ = 0;
  disk_ok_ = true;
  return true;
}

void RawFtraceRingBuffer::DiskPush(const uint8_t* page, uint64_t page_ts) {
  off_t offset =
      static_cast<off_t>(disk_head_) * static_cast<off_t>(page_size_);
  ssize_t w = PERFETTO_EINTR(pwrite(*disk_fd_, page, page_size_, offset));
  if (w != static_cast<ssize_t>(page_size_)) {
    // Disk full / IO error: degrade to memory-only instead of crashing the
    // daemon (disk-full is an expected condition for an overflow feature).
    PERFETTO_PLOG(
        "deferred-raw: disk spill write failed, disabling disk overflow");
    disk_ok_ = false;
    return;
  }
  disk_page_ts_[disk_head_] = page_ts;
  disk_head_ = (disk_head_ + 1) % disk_capacity_pages_;
  if (disk_count_ < disk_capacity_pages_)
    disk_count_++;
}

void RawFtraceRingBuffer::PushPage(const uint8_t* page, uint64_t page_ts) {
  // When the memory ring is full, the slot at head_ holds the oldest page.
  // With disk overflow enabled, spill it to disk instead of dropping it.
  if (count_ == capacity_pages_ && disk_enabled())
    DiskPush(SlotData(head_), page_ts_[head_]);
  memcpy(SlotDataMut(head_), page, page_size_);
  page_ts_[head_] = page_ts;
  head_ = (head_ + 1) % capacity_pages_;
  if (count_ < capacity_pages_)
    count_++;
}

void RawFtraceRingBuffer::ForEachPageSince(
    uint64_t cutoff_ts,
    const std::function<void(const uint8_t*, uint64_t)>& fn) const {
  // Disk pages are strictly older than memory pages; emit them first,
  // oldest-to-newest, reading each back into a scratch buffer.
  if (disk_count_ > 0) {
    std::vector<uint8_t> scratch(page_size_);
    size_t disk_oldest = (disk_head_ + disk_capacity_pages_ - disk_count_) %
                         disk_capacity_pages_;
    for (size_t i = 0; i < disk_count_; i++) {
      size_t slot = (disk_oldest + i) % disk_capacity_pages_;
      if (disk_page_ts_[slot] < cutoff_ts)
        continue;
      off_t offset = static_cast<off_t>(slot) * static_cast<off_t>(page_size_);
      ssize_t r =
          PERFETTO_EINTR(pread(*disk_fd_, scratch.data(), page_size_, offset));
      if (r != static_cast<ssize_t>(page_size_)) {
        PERFETTO_PLOG("deferred-raw: disk read-back failed, skipping page");
        continue;  // Skip the unreadable page rather than crash.
      }
      fn(scratch.data(), disk_page_ts_[slot]);
    }
  }
  // Then the (newest) memory pages, oldest-to-newest.
  size_t oldest = (head_ + capacity_pages_ - count_) % capacity_pages_;
  for (size_t i = 0; i < count_; i++) {
    size_t slot = (oldest + i) % capacity_pages_;
    if (page_ts_[slot] >= cutoff_ts)
      fn(SlotData(slot), page_ts_[slot]);
  }
}

}  // namespace perfetto
