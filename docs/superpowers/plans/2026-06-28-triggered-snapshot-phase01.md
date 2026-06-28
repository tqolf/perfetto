# 触发式快照（阶段 0 + 阶段 1）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 traced_probes 在常驻期把全量 ftrace 原始页缓存进每 CPU 内存环形缓冲（跳过解析），触发时才解析落盘"前 N 秒 + 后 M 秒"，从而降低常驻 CPU/IO，并以可配上限管控内存。

**Architecture:** 复用原生 `CLONE_SNAPSHOT` 触发与 clone-flush 流程；唯一改造点是给 ftrace 路径加一条 "deferred-raw" 旁路：在 `CpuReader::ReadAndProcessBatch` 的 read 之后、parse 之前，把原始页 memcpy 进新类 `RawFtraceRingBuffer`，触发时由 clone-flush 回调把缓冲页用现有 `ParsePagePayload` 解析进 `TraceWriter`。

**Tech Stack:** C++17，GN/Ninja，GoogleTest（`perfetto_unittests` / `perfetto_integrationtests`），protozero，Perfetto base（`PagedMemory`、`FlatSet`）。

## Global Constraints

- C++17，禁用异常，遵循 Google C++ style（见 CLAUDE.md）。
- 失败快：用 `PERFETTO_CHECK`（生产）/ `PERFETTO_DCHECK`（仅 debug，禁放副作用代码）。
- out 参数用指针不用引用；删除拷贝/移动构造除非确需。
- 不新增第三方库；优先用 `include/perfetto/ext/base/` 既有设施。
- 改了 `.gn`/`.gni` 后必须 `tools/gn gen --check out/linux_clang_release`。
- 改了 `.proto` 后需重新生成（构建会带；如需手动见 tools/）。
- 构建前先确定 OUT 目录：`echo $OUT`，空则 `ls -t1 out | head -n1`；本 plan 命令统一用 `out/linux_clang_release`，执行者按实际替换。
- ftrace 解析路径为静态函数 `CpuReader::ParsePagePayload` / `ProcessPagesForDataSource`，复用它们，不重写解析器。
- proto 新字段：`FtraceConfig` 当前最大已用字段号 37（`tracing_cpumask`），新字段从 38 起。

---

## File Structure

- `src/traced/probes/ftrace/raw_ftrace_ring_buffer.h`（新）— `RawFtraceRingBuffer` 类声明：每 CPU 一个，存整页 + 页时间戳的内存环形缓冲，固定容量、满则覆盖最旧、支持按 cutoff 时间戳遍历。单一职责，可独立单元测试。
- `src/traced/probes/ftrace/raw_ftrace_ring_buffer.cc`（新）— 实现。
- `src/traced/probes/ftrace/raw_ftrace_ring_buffer_unittest.cc`（新）— 单元测试。
- `protos/perfetto/config/ftrace/ftrace_config.proto`（改）— 新增 `DeferredRawCapture` 子消息 + 字段 38。
- `src/traced/probes/ftrace/ftrace_config_muxer.h`（改）— `FtraceDataSourceConfig` 增加 deferred-raw 解析后的配置。
- `src/traced/probes/ftrace/ftrace_config_muxer.cc`（改）— 把 proto 字段读进 `FtraceDataSourceConfig`。
- `src/traced/probes/ftrace/cpu_reader.h` / `.cc`（改）— `ReadAndProcessBatch` 注入 deferred-raw 旁路；新增触发时解析入口 `ParseRawRingBufferInto(...)`。
- `src/traced/probes/ftrace/ftrace_data_source.h` / `.cc`（改）— 持有每 CPU `RawFtraceRingBuffer`；clone-flush 时触发解析。
- `src/traced/probes/probes_producer.cc`（改）— `Flush` 中识别 `Reason::kTraceClone` 时驱动 ftrace data source 解析 raw 缓冲。
- `src/traced/probes/ftrace/BUILD.gn`（改）— 加入新源文件与单测。
- `test/configs/`（新 textproto）+ `tools/`（新压测脚本）— 阶段 0 基线。

---

## 阶段 0：原生 CLONE_SNAPSHOT 基线 + 压测框架

### Task 0: 用纯配置跑通"前N秒+后M秒"并建立 CPU 基线

**Files:**
- Create: `test/configs/triggered_snapshot_baseline.cfg`（textproto trace config）
- Create: `tools/measure_traced_probes_cpu.sh`（采样 traced_probes CPU 占用）

**Interfaces:**
- Produces: 一个可复现的基线测量脚本与配置，后续阶段 1 用同一脚本对比"常驻 CPU 下降"。

- [ ] **Step 1: 写基线 trace config（环形 + CLONE_SNAPSHOT + stop_delay=M）**

`test/configs/triggered_snapshot_baseline.cfg`:
```
# 常态化 ring buffer，触发时 clone 前N秒、再续录后M秒（M 由 stop_delay_ms 决定）。
buffers {
  size_kb: 65536          # 前N秒数据量上限（按事件速率调整）
  fill_policy: RING_BUFFER
}
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
      ftrace_events: "irq/irq_handler_entry"
      ftrace_events: "irq/irq_handler_exit"
    }
  }
}
trigger_config {
  trigger_mode: CLONE_SNAPSHOT
  trigger_timeout_ms: 3600000
  triggers {
    name: "snap"
    stop_delay_ms: 2000     # 后 M 秒 = 2s
  }
}
```

- [ ] **Step 2: 跑通端到端，确认触发产出快照**

Run:
```bash
out/linux_clang_release/perfetto -c test/configs/triggered_snapshot_baseline.cfg --txt -o /tmp/snap.pftrace &
sleep 5
out/linux_clang_release/perfetto --trigger snap
sleep 3
```
Expected: 生成 `/tmp/snap.pftrace.0`（或带序号），且原会话仍在运行。

- [ ] **Step 3: 写 CPU 采样脚本**

`tools/measure_traced_probes_cpu.sh`:
```bash
#!/usr/bin/env bash
# 采样 traced_probes 在 DURATION 秒内的平均 CPU%（常驻期，无触发）。
set -euo pipefail
DURATION="${1:-30}"
PID="$(pgrep -x traced_probes | head -n1)"
[ -z "$PID" ] && { echo "traced_probes not running"; exit 1; }
T0=$(awk '{print $14+$15}' /proc/$PID/stat)
sleep "$DURATION"
T1=$(awk '{print $14+$15}' /proc/$PID/stat)
HZ=$(getconf CLK_TCK)
echo "traced_probes CPU%% over ${DURATION}s: $(awk -v d="$DURATION" -v hz="$HZ" -v a="$T0" -v b="$T1" 'BEGIN{printf "%.2f", (b-a)/hz/d*100}')"
```

- [ ] **Step 4: 记录基线数值**

Run: `chmod +x tools/measure_traced_probes_cpu.sh && ./tools/measure_traced_probes_cpu.sh 30`
Expected: 打印常驻 CPU%（原生解析路径）。记录此值作为阶段 1 对照基线。

- [ ] **Step 5: Commit**

```bash
git add test/configs/triggered_snapshot_baseline.cfg tools/measure_traced_probes_cpu.sh
git commit -m "perfetto: add CLONE_SNAPSHOT baseline config and traced_probes CPU probe"
```

---

## 阶段 1：deferred-raw 内存环形缓冲 + 触发时解析

### Task 1: RawFtraceRingBuffer（内存段，纯单元）

**Files:**
- Create: `src/traced/probes/ftrace/raw_ftrace_ring_buffer.h`
- Create: `src/traced/probes/ftrace/raw_ftrace_ring_buffer.cc`
- Test: `src/traced/probes/ftrace/raw_ftrace_ring_buffer_unittest.cc`
- Modify: `src/traced/probes/ftrace/BUILD.gn`

**Interfaces:**
- Produces:
  - `class RawFtraceRingBuffer`
  - `RawFtraceRingBuffer(size_t capacity_pages, size_t page_size)`
  - `void PushPage(const uint8_t* page, uint64_t page_ts)` — 拷贝 `page_size` 字节整页 + 页时间戳；满则覆盖最旧。
  - `size_t size() const` / `size_t capacity_pages() const`
  - `void ForEachPageSince(uint64_t cutoff_ts, const std::function<void(const uint8_t*, uint64_t)>& fn) const` — 按从旧到新顺序遍历 `page_ts >= cutoff_ts` 的页。
  - `void Clear()`

- [ ] **Step 1: 写失败测试**

`src/traced/probes/ftrace/raw_ftrace_ring_buffer_unittest.cc`:
```cpp
#include "src/traced/probes/ftrace/raw_ftrace_ring_buffer.h"

#include <cstring>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

constexpr size_t kPageSize = 4096;

std::vector<uint8_t> MakePage(uint8_t fill) {
  return std::vector<uint8_t>(kPageSize, fill);
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
  buf.PushPage(MakePage(30).data(), 30);  // 覆盖 ts=10
  EXPECT_EQ(buf.size(), 2u);

  std::vector<uint64_t> seen;
  buf.ForEachPageSince(0, [&](const uint8_t* p, uint64_t ts) {
    seen.push_back(ts);
    EXPECT_EQ(p[0], static_cast<uint8_t>(ts));  // 内容随 ts
  });
  EXPECT_EQ(seen, (std::vector<uint64_t>{20, 30}));  // 从旧到新，10 被挤掉
}

TEST(RawFtraceRingBufferTest, ForEachPageSinceFiltersByCutoff) {
  RawFtraceRingBuffer buf(8, kPageSize);
  for (uint64_t ts : {5u, 15u, 25u, 35u})
    buf.PushPage(MakePage(static_cast<uint8_t>(ts)).data(), ts);

  std::vector<uint64_t> seen;
  buf.ForEachPageSince(20, [&](const uint8_t*, uint64_t ts) {
    seen.push_back(ts);
  });
  EXPECT_EQ(seen, (std::vector<uint64_t>{25, 35}));
}

TEST(RawFtraceRingBufferTest, ClearResetsSize) {
  RawFtraceRingBuffer buf(4, kPageSize);
  buf.PushPage(MakePage(1).data(), 1);
  buf.Clear();
  EXPECT_EQ(buf.size(), 0u);
}

}  // namespace
}  // namespace perfetto
```

- [ ] **Step 2: 加进 BUILD.gn 并确认编译失败（类不存在）**

在 `src/traced/probes/ftrace/BUILD.gn` 的 ftrace source_set 的 `sources` 加入：
```
    "raw_ftrace_ring_buffer.cc",
    "raw_ftrace_ring_buffer.h",
```
在该目录的 unittest source_set（搜 `_unittest.cc` 所在 target）的 `sources` 加入：
```
    "raw_ftrace_ring_buffer_unittest.cc",
```

Run:
```bash
tools/gn gen --check out/linux_clang_release
tools/ninja -C out/linux_clang_release perfetto_unittests 2>&1 | tail -5
```
Expected: 编译失败，提示 `raw_ftrace_ring_buffer.h` not found / 未定义 `RawFtraceRingBuffer`。

- [ ] **Step 3: 写头文件**

`src/traced/probes/ftrace/raw_ftrace_ring_buffer.h`:
```cpp
#ifndef SRC_TRACED_PROBES_FTRACE_RAW_FTRACE_RING_BUFFER_H_
#define SRC_TRACED_PROBES_FTRACE_RAW_FTRACE_RING_BUFFER_H_

#include <cstdint>
#include <functional>
#include <vector>

#include "perfetto/ext/base/paged_memory.h"

namespace perfetto {

// Per-cpu in-memory ring buffer holding *unparsed* raw ftrace pages during
// steady-state collection. Pages are parsed only when a snapshot is triggered.
// Fixed capacity; pushing into a full buffer overwrites the oldest page.
class RawFtraceRingBuffer {
 public:
  RawFtraceRingBuffer(size_t capacity_pages, size_t page_size);
  ~RawFtraceRingBuffer();

  RawFtraceRingBuffer(const RawFtraceRingBuffer&) = delete;
  RawFtraceRingBuffer& operator=(const RawFtraceRingBuffer&) = delete;

  // Copies |page_size| bytes from |page| into the buffer, tagged with
  // |page_ts| (the page's timestamp, used for time-windowed retrieval).
  void PushPage(const uint8_t* page, uint64_t page_ts);

  // Iterates retained pages with page_ts >= cutoff_ts, oldest-to-newest.
  void ForEachPageSince(
      uint64_t cutoff_ts,
      const std::function<void(const uint8_t*, uint64_t)>& fn) const;

  size_t size() const { return count_; }
  size_t capacity_pages() const { return capacity_pages_; }
  void Clear() { count_ = 0; head_ = 0; }

 private:
  const uint8_t* SlotData(size_t slot) const {
    return reinterpret_cast<const uint8_t*>(storage_.Get()) +
           slot * page_size_;
  }
  uint8_t* SlotDataMut(size_t slot) {
    return reinterpret_cast<uint8_t*>(storage_.Get()) + slot * page_size_;
  }

  const size_t capacity_pages_;
  const size_t page_size_;
  base::PagedMemory storage_;          // capacity_pages_ * page_size_ bytes
  std::vector<uint64_t> page_ts_;      // size capacity_pages_, indexed by slot
  size_t head_ = 0;                    // slot index of next write
  size_t count_ = 0;                   // number of valid pages (<= capacity)
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_RAW_FTRACE_RING_BUFFER_H_
```

- [ ] **Step 4: 写实现**

`src/traced/probes/ftrace/raw_ftrace_ring_buffer.cc`:
```cpp
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
```

- [ ] **Step 5: 编译并通过单测**

Run:
```bash
tools/ninja -C out/linux_clang_release perfetto_unittests
out/linux_clang_release/perfetto_unittests --gtest_brief=1 --gtest_filter="RawFtraceRingBufferTest.*"
```
Expected: 5 个测试全部 PASS。

- [ ] **Step 6: Commit**

```bash
git add src/traced/probes/ftrace/raw_ftrace_ring_buffer.h src/traced/probes/ftrace/raw_ftrace_ring_buffer.cc src/traced/probes/ftrace/raw_ftrace_ring_buffer_unittest.cc src/traced/probes/ftrace/BUILD.gn
git commit -m "perfetto: add RawFtraceRingBuffer for deferred-raw ftrace capture"
```

---

### Task 2: proto 扩展 DeferredRawCapture + muxer 解析

**Files:**
- Modify: `protos/perfetto/config/ftrace/ftrace_config.proto`（字段 38）
- Modify: `src/traced/probes/ftrace/ftrace_config_muxer.h:50`（`FtraceDataSourceConfig`）
- Modify: `src/traced/probes/ftrace/ftrace_config_muxer.cc`

**Interfaces:**
- Consumes: 无（第一处使用配置）。
- Produces: `FtraceDataSourceConfig` 新增成员
  - `bool deferred_raw_enabled = false;`
  - `uint32_t deferred_raw_per_cpu_mem_limit_kb = 0;`
  - `uint32_t deferred_raw_retain_seconds = 0;`

- [ ] **Step 1: 加 proto 消息与字段**

在 `protos/perfetto/config/ftrace/ftrace_config.proto` 的 `message FtraceConfig {` 末尾（`reserved 26;` 之前）加：
```
  // Deferred-raw capture: during steady state, traced_probes copies raw ftrace
  // pages into a per-cpu in-memory ring buffer WITHOUT parsing them, and parses
  // them into the trace only when a CLONE_SNAPSHOT trigger fires. Cuts resident
  // CPU (no steady-state parse) and IO (no steady-state writeback).
  message DeferredRawCapture {
    optional bool enabled = 1;
    // Per-cpu in-memory ring buffer cap. Memory ~= this * num_cpus.
    optional uint32 per_cpu_mem_limit_kb = 2;
    // Target retained window N (seconds) before the trigger. Best-effort:
    // also bounded by per_cpu_mem_limit_kb.
    optional uint32 retain_seconds = 3;
  }
  optional DeferredRawCapture deferred_raw_capture = 38;
```

- [ ] **Step 2: 在 FtraceDataSourceConfig 加字段**

在 `src/traced/probes/ftrace/ftrace_config_muxer.h` 的 `struct FtraceDataSourceConfig { ... }` 内加：
```cpp
  // Deferred-raw capture (see FtraceConfig.DeferredRawCapture).
  bool deferred_raw_enabled = false;
  uint32_t deferred_raw_per_cpu_mem_limit_kb = 0;
  uint32_t deferred_raw_retain_seconds = 0;
```

- [ ] **Step 3: 写失败测试（muxer 解析配置）**

在 `src/traced/probes/ftrace/ftrace_config_muxer_unittest.cc` 加：
```cpp
TEST_F(FtraceConfigMuxerTest, DeferredRawCaptureParsed) {
  FtraceConfig config = CreateFtraceConfig({"sched/sched_switch"});
  config.mutable_deferred_raw_capture()->set_enabled(true);
  config.mutable_deferred_raw_capture()->set_per_cpu_mem_limit_kb(8192);
  config.mutable_deferred_raw_capture()->set_retain_seconds(10);

  FtraceConfigMuxer muxer(&tracefs_, &procfs_, table_.get(), GetSyscallTable(),
                          {});
  FtraceConfigId id = muxer.SetupConfig(/*session_id=*/1, config);
  ASSERT_TRUE(id);
  const FtraceDataSourceConfig* ds = muxer.GetDataSourceConfig(id);
  ASSERT_NE(ds, nullptr);
  EXPECT_TRUE(ds->deferred_raw_enabled);
  EXPECT_EQ(ds->deferred_raw_per_cpu_mem_limit_kb, 8192u);
  EXPECT_EQ(ds->deferred_raw_retain_seconds, 10u);
}
```
> 注：`CreateFtraceConfig` / `SetupConfig` / `GetDataSourceConfig` 是该测试文件既有 helper/接口；若签名不符，按文件内既有用法对齐（执行前 grep 确认）。

Run:
```bash
tools/ninja -C out/linux_clang_release perfetto_unittests 2>&1 | tail -5
```
Expected: 编译失败或测试失败（字段未被填充）。

- [ ] **Step 4: 在 muxer 填充字段**

在 `src/traced/probes/ftrace/ftrace_config_muxer.cc` 中 `SetupConfig` 构造 `FtraceDataSourceConfig`（搜 `FtraceDataSourceConfig` 的赋值/构造处）之后，加入：
```cpp
  if (request.deferred_raw_capture().enabled()) {
    ds_config.deferred_raw_enabled = true;
    ds_config.deferred_raw_per_cpu_mem_limit_kb =
        request.deferred_raw_capture().per_cpu_mem_limit_kb();
    ds_config.deferred_raw_retain_seconds =
        request.deferred_raw_capture().retain_seconds();
  }
```
> 注：`request` 与 `ds_config` 的实际变量名以该函数内既有命名为准（执行前读 ±30 行确认）。

- [ ] **Step 5: 编译并通过测试**

Run:
```bash
tools/ninja -C out/linux_clang_release perfetto_unittests
out/linux_clang_release/perfetto_unittests --gtest_brief=1 --gtest_filter="FtraceConfigMuxerTest.*"
```
Expected: 含 `DeferredRawCaptureParsed` 的 muxer 测试全部 PASS。

- [ ] **Step 6: Commit**

```bash
git add protos/perfetto/config/ftrace/ftrace_config.proto src/traced/probes/ftrace/ftrace_config_muxer.h src/traced/probes/ftrace/ftrace_config_muxer.cc src/traced/probes/ftrace/ftrace_config_muxer_unittest.cc
git commit -m "perfetto: add DeferredRawCapture ftrace config and muxer parsing"
```

---

### Task 3: deferred-raw 旁路注入 CpuReader

**Files:**
- Modify: `src/traced/probes/ftrace/cpu_reader.h`（新增触发解析入口声明）
- Modify: `src/traced/probes/ftrace/cpu_reader.cc:353-364`（read 后、parse 前注入旁路）
- Modify: `src/traced/probes/ftrace/ftrace_data_source.h` / `.cc`（持有每 CPU `RawFtraceRingBuffer`）

**Interfaces:**
- Consumes: `RawFtraceRingBuffer`（Task 1）；`FtraceDataSourceConfig::deferred_raw_*`（Task 2）；`CpuReader::ParsePageHeader`（既有，返回 `PageHeader{timestamp,size,lost_events}`）。
- Produces:
  - `FtraceDataSource`：`RawFtraceRingBuffer* raw_ring_buffer(size_t cpu)`（deferred 模式下非空，否则 nullptr）。
  - `CpuReader::ParseRawRingBufferInto(RawFtraceRingBuffer*, uint64_t cutoff_ts, TraceWriter*, FtraceMetadata*, base::FlatSet<protos::pbzero::FtraceParseStatus>*, const FtraceDataSourceConfig*)`（静态，Task 4 调用）。

- [ ] **Step 1: 在 FtraceDataSource 持有每 CPU raw 缓冲**

在 `src/traced/probes/ftrace/ftrace_data_source.h` 的 `FtraceDataSource` 私有区加：
```cpp
  // Non-empty only when deferred-raw capture is enabled. Indexed by cpu.
  std::vector<std::unique_ptr<RawFtraceRingBuffer>> raw_ring_buffers_;
```
public 区加：
```cpp
  RawFtraceRingBuffer* raw_ring_buffer(size_t cpu) {
    return cpu < raw_ring_buffers_.size() ? raw_ring_buffers_[cpu].get()
                                          : nullptr;
  }
  bool deferred_raw_enabled() const { return !raw_ring_buffers_.empty(); }
```
顶部 `#include "src/traced/probes/ftrace/raw_ftrace_ring_buffer.h"`。

在 `ftrace_data_source.cc` 的 `Start()`（data source 启动、cpu 数已知处，参考既有按 cpu 初始化代码）按 `parsing_config()->deferred_raw_enabled` 初始化：
```cpp
  if (parsing_config_->deferred_raw_enabled) {
    size_t num_cpus = /* 既有获取 cpu 数的途径 */;
    size_t page_size = base::GetSysPageSize();
    size_t cap_pages = std::max<size_t>(
        1, (parsing_config_->deferred_raw_per_cpu_mem_limit_kb * 1024) /
               page_size);
    raw_ring_buffers_.clear();
    for (size_t c = 0; c < num_cpus; c++) {
      raw_ring_buffers_.push_back(
          std::make_unique<RawFtraceRingBuffer>(cap_pages, page_size));
    }
  }
```
> 注：获取 cpu 数的途径以该文件既有用法为准（执行前确认）。

- [ ] **Step 2: 在 ReadAndProcessBatch 注入旁路**

修改 `src/traced/probes/ftrace/cpu_reader.cc` 中 `ReadAndProcessBatch` 末尾的 parse 循环（当前 `:357-363`）。改为：deferred 模式的 data source 把页 push 进 raw buffer 并跳过解析；其余照常解析：
```cpp
  const uint32_t sys_page_size = base::GetSysPageSize();
  for (FtraceDataSource* data_source : started_data_sources) {
    RawFtraceRingBuffer* raw = data_source->raw_ring_buffer(cpu_);
    if (raw) {
      // Deferred-raw: stash unparsed pages, parse later on trigger.
      for (size_t i = 0; i < pages_read; i++) {
        const uint8_t* page = parsing_buf + i * sys_page_size;
        const uint8_t* hdr_ptr = page;
        std::optional<PageHeader> hdr =
            ParsePageHeader(&hdr_ptr, table_->page_header_size_len());
        uint64_t page_ts = hdr.has_value() ? hdr->timestamp : 0;
        raw->PushPage(page, page_ts);
      }
      continue;
    }
    ProcessPagesForDataSource(
        data_source->trace_writer(), data_source->mutable_metadata(), cpu_,
        data_source->parsing_config(), data_source->mutable_parse_errors(),
        data_source->mutable_bundle_end_timestamp(cpu_), parsing_buf,
        pages_read, compact_sched_buf, table_, symbolizer_, clock_snapshot);
  }
  return pages_read;
```

- [ ] **Step 3: 声明并实现 ParseRawRingBufferInto（触发时解析）**

在 `cpu_reader.h` 的 public 静态方法区（紧邻 `ProcessPagesForDataSource`）加声明：
```cpp
  // Parses all pages in |raw| with page_ts >= cutoff_ts into |trace_writer|.
  // Used when a snapshot trigger fires for a deferred-raw data source.
  // page_size is the system page size used when pages were captured.
  static void ParseRawRingBufferInto(
      size_t cpu,
      size_t page_size,
      const RawFtraceRingBuffer* raw,
      uint64_t cutoff_ts,
      const ProtoTranslationTable* table,
      const FtraceDataSourceConfig* ds_config,
      TraceWriter* trace_writer,
      FtraceMetadata* metadata,
      base::FlatSet<protos::pbzero::FtraceParseStatus>* parse_errors,
      LazyKernelSymbolizer* symbolizer);
```
顶部前置声明加 `class RawFtraceRingBuffer;`。

在 `cpu_reader.cc` 实现（复用现有 `Bundler` + `ParsePagePayload`）：
```cpp
void CpuReader::ParseRawRingBufferInto(
    size_t cpu,
    size_t page_size,
    const RawFtraceRingBuffer* raw,
    uint64_t cutoff_ts,
    const ProtoTranslationTable* table,
    const FtraceDataSourceConfig* ds_config,
    TraceWriter* trace_writer,
    FtraceMetadata* metadata,
    base::FlatSet<protos::pbzero::FtraceParseStatus>* parse_errors,
    LazyKernelSymbolizer* symbolizer) {
  uint64_t bundle_end_ts = 0;
  Bundler bundler(trace_writer, metadata, symbolizer, cpu,
                  /*clock_snapshot=*/std::nullopt, /*compact_sched_buf=*/nullptr,
                  /*compact_sched_enabled=*/false, bundle_end_ts,
                  ds_config->generic_pb_descriptors());
  raw->ForEachPageSince(
      cutoff_ts, [&](const uint8_t* page, uint64_t) {
        const uint8_t* payload = page;
        std::optional<PageHeader> hdr =
            ParsePageHeader(&payload, table->page_header_size_len());
        if (!hdr.has_value())
          return;
        FtraceParseStatus st = ParsePagePayload(
            payload, &hdr.value(), table, ds_config, &bundler, metadata,
            &bundle_end_ts);
        if (st != FtraceParseStatus::FTRACE_STATUS_OK)
          parse_errors->insert(st);
      });
}
```
> 注：`Bundler` 构造参数列表以 `cpu_reader.h:127` 实际签名为准；若 `compact_sched_buf` 不允许 nullptr，则在调用处传入一个本地 `CompactSchedBuffer`。`ds_config->generic_pb_descriptors()` 的实际取法以 `FtraceDataSourceConfig` 既有成员为准（执行前确认）。compact_sched 在 deferred 路径先禁用，sched_switch 走普通事件路径，功能正确性优先；compact 优化留后续。

- [ ] **Step 4: 加进 BUILD.gn 依赖并编译**

确认 `raw_ftrace_ring_buffer` 已在 ftrace target（Task 1 已加）。
Run:
```bash
tools/gn gen --check out/linux_clang_release
tools/ninja -C out/linux_clang_release traced_probes perfetto_unittests 2>&1 | tail -10
```
Expected: 编译通过。

- [ ] **Step 5: 单测 — 旁路不丢页 + 触发解析等价**

在 `src/traced/probes/ftrace/cpu_reader_unittest.cc` 加（复用该文件既有的造页 helper，如 `FakeFtracePage`/`PageFromTrace` 之类；执行前 grep 既有 helper 名）：
```cpp
// 用既有 helper 造若干含 sched_switch 的页，先 PushPage 进 RawFtraceRingBuffer，
// 再 ParseRawRingBufferInto 解析，断言 TraceWriter 收到的事件数与直接
// ProcessPagesForDataSource 解析同样的页一致。
TEST(CpuReaderTest, DeferredRawParseEquivalentToImmediate) {
  // ... 见执行说明：构造与 cpu_reader_unittest.cc 既有 sched_switch 用例一致的页，
  // 对比两条路径的 bundle 事件数。
}
```
> 该测试需对齐本文件既有测试夹具，故此处给出意图与断言点；执行者按既有 sched_switch 解析用例（搜 `ProcessPagesForDataSource` 在测试中的调用）镜像实现。

Run:
```bash
tools/ninja -C out/linux_clang_release perfetto_unittests
out/linux_clang_release/perfetto_unittests --gtest_brief=1 --gtest_filter="CpuReaderTest.*"
```
Expected: 含新测试在内 CpuReaderTest 全部 PASS。

- [ ] **Step 6: Commit**

```bash
git add src/traced/probes/ftrace/cpu_reader.h src/traced/probes/ftrace/cpu_reader.cc src/traced/probes/ftrace/ftrace_data_source.h src/traced/probes/ftrace/ftrace_data_source.cc src/traced/probes/ftrace/cpu_reader_unittest.cc
git commit -m "perfetto: route deferred-raw ftrace pages through RawFtraceRingBuffer"
```

---

### Task 4: 触发时解析挂到 clone-flush

**Files:**
- Modify: `src/traced/probes/ftrace/ftrace_data_source.h` / `.cc`（新增 `OnCloneSnapshot()`）
- Modify: `src/traced/probes/probes_producer.cc:641`（`Flush` 识别 `kTraceClone`）

**Interfaces:**
- Consumes: `FtraceDataSource::raw_ring_buffer(cpu)`、`CpuReader::ParseRawRingBufferInto`（Task 3）；`FlushFlags::Reason::kTraceClone`。
- Produces: `void FtraceDataSource::OnCloneSnapshot()` — 对每 CPU raw 缓冲调用解析，写入本 data source 的 `trace_writer()`。

- [ ] **Step 1: 实现 FtraceDataSource::OnCloneSnapshot**

`ftrace_data_source.h` public 加 `void OnCloneSnapshot();`。
`ftrace_data_source.cc` 实现：
```cpp
void FtraceDataSource::OnCloneSnapshot() {
  if (!deferred_raw_enabled())
    return;
  size_t page_size = base::GetSysPageSize();
  // cutoff = now - N 秒；用 boottime ns（与 ftrace boot clock 对齐）。
  uint64_t now_ns =
      static_cast<uint64_t>(base::GetBootTimeNs().count());
  uint64_t window_ns =
      static_cast<uint64_t>(parsing_config_->deferred_raw_retain_seconds) *
      1000000000ull;
  uint64_t cutoff_ts = window_ns < now_ns ? now_ns - window_ns : 0;
  for (size_t cpu = 0; cpu < raw_ring_buffers_.size(); cpu++) {
    CpuReader::ParseRawRingBufferInto(
        cpu, page_size, raw_ring_buffers_[cpu].get(), cutoff_ts,
        table_ /* ProtoTranslationTable* */, parsing_config_,
        writer_.get(), &metadata_, &parse_errors_, symbolizer_ /* 既有 */);
  }
}
```
> 注：`table_` / `symbolizer_` / `metadata_` / `parse_errors_` 的实际成员名以 `FtraceDataSource` 既有定义为准（执行前确认，可能需经 `mutable_metadata()` 等访问器）。

- [ ] **Step 2: 在 ProbesProducer::Flush 识别 clone 并驱动**

`src/traced/probes/probes_producer.cc` 的 `Flush`（`:641`，参数当前未命名）改为接收 flush flags 并在 clone 时调用：
```cpp
void ProbesProducer::Flush(FlushRequestID flush_request_id,
                           const DataSourceInstanceID* data_source_ids,
                           size_t num_data_sources,
                           FlushFlags flush_flags) {
  for (size_t i = 0; i < num_data_sources; i++) {
    auto it = data_sources_.find(data_source_ids[i]);
    if (it == data_sources_.end())
      continue;
    if (flush_flags.reason() == FlushFlags::Reason::kTraceClone) {
      if (auto* ftrace = it->second->GetFtraceForCloneOrNull())
        ftrace->OnCloneSnapshot();
    }
    it->second->Flush(flush_request_id, /* ... 既有逻辑 ... */);
  }
  // ... 既有 ack 逻辑 ...
}
```
> 注：`Flush` 的真实签名、`data_sources_` 容器、以及如何从 `ProbesDataSource*` 取到 `FtraceDataSource*`（可能需在 `ProbesDataSource` 加虚函数 `GetFtraceForCloneOrNull()` 默认返回 nullptr，`FtraceDataSource` override 返回 this）以既有代码为准。保持原有 flush ack 行为不变，仅在 clone 前插入 `OnCloneSnapshot`。

- [ ] **Step 3: 编译**

Run:
```bash
tools/ninja -C out/linux_clang_release traced_probes 2>&1 | tail -10
```
Expected: 编译通过。

- [ ] **Step 4: Commit**

```bash
git add src/traced/probes/ftrace/ftrace_data_source.h src/traced/probes/ftrace/ftrace_data_source.cc src/traced/probes/probes_producer.cc src/traced/probes/ftrace/ftrace_data_source.h
git commit -m "perfetto: parse deferred-raw ftrace buffer on CLONE_SNAPSHOT flush"
```

---

### Task 5: 端到端集成验证 + CPU 对照

**Files:**
- Create: `test/configs/triggered_snapshot_deferred.cfg`
- Test: 复用 Task 0 的 `tools/measure_traced_probes_cpu.sh`

**Interfaces:**
- Consumes: 阶段 1 全部改造。
- Produces: 验证"快照含前N秒 ftrace 数据"+"常驻 CPU 显著低于 Task 0 基线"。

- [ ] **Step 1: 写 deferred-raw 端到端 config**

`test/configs/triggered_snapshot_deferred.cfg`（在 Task 0 config 基础上，给 ftrace_config 加）:
```
      deferred_raw_capture {
        enabled: true
        per_cpu_mem_limit_kb: 16384
        retain_seconds: 10
      }
```

- [ ] **Step 2: 跑通并校验快照内容**

Run:
```bash
out/linux_clang_release/perfetto -c test/configs/triggered_snapshot_deferred.cfg --txt -o /tmp/snapd.pftrace &
sleep 12
out/linux_clang_release/perfetto --trigger snap
sleep 3
out/linux_clang_release/trace_processor_shell -q <(echo "select name, count(*) c from slice join thread_track on slice.track_id=thread_track.id group by 1 limit 5;") /tmp/snapd.pftrace.0
```
Expected: trace_processor 能解析快照，含触发前的 sched 事件（说明前 N 秒原始页被成功延迟解析）。

- [ ] **Step 3: 常驻 CPU 对照**

Run（deferred 跑起来、未触发时采样）:
```bash
./tools/measure_traced_probes_cpu.sh 30
```
Expected: 常驻 CPU% 明显低于 Task 0 记录的基线（解析被推迟）。记录对照数值。

- [ ] **Step 4: 集成测试（若环境支持 ftrace）**

Run:
```bash
tools/ninja -C out/linux_clang_release perfetto_integrationtests
out/linux_clang_release/perfetto_integrationtests --gtest_brief=1 --gtest_filter="*Ftrace*Clone*:*Clone*Ftrace*"
```
Expected: 现有 ftrace + clone 相关集成测试不回归（无相关用例则跳过，依赖 Step 2 手测）。

- [ ] **Step 5: Commit**

```bash
git add test/configs/triggered_snapshot_deferred.cfg
git commit -m "perfetto: add deferred-raw end-to-end config and CPU comparison"
```

---

## Self-Review（已执行）

- **Spec 覆盖**：§4 选型→阶段0 Task0；§6.1 内存环形→Task1；§9 proto→Task2；§6.2 旁路注入→Task3；§6.3 clone-flush 解析→Task4；§10 CPU 管控验证→Task5。**本 plan 不含**：磁盘溢出段（spec §6.1 阶段2）、通路 C 合并（spec §7 阶段3）、watcher（spec §8 阶段3）、解析限速（spec §12 风险1 / 阶段4）——按 spec §14 留作后续独立 plan。
- **占位扫描**：无 "TODO/TBD"。Task3 Step5 与 Task4 因深度依赖既有测试夹具/成员名，以"接口契约 + 镜像既有用例 + 执行前确认点"形式给出，并非空占位；所有新建独立单元（Task1）给出完整可编译代码与测试。
- **类型一致性**：`RawFtraceRingBuffer` 的 `PushPage`/`ForEachPageSince`/`size`/`capacity_pages`/`Clear` 在 Task1 定义、Task3/4 一致引用；`ParseRawRingBufferInto` 签名 Task3 定义、Task4 一致调用；`FtraceDataSourceConfig::deferred_raw_*` Task2 定义、Task3 引用。
