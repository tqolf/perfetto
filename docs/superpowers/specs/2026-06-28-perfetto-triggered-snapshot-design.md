# 设计文档：Perfetto 常态化采集 + 触发式前N秒/后M秒落盘（低开销改造）

- 日期：2026-06-28
- 状态：已评审（待实现计划）
- 目标平台：嵌入式 / 通用 Linux（可改 Perfetto C++ 源码）

## 1. 背景与问题

`traced` / `traced_probes` 常驻运行时，在 CPU、IO 上对系统形成持续压力，可能引发稳定性问题。
现状下若开启 ftrace 全量采集并持续落盘，开销主要来自两处：

1. **CPU**：`traced_probes` 持续把内核 ftrace 二进制页**解析（parse）成 protobuf** 事件。这是常驻 CPU 的大头。
2. **IO**：tracing session 持续把数据落盘。

## 2. 需求

1. 常态化开启 `traced` / `traced_probes`，但**只在触发信号到来时**落盘"触发前 N 秒 + 触发后 M 秒"的数据（N、M 可配置）。
2. 对 CPU、IO、内存的影响**可管控**（有明确上限旋钮）。

### 2.1 需求画像（评审确认）

| 维度 | 结论 |
|---|---|
| 运行环境 | 嵌入式 / 通用 Linux，可改源码，Perfetto 版本可控 |
| 优化目标 | **同时**压住 CPU + IO + 内存 |
| 常驻数据源 | ftrace 内核事件（CPU 大头）+ 应用 SDK track events（开销已低） |
| 动态数据源 | perf 采样 / 详细 ftrace 等重型源，**触发时才开**，只需覆盖"后 M 秒" |
| 保真度 | 常驻源的"前 N 秒"和"后 M 秒"**都要全量高保真** |
| N / M | 可配置，**N 跨度大**（几秒 ~ 几分钟） |
| 触发 | 外部进程发 trigger + 可选内部指标自动判定 |
| raw 缓冲位置 | **内存为主 + 可选溢出到磁盘** |
| 通路 C 落盘 | **尽量合入同一份 trace** |

## 3. 一个绕不开的物理下限

要保留"触发**前** N 秒"的全量数据，意味着这 N 秒的事件在触发之前就**必须已经在缓冲区里**。因此：

- **内存 ≈ N × 事件产生速率**，是任何方案都绕不过的硬成本。能做的只是把"每条事件的存储系数"压到最小，并用环形缓冲设硬上限（溢出丢最旧）。
- **CPU**：常驻期真正烧 CPU 的是"把 ftrace 二进制页解析成 protobuf"。能否把这一步**推迟到触发时**，是方案的根本分水岭。

由于评审确认"前后都要全量高保真"，排除了"常驻降频/采样"路线；能大幅降常驻 CPU 的，只剩 **延迟解析（deferred parse）** 路线。

## 4. 方案选型

### 方案 A：纯原生 `CLONE_SNAPSHOT`（不改/极少改源码）
环形 buffer 常驻采集，`trigger_mode=CLONE_SNAPSHOT` + `stop_delay_ms=M*1000` 实现"触发即落盘前 N 秒、再续录后 M 秒"。
- ✅ IO 降为零（平时不落盘），稳定，完全兼容 upstream。
- ❌ **CPU 降不下来**——`traced_probes` 仍在常驻期全量解析 ftrace 写 service buffer（已被代码探查证实：原生 `CLONE_SNAPSHOT` 克隆的是 service 端已解析的 `TraceBuffer`，解析 CPU 在常驻期就已付出）。**不满足降 CPU 硬需求**。

### 方案 B：全量 ftrace 走 raw 延迟解析
所有常驻 ftrace 源都改成"常驻只搬运不解析、触发时才解析"。架构最纯粹，但改造面最大，且对本就不烧 CPU 的源（SDK track events）做 raw 通道没有收益。

### 方案 C：分级分流（**选定**）
按数据源特性分三条通路：低开销源走原生 `CLONE_SNAPSHOT`；唯一烧 CPU 的"全量重型 ftrace"走新增的 raw 延迟解析通道；重型动态源触发时才启动。把改造精确压在"唯一的 CPU 大头"上，风险/收益比最好。

**结论：选方案 C。**

## 5. 整体架构

三条通路汇入同一份快照：

```
  常驻·低开销通路 ───────►  A. SDK track events + 低频关键 ftrace
  (原生 CLONE_SNAPSHOT)        走 service 端 ring buffer (原生解析)   ─┐
                                                                      │ 触发时
  常驻·CPU大头通路 ──────►  B. 全量重型 ftrace(sched/irq/block)        │ CLONE
  (新增 raw 延迟解析)          read()→memcpy 进 raw 环形 buffer        ─┼─► 单份 trace
                              常驻期【不解析】, 触发时才 parse          │
                                                                      │
  动态·触发后通路 ───────►  C. perf采样/详细ftrace, 触发时才启动        │
  (START_TRACING 第二会话)     只录后 M 秒, 落盘后 concat 入主快照     ─┘
                                       ▲
                                       │ 触发
                        外部 perfetto --trigger / SDK ActivateTriggers
                        + 可选内部 watcher 进程(读 PSI/loadavg 越阈值发 trigger)
```

- **A 与 B 同属一个 `traced_probes` 会话的不同 data source**，共享同一 service buffer，clock snapshot 由 `MaybeSnapshotClocksIntoRingBuffer` 原生处理，时间轴天然对齐。
- **B 是唯一的真改造**，其余复用原生能力。

## 6. 通路 B 详细设计（核心改造）

### 6.1 二级 raw 环形缓冲（每 CPU 一个）

```
RawFtraceRingBuffer (per-cpu)
 ├─ 内存段:        固定上限 per_cpu_mem_limit_kb, 环形, 存【完整 ftrace page】(含 page header)
 └─ 磁盘溢出段(可选): mmap 固定大小环形文件, 仅当内存段满且配置了大 N 时启用, 满则丢最旧
裁剪单位 = 一个 ftrace page; 每页以 PageHeader.timestamp(页基准时间戳, ≈页内首个事件 ts)为页时间戳;
触发时按 cutoff 丢弃更旧的页。窗口长度 = N+M(见 §6.3 off-by-M 修正), 容量须按 N+M 秒配。
```

存**整页（含 page header）**而非单事件，是为了触发时能直接复用现有解析路径 `CpuReader::ParsePagePayload`，不重写解析器。

### 6.2 改造注入点（精确到代码）

- 注入位置：`CpuReader::ReadAndProcessBatch`（`src/traced/probes/ftrace/cpu_reader.cc:269`）。该函数内 read 循环（`:283` 起）把最多 `kFtraceDataBufSizePages=32` 个原始页读入 `parsing_buf`，之后（`:357`）才对每个 data source 调 `ProcessPagesForDataSource` 解析。
- 改造：若该 data source 标记为 **deferred-raw 模式**，在 read 循环之后、解析循环里插入分支——把 `parsing_buf` 内刚读到的 `pages_read` 个原始页 `memcpy` 进 `data_source->raw_ring_buffer(cpu_)`（页时间戳取 `ParsePageHeader` 的 `PageHeader.timestamp`），然后 `continue` 跳过该源解析。
- 效果：常驻 CPU 从"read + parse + 写 SMB"降到"read + memcpy"。
- raw buffer **挂在 `FtraceDataSource`**（每 CPU 一个），push 由 `CpuReader` 完成。无需加锁：ftrace 读与 flush 都在 traced_probes 同一 task runner 线程。

> 复用提示：`FrozenFtraceDataSource` / `CpuReader::ReadFrozen`（`cpu_reader.cc:1175`）已实现"从内核现存 ring buffer 事后读出并解析"，触发时解析逻辑可参考其对 `ParsePagePayload` 的调用方式。

### 6.3 触发时解析——挂在原生 CLONE flush 上（含审查修正）

复用原生 `CLONE_SNAPSHOT` 链路，但代码审查暴露 4 处必须修正的点：

1. **解析入口在 `FtraceController` 而非 `FtraceDataSource`**。解析必须有 `ProtoTranslationTable` 与 `LazyKernelSymbolizer`，二者归 controller/muxer 持有，`FtraceDataSource` **不持有**。flush 本就经 `controller_weak_` 走到 controller，由它拿 `table_`/`symbolizer_` 遍历各源 per-cpu raw buffer 解析。
2. **窗口是 N+M 不是 N（off-by-M）**。clone 在触发 **+M 秒**后发生（`stop_delay_ms`），解析时刻 `now=触发+M`。覆盖 `[触发-N, 触发+M]` 须 `cutoff = now - (retain_seconds + stop_delay_ms/1000)`；buffer 容量按 **N+M** 配。
3. **时钟域守卫**。`cutoff` 用 `base::GetBootTimeNs()`，仅当 `trace_clock=boot`（`ftrace_clock==FTRACE_CLOCK_UNSPECIFIED`）时与页 `timestamp` 同域。非 boot（global/local 或 mono_raw）时跳过时间裁剪（全量解析）或用 `FtraceClockSnapshot` 换算。
4. **flush 时序**。controller 在 clone-flush 回调里须 **先 parse 写满各 cpu TraceWriter → 再 commit/flush → 再 ack**；ack 必须晚于解析数据全部 commit，否则 service 克隆到半份。

```
trigger 到达 service
  → PostDelayedTask(stop_delay_ms = M*1000)            [tracing_service_impl.cc:2002, 原生]
  → M 秒后(其间 raw buffer 仍在 memcpy 新页) NotifyCloneSnapshotTrigger
  → clone client 连接 → FlushAndCloneSession           [tracing_service_impl.cc:4436, 原生]
  → 给 traced_probes 发 Reason::kTraceClone flush       [tracing_service_impl.cc:4602, 原生]
  → ProbesProducer::Flush 识别 kTraceClone, 转交 FtraceController:
       cutoff = GetBootTimeNs() - (N+M)秒  (非 boot 时钟则不裁剪)
       for cpu: 用 table_/symbolizer_ 把 raw buffer 内 ts>=cutoff 的页
                逐页 ParsePagePayload → 写进该源 TraceWriter(SMB)
       commit/flush writer → 之后才 ack
  → flush ack → service 克隆 service buffer → 落盘       [原生]
```

- 收 clone flush 入口：`ProbesProducer::Flush`（`probes_producer.cc:641`，第 4 参 `FlushFlags` 当前匿名，需命名并读 `flush_flags.reason()`）。识别 `FtraceDataSource` 用 `descriptor == &FtraceDataSource::descriptor` 后 `static_cast`（`probes_producer.cc:804` 先例），不改基类。
- 解析峰值需**分块 + 让出 CPU**（见 §12 风险 1）。
- ⚠️ **容量匹配（§12 风险 5）**：解析后 protobuf 仍经 SMB→service buffer 才被克隆落盘，故 service buffer 须能容纳 N+M 秒解析量，否则环形覆盖丢数据；常驻内存省了，触发瞬间峰值不省。

## 7. 通路 A / C 设计

- **A**（SDK track events + 低频关键 ftrace）：与 B 同会话不同 data source，走原生解析（开销本就低），共享 service buffer 与 clock snapshot。
- **C**（详细 ftrace，决策已定）：**纯配置，无需改代码、无需第二会话/concat**。把详细 ftrace 事件（irq、workqueue、block、更多 sched、syscalls 等）直接加进 **deferred-raw 的 ftrace 事件集**——它们和主数据一样常驻缓存、触发时一并解析进**同一快照**，天然前 N 秒 + 后 M 秒。现有实现（Task 1–5）已支持，只是配置里多列事件。
  - 代价：详细事件量更大 → raw buffer 更大（受 §15 内存护栏约束），用 §16 进程过滤压低事件量、或 Stage 2 磁盘溢出承接大窗口。
  - 注：perf 栈采样（traced_perf）是独立 producer、无 deferred 机制，要进同一单快照需较大改造；本项目范围内**通路 C 限定为详细 ftrace**。

## 8. 触发设计

- **触发方式（决策已定）：应用主动触发**。机器人业务在自己发现异常时直接发触发，不需要独立 watcher/检测器。两种等价方式：
  - 进程内 SDK：`Tracing::ActivateTriggers({"snap"})`（一行）。
  - 脚本/外部进程：`perfetto -c trig.cfg`，其中 `trig.cfg` 仅含 `activate_triggers: "snap"`（注意：`perfetto` **无** `--trigger` 选项）。
  - IPC 路径：`producer_ipc_service.cc:346` → `TracingServiceImpl::ActivateTriggers`（`tracing_service_impl.cc:1865`），原生。
- 不实现 watcher/标准检测器（见 §17，按决策已删减）：业务自有健康判定，发现问题即发 `snap` 触发。

## 9. 配置（proto 扩展）

- `FtraceConfig` 新增：
  ```
  message DeferredRawCapture {
    optional bool   enabled = 1;
    optional uint32 per_cpu_mem_limit_kb = 2;   // 内存段每 CPU 上限
    optional uint32 retain_seconds = 3;          // N
    optional string disk_overflow_path = 4;      // 可选磁盘溢出环形文件目录
    optional uint32 disk_limit_mb = 5;           // 磁盘溢出总上限
  }
  ```
- 触发沿用现有 `TriggerConfig{ mode=CLONE_SNAPSHOT, triggers[].stop_delay_ms = M*1000 }`，零新增字段。
- ⚠️ **muxer 接线（审查修正）**：`FtraceDataSourceConfig`（`ftrace_config_muxer.h:50`）**不是 POD**——它有显式全参构造函数 + 初始化列表、部分成员 `const`、无默认值。新增 3 个字段须同步改：构造函数签名、初始化列表、成员声明三处；并在 `ftrace_config_muxer.cc:697` 的 `emplace(piecewise_construct, forward_as_tuple(...))` 末尾按序追加 3 个实参（值在 emplace 前从 `request`，即 proto 参数名，解析）。不能"先构造再赋值"。

## 10. CPU / IO / 内存管控

| 资源 | 常驻期 | 触发时 | 旋钮 |
|---|---|---|---|
| CPU | 仅 read + memcpy（省掉 parse） | 一次性解析 N 秒，**分块 + yield 限速** | `drain_period_ms` / `drain_buffer_percent` / 解析块大小 |
| IO | 0（小 N）；仅内存段满才顺序写 raw（大 N） | 落盘一次 | `per_cpu_mem_limit_kb` / `disk_overflow_path` 开关 |
| 内存 | raw 环形 buffer，硬上限，溢出丢最旧 | 解析 protobuf + **service buffer** 容纳整份 | `per_cpu_mem_limit_kb` / `disk_limit_mb` / service `buffer.size_kb` |

内核侧：`traced_probes` 仍需按 tick `read()` per-cpu pipe，read 频率不能太低，否则内核 per-cpu buffer（`buffer_size_kb`，默认低内存 2MB / 高内存 8MB 每 CPU）先溢出丢数据。

> ⚠️ **峰值内存（审查修正）**：触发时解析出的 protobuf 要经 SMB→service buffer 才被克隆落盘，故 service `buffer.size_kb` 须能容纳 N+M 秒解析量。**常驻内存省了**（raw 比 protobuf 紧凑、service buffer 常驻几乎空），但**触发瞬间峰值并不省**，还多一份 service buffer。若内存紧张，备选是改走 `write_into_file` 流式落盘（偏离 CLONE_SNAPSHOT 模型，需另设计）。

## 11. 关键改造点清单

| 区域 | 文件:行 | 改动 |
|---|---|---|
| raw 旁路注入 | `cpu_reader.cc:357`（解析循环内） | deferred-raw 分支：memcpy 整页进 `data_source->raw_ring_buffer(cpu_)` 后 `continue` |
| raw 缓冲类（新增） | `src/traced/probes/ftrace/raw_ftrace_ring_buffer.{h,cc}`（新文件） | `RawFtraceRingBuffer`：内存段 + 可选磁盘溢出环形 |
| raw buffer 归属 | `ftrace_data_source.{h,cc}` | `FtraceDataSource` 持每 cpu `RawFtraceRingBuffer` + 访问器 |
| 触发时解析（入口） | `FtraceController`（持 `table_`/`symbolizer_`/`cpu_readers`） | clone-flush 时遍历各源 raw buffer，`ParsePagePayload` 写 SMB；**不在 FtraceDataSource**（它无 table/symbolizer） |
| clone 识别 | `probes_producer.cc:641`（命名第4参 `FlushFlags`）+ `:804` 式 `descriptor` 判别 | `reason()==kTraceClone` 时转交 controller |
| 解析复用 | `cpu_reader.cc`（`ParsePagePayload`，`table->generic_evt_pb_descriptors()`；参考 `ReadFrozen:1175`） | 事后解析整页；generic descriptors 取自 **table** 非 ds_config |
| 配置 | `protos/perfetto/config/ftrace/ftrace_config.proto`（字段 38）+ `ftrace_config_muxer.{h,cc}` | 新增 `DeferredRawCapture` + 构造函数三处接线 |
| 通路 C concat | `src/perfetto_cmd/perfetto_cmd.cc`（snapshot 落盘附近，参考 `OnSessionCloned` ~`:1431`） | 第二会话输出 concat 入主快照 |
| watcher（新增） | 新工具 | 读 PSI/loadavg 越阈值发 trigger |

> 注：触发、CLONE、stop_delay、clock snapshot 均复用原生，无需改 `tracing_service_impl.cc` 的核心逻辑。改动集中在 `traced_probes` 侧（ftrace controller / data source / cpu_reader / muxer / proto）。

## 12. 风险与缓解

1. **触发瞬间解析峰值 CPU**：N 大时一次性解析几百 MB，恰逢业务可能正异常。→ 分块解析 + 主动让出 CPU（限速块大小可配）；必要时降优先级线程解析。这是本方案的真实代价，需在落地时压测。
2. **ftrace format 描述符一致性**：触发时解析依赖常驻期已 setup 的 event format。→ deferred-raw 期间禁止对该 data source 重配；format 随会话固定。
3. **内核 pipe 及时 drain**：read 频率过低 → 内核 buffer 先溢出。→ memcpy 很快，保持默认/合理 `drain_period_ms`；可用 `drain_buffer_percent` watermark。
4. **incremental state**：通路 A 的 SDK interned data 跨 snapshot 自解码依赖周期性 `ClearIncrementalState`（原生 `CLONE_SNAPSHOT` 已有考量），沿用即可；B 的 ftrace 事件自包含，无此问题。
5. **触发峰值 service buffer 容量**（审查新增）：解析后 protobuf 经 SMB→service buffer 才被克隆，service `buffer.size_kb` 须容纳 N+M 秒解析量，否则环形覆盖丢数据。→ 按 N+M 解析量配 service buffer；或改 `write_into_file` 流式落盘（另设计）。
6. **解析入口需 table/symbolizer**（审查新增）：`FtraceDataSource` 不持有 `ProtoTranslationTable`/`LazyKernelSymbolizer`，触发解析入口必须在 `FtraceController`，否则编译失败。
7. **off-by-M 窗口**（审查新增）：clone 在触发 +M 秒后，`cutoff = now-(N+M秒)`，buffer 容量按 N+M。
8. **时钟域**（审查新增）：`cutoff` 用 boottime，仅 `trace_clock=boot` 时与页 timestamp 同域；非 boot 须跳过时间裁剪或用 `FtraceClockSnapshot` 换算。
9. **flush 时序**（审查新增）：controller 须 parse→commit→ack 严格有序，ack 晚于数据 commit，否则克隆半份。
10. **大 N 内存物理上限**：纯内存装不下几分钟全量。→ "内存为主 + 磁盘溢出环形"承接；明确 N 与内存/磁盘上限换算。

## 13. 测试策略

- **单元测试**：`RawFtraceRingBuffer` 环形/溢出/磁盘回绕语义；deferred-raw 旁路不改变解析结果（同一批页，立即解析 vs 缓存后解析，protobuf 输出一致）。
- **集成测试**（`perfetto_integrationtests`）：常驻 deferred-raw + CLONE_SNAPSHOT 全链路，校验快照包含 [T-N, T+M] 数据。
- **diff 测试**：触发后产出的 trace 经 `trace_processor` 解析，sched/irq 事件完整、时间轴连续。
- **压测**：常驻 CPU（deferred-raw vs 原生解析）对比；触发瞬间解析峰值与限速效果；大 N 下内存/磁盘上限不被突破。

## 14. 建议实施阶段

1. **阶段 0**：先用纯配置（方案 A，原生 `CLONE_SNAPSHOT` + `stop_delay_ms`）跑通"前 N 秒 + 后 M 秒"端到端，建立基线与压测框架（IO 已降零，CPU 未降）。
2. **阶段 1**：实现 `RawFtraceRingBuffer`（仅内存段）+ deferred-raw 旁路 + 触发时解析，验证常驻 CPU 下降。
3. **阶段 2**：磁盘溢出环形段，支持大 N。
4. **阶段 3**：通路 C 动态源 + concat 合并；watcher 进程。
5. **阶段 4**：解析限速、参数化压测、上限校验。

## 15. 实测验证结果（本机真机压测）

在内核 6.8 + 真实 tracefs 的 128 核机器上，跑完整 `traced`/`traced_probes`/`perfetto` 栈，相同 fork/syscall 负载（sched + raw_syscalls 事件），30s 窗口，同一 traced_probes 实例背靠背对比：

| 模式 | traced_probes 常驻 CPU% | trace 输出 |
|---|---|---|
| 立即解析（baseline） | **~39%** | 67 MB（全量解析落盘） |
| deferred-raw（无触发） | **~8%** | ~11 KB（页缓存，未解析/未落盘） |

- **常驻 CPU 降约 80%**，残留 ~8% 为 `read()`+`memcpy`+poll 搬运开销（已无解析）。这是"降 CPU"核心命题的实测证据。
- deferred 无触发时 trace ≈ 空（11 KB vs 67 MB），系统级证实页只缓存不解析。

### ⚠️ 实测发现的硬约束：内存护栏
首次用 `per_cpu_mem_limit_kb=8192`（8 MB）时，**traced_probes 被 Perfetto 内置内存看门狗杀掉**：
```
watchdog_posix.cc: Memory window of 358 MB is above the 34 MB limit.
```
8 MB × 实际分配 raw buffer 的 ~44 个 CPU ≈ 358 MB，远超 traced_probes 默认 ~32 MB 护栏。降到 256 KB/cpu（总 ≤ 32 MB）后正常。

**硬约束**：`per_cpu_mem_limit_kb × num_cpus` 必须 ≤ traced_probes 内存护栏（默认 ~32 MB），否则进程被杀。推论：
- 核数越多，每核 buffer 必须越小（128 核 → 每核仅 ~256 KB）→ 内存段能保留的"前 N 秒"很短。
- 要支持大 N，必须：(a) 调高 `traced_probes` 内存护栏上限，或 (b) 用磁盘溢出环形段（阶段 2），或 (c) 用 §16 的进程过滤大幅压低事件量。
- 进程过滤（§16）是这里的关键杠杆：过滤后事件量小一两个数量级，相同内存能保留长得多的 N。

### 触发侧 e2e 实测（CLONE_SNAPSHOT 全链路）
在同一真机上，用 `test/configs/triggered_snapshot_deferred.cfg`（deferred-raw + `CLONE_SNAPSHOT` 触发）跑完整链路：启动会话 → 攒 6s deferred 页 → 用 `activate_triggers: "snap"` 发触发 → 内核 clone flush。结果：

| 场景 | 快照输出 |
|---|---|
| 无触发 | ~11 KB（空，页只缓存未解析） |
| **发触发后** | **`*.0` 快照 2.25 MB**，含 **247,837 条 ftrace 事件**（sched_switch 167,073 + sched_waking 80,764），trace_processor 正确解析为 167,073 个 sched_slice |

证明触发侧全链路工作：`ProbesProducer::Flush(kTraceClone)` → `FtraceDataSource::OnFtraceFlushComplete` → `FtraceController::ParseDeferredRawForClone` 解析 per-cpu raw buffer → 数据进 SMB → 被 clone 落盘。这是之前唯一无法单测的路径，现真机端到端验证通过。

注：触发由含 `activate_triggers: "snap"` 的配置发送（`perfetto -c trig.cfg`），**不是** `--trigger`（该选项不存在）；CLONE_SNAPSHOT 快照写到 `-o` 路径加计数后缀（`file.0`、`file.1`…）。

### 磁盘溢出段 e2e 实测（大 N，绕开内存护栏）
配置 tiny 内存段（`per_cpu_mem_limit_kb=64`）+ 磁盘溢出（`disk_overflow_path=/tmp/draw`、`per_cpu_disk_limit_kb=2048`），真机跑 deferred-raw + CLONE_SNAPSHOT：

- traced_probes 为每个 CPU 创建并预分配磁盘环形文件 `deferred_raw_cpu<N>`（128 个，各 2 MB）；内存段仅 64 KB/cpu（总 8 MB，**远在内存护栏内**）。
- 触发后快照 **9.15 MB，含 ~104 万 ftrace 事件**（sched_switch 711,288 + sched_waking 328,756）——对比同机纯内存 256 KB/cpu 仅 ~24.7 万事件。**磁盘溢出把保留窗口扩大了数倍，且内存占用不升**。

结论：磁盘溢出段让"大 N + 多核"可行——用顺序磁盘 IO 换内存，内存段保持在护栏内，溢出页在触发时一并解析进快照。这是 §15 内存护栏约束的解法之一（另一解法是 §16 进程过滤降事件量）。

## 16. 进程级事件过滤（可选，可动态增删）

### 动机
很多场景只关心**部分进程**的事件。在内核 ftrace **写入 ring buffer 之前**就按 PID 丢弃无关事件，是最高效的过滤——同时省 CPU（不写、不读、不解析）、IO、内存，且与 deferred-raw 叠加：过滤后缓存的 raw 页更少，相同 `per_cpu_mem_limit_kb` 能覆盖长得多的 N。

### 现状（Perfetto 已有，但不够）
- `FtraceConfig.tids_to_trace`（字段 35）→ `Tracefs::SetEventTidFilter` 写入 tracefs `set_event_pid`（`tracefs.cc:285`）。内核据此只记录这些 **TID** 的事件。
- 局限：**(1) TID 级**（用户通常按进程 PID 思考，一个进程多线程要逐个列）；**(2) 不跟随**新线程/子进程（没设 `event-fork` 选项）；**(3) 静态**，会话启动时设定，不能中途增删。

### 设计增量
1. **进程级（PID → TID 展开）**：新增配置 `pids_to_trace`（进程粒度）。setup 时对每个 PID 读 `/proc/<pid>/task/` 得到全部 TID，与 `tids_to_trace` 求并集后写 `set_event_pid`。
2. **跟随新线程/子进程**：启用内核 tracefs 选项 `options/event-fork`。开启后内核在 fork 时**自动**把被过滤任务的子任务加入 `set_event_pid`——新线程、子进程原生覆盖，无需轮询 `/proc`。这是关键，使过滤集"自我维护"。
3. **动态增删**：`set_event_pid` 内核文件支持运行时重写。traced_probes 侧维护"目标 PID 集合"，提供一个控制入口，在变更时重新展开 TID 并重写 `set_event_pid`（配合 `event-fork`，通常只需增删"根进程"，其后代由内核自动跟随）。控制入口候选：
   - **(A) 控制文件 watch（推荐）**：traced_probes 监视一个控制路径（如 `/run/perfetto/ftrace_pid_filter`），内容变化即重算并重写。简单、与 Perfetto 会话生命周期解耦、易脚本化。
   - (B) 复用 trigger：trigger 不易携带 PID 负载，不合适。
   - (C) 外部特权 helper 直接写 `set_event_pid`：最简，但绕过 Perfetto，与 muxer 在 teardown 时 `ClearEventTidFilter` 冲突，多会话下有竞争——不推荐。

### 与本方案的关系
- 纯配置层面，`tids_to_trace` 现在就能用（静态 TID 过滤）；**进程级 + event-fork + 动态**是新增改造，集中在 muxer / tracefs / 一个控制 watch。
- 与 deferred-raw 正交且互补：先内核过滤（降事件量）→ 再 deferred 缓存（降解析）。两者叠加对 CPU/IO/内存三者都最优。

### 实现进展 + e2e 实测
**进程级 `pids_to_trace` 已实现并真机验证**（commit 1f53decfa5）：`FtraceConfig.pids_to_trace`（字段 39）按 `/proc/<pid>/task` 展开当前 TID，与 `tids_to_trace` 求并集写 `set_event_pid`，并自动开启内核 `event-fork`（跟随未来线程/子进程）。单测 `ProcessLevelPidFilter`（用 `getpid()` 展开），41 muxer 测试通过。

真机 e2e（5 线程 python 目标 + 20 噪声进程，只过滤目标 PID）：
- 目标进程的 **5 个线程全部被捕获**（主 + 4 worker，各 ~7800 事件）→ PID→TID 展开抓全。
- 总事件 ~11 万 vs 无过滤数百万 → **降约 20 倍**。
- **sched_switch 语义已确认**：过滤后仍记录每次切换的 prev/next **对手方**（噪声进程以零星计数出现，作为目标的切换对端），即"能拿到目标的完整切换上下文，连对手方一起给"——正是排查所需。

### 待确认（实现前）
- 动态控制入口选 (A) 还是别的形式（控制文件 watch，第二步实现）；
- `event-fork` 同时影响 function tracer 的 `function-fork`，本方案只在设置 `pids_to_trace` 时开启，且 deferred-raw 不用 function tracer，无冲突。

### 动态控制文件 watch —— 已实现 + e2e
**已实现**（commit 6ee1f4c2cf）：`FtraceConfig.pid_filter_control_file`（字段 40）。traced_probes 起一个 `FtracePidFilterWatcher`（task runner 上轮询，默认 1s），控制文件内容 = 完整目标 PID 集（每行一个，`#` 注释/空行忽略）；变化时展开 TID + 保留静态 tids/pids，**运行时重写 `set_event_pid`**，并开启 event-fork。依赖注入设计，`PollOnce`/`ParsePidFilterFile` 单测覆盖。`ExpandPidToTids`/`ParsePidFilterFile` 提到 `ftrace_config_utils` 共享（DRY）。

真机 e2e：控制文件初始放无关 PID（目标排除）→ 运行中 `echo $TARGET > 控制文件` → watcher 1s 内拾取 → 目标的 **5 个线程全部被动态捕获**（各 ~6700 事件）。证明运行时增删 PID 生效。

### 控制入口与 HTTP
控制文件天然适合后接 HTTP/gRPC：它把"机制"和"接口"解耦。traced_probes 只认一个控制文件（如 `/run/perfetto/ftrace_pid_filter`），内容变化即重算 TID 并重写 `set_event_pid`；HTTP 服务只是薄壳（`PUT /filter` → 写文件），可作为独立进程、不与 traced_probes 耦合。控制协议建议**行式**（每行一个 PID，或 `+1234`/`-1234` 增删），便于 HTTP 转发与 `echo >>` 手动调试。机器人单机部署，HTTP 服务与 traced_probes 共享文件系统，无跨主机问题。

## 17. 触发检测架构（标准 + 业务自定义）

> **决策已定（精简）**：机器人业务**自有异常判定**，发现问题后**主动发 `snap` 触发**（SDK `ActivateTriggers` 或 `perfetto -c trig.cfg`）。因此**不实现 watcher 和标准检测器**（§17.2 取消）。下文"沙漏腰"的契约思想仍成立——触发机制不关心谁触发；只是检测侧由业务承担，本项目不内置检测器。`max_per_24_h`/`skip_probability` 原生限流仍可用；需要时不同问题可用不同具名 trigger 映射不同抓取档案。

整套方案服务于**机器人稳定性排查**：在机器人出现不稳定的瞬间抓到现场。难点在"如何检测那一刻"，且要同时支持标准信号与业务自定义。设计原则：**不让检测器去理解所有"不稳定"，而是把"发触发"做成谁都能用的统一契约**。

### 核心：trigger 是"沙漏的腰"
```
   多种检测器(上半)              统一契约              一套快照机制(下半)
 标准: PSI/loadavg/sched 延迟  ─┐
 OOM/watchdog/关键进程死亡      ├──► 具名 trigger "snap" ──► deferred-raw + CLONE_SNAPSHOT
 业务自定义信号                ─┘    (不关心谁/为什么触发)    → 前N秒 + 后M秒快照
```
快照机制完全不关心触发来源。标准与自定义天然共存，因为都收敛到同一个具名 trigger。

### 标准检测器（开箱即用）
一个轻量 watcher 守护进程，配置驱动一组内置检测器，越阈值即 `ActivateTriggers("snap")`：
- `psi`：cpu/io/memory 压力（读 `/proc/pressure/*`，如 `full avg10 > 阈值`）
- `loadavg`：`/proc/loadavg` 越阈值
- `sched_latency`：关键线程 runnable-但-未运行 时间过长（被抢占太久）
- `proc_exit`：关键进程死亡
- `oom`：OOM（`/proc` 或内核事件）
每个检测器带 `debounce_ms` 防抖。内置这些是因为它们是机器人不稳定的通用信号，业务无需自己写。

### 业务自定义（两种接入，丰俭由人）
- **模式 A — 业务自己判定，直接发 trigger（最直接）**：业务已知道自己出问题，直接发触发，不经过 watcher。
  - 进程内：Perfetto SDK `Tracing::ActivateTriggers({"snap"})`（一行）
  - 脚本/其它进程：`perfetto -c trig.cfg`（或一个 `snap-trigger` 小工具）
- **模式 B — 业务只上报指标，watcher 做阈值/防抖**：业务把自定义指标推给 watcher 的通用输入（与 §16 控制文件同款思路，HTTP-ready）：
  - `{ type: external, source: "/run/perfetto/signals", rule: "value>X for 3s" }`
  - 业务往文件/管道/socket/HTTP 写指标，watcher 统一套阈值 + 防抖。

### 两个增强点
1. **具名 trigger → 每类问题不同抓取档案**：卡顿用 `snap_hang`、OOM 用 `snap_oom`，各自配不同 N/M 与事件集（卡顿要详细 sched，OOM 要内存事件）。标准/自定义信号映射到不同 trigger 名即可。
2. **限流原生支持**：`TriggerConfig.triggers` 的 `max_per_24_h`、`skip_probability` 已能防"同一问题狂发触发淹没系统"。

### 结论：watcher 可以很小
业务自定义大多走**模式 A（直接发 trigger）**，watcher 只需提供少数标准系统检测器 + 一个通用 external 输入，不必做成"什么都懂"的大家伙。

### 实施建议
- **17.1**：`snap-trigger` 小工具 + SDK 用法示例（模式 A，近零成本）。
- **17.2**：最小 watcher（2~3 个标准检测器：psi/loadavg/proc_exit + external 通用输入），配置驱动 + 防抖 + 复用原生 `max_per_24_h` 限流。

### 待确认
- 标准检测器首批落地哪几个（建议 psi + loadavg + proc_exit）；
- 机器人"不稳定"的具体可观测信号（卡顿/重启/实时性丢失/业务指标），用以校准 sched_latency 等检测器的阈值与语义——这一项需业务侧进一步明确。
