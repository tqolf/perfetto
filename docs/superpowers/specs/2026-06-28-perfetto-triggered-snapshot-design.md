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
裁剪单位 = 一个 ftrace page; 每页以"页内最后一个事件 ts"作为页时间戳;
触发时按 [T-N, T+M] 丢弃更旧的页, 保留最近 N 秒(+ 续录的 M 秒)。
```

存**整页（含 page header）**而非单事件，是为了触发时能直接复用现有解析路径 `CpuReader::ParsePagePayload`，不重写解析器。

### 6.2 改造注入点（精确到代码）

- 注入位置：`CpuReader::ReadAndProcessBatch`（`src/traced/probes/ftrace/cpu_reader.cc:269`）。该函数内 read 循环（`:283` 起）把最多 `kFtraceDataBufSizePages=32` 个原始页读入 `parsing_buf`，之后（`:358`）才调 `ProcessPagesForDataSource` 解析。
- 改造：若该 data source 标记为 **deferred-raw 模式**，在 read 循环之后、`ProcessPagesForDataSource` 之前插入分支——把 `parsing_buf` 内刚读到的 `pages_read` 个原始页 `memcpy` 进该 cpu 的 `RawFtraceRingBuffer`，然后 **return**，跳过解析。
- 效果：常驻 CPU 从"read + parse + 写 SMB"降到"read + memcpy"。

> 复用提示：`FrozenFtraceDataSource` / `CpuReader::ReadFrozen`（`cpu_reader.cc:1175`）已实现"从内核现存 ring buffer 事后读出并解析"，触发时解析逻辑可参考其对 `ParsePagePayload` 的调用方式。

### 6.3 触发时解析——挂在原生 CLONE flush 上

复用原生 `CLONE_SNAPSHOT` 的整条链路，只在一个点插入逻辑：

```
trigger 到达 service
  → PostDelayedTask(stop_delay_ms = M*1000)            [tracing_service_impl.cc:2002, 原生]
  → M 秒后(其间 B 仍在 memcpy 新页) NotifyCloneSnapshotTrigger
  → clone client 连接 → FlushAndCloneSession           [tracing_service_impl.cc:4436, 原生]
  → 给 traced_probes 发 Reason::kTraceClone flush       [tracing_service_impl.cc:4601, 原生]
  → 【新增】FtraceDataSource 在该 flush 回调里:
        把 RawFtraceRingBuffer 内 [T-N, now] 的原始页
        逐页 ParsePagePayload → 写进 TraceWriter(SMB)
  → flush ack → service 克隆 buffer → 落盘               [原生]
```

优雅之处：M 秒延迟、clone、落盘全部是原生流程；改造只在 `traced_probes` 的 flush 处理里插入"raw → parse → SMB"。后 M 秒天然由 `stop_delay_ms` 覆盖（这期间 raw buffer 继续收新页），前 N 秒由环形 buffer 保留。

- `traced_probes` 收 clone flush 的入口：`ProbesProducer::Flush`（`src/traced/probes/probes_producer.cc:641`），可据 `FlushFlags::Reason::kTraceClone` 区分 clone。
- 解析峰值需**分块 + 让出 CPU**（见 §10 风险 1）。

## 7. 通路 A / C 设计

- **A**（SDK track events + 低频关键 ftrace）：与 B 同会话不同 data source，走原生解析（开销本就低），共享 service buffer 与 clock snapshot。
- **C**（perf 采样 / 详细 ftrace，只录后 M 秒）：独立 `START_TRACING` 第二会话，trigger 时启动录 M 秒。
  - **合入同一份 trace**：Perfetto trace 文件本质是 `TracePacket` 的 protobuf 流、**物理可拼接**。C 会话落盘后，将其字节 **concat 到主快照文件尾部**，对外呈现为单份 trace。
  - 时间轴对齐：同机 boot clock 一致，各会话自带 clock snapshot，`trace_processor` 可正确合并解码。
  - concat 动作可在 `perfetto_cmd` 的 snapshot 落盘阶段完成，或由外围触发脚本兜底。

## 8. 触发设计

- **外部触发**：业务 / 监控进程 `perfetto --trigger <name>` 或 SDK `Tracing::ActivateTriggers`。IPC 路径：`producer_ipc_service.cc:346` → `TracingServiceImpl::ActivateTriggers`（`tracing_service_impl.cc:1865`），原生。
- **内部自动判定**：⚠️ 关键约束——常驻期通路 B **不解析**，无法从 trace 内容取指标，故内部判定**不能寄生在 `traced_probes` 内**。采用独立轻量 **watcher 进程**：读 PSI / `loadavg` / `/proc` 指标，越阈值即发 trigger。这是唯一干净的内部触发路径。

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

## 10. CPU / IO / 内存管控

| 资源 | 常驻期 | 触发时 | 旋钮 |
|---|---|---|---|
| CPU | 仅 read + memcpy（省掉 parse） | 一次性解析 N 秒，**分块 + yield 限速** | `drain_period_ms` / `drain_buffer_percent` / 解析块大小 |
| IO | 0（小 N）；仅内存段满才顺序写 raw（大 N） | 落盘一次 | `per_cpu_mem_limit_kb` / `disk_overflow_path` 开关 |
| 内存 | raw 环形 buffer，硬上限，溢出丢最旧 | 解析时临时 protobuf | `per_cpu_mem_limit_kb` / `disk_limit_mb` |

内核侧：`traced_probes` 仍需按 tick `read()` per-cpu pipe，read 频率不能太低，否则内核 per-cpu buffer（`buffer_size_kb`，默认低内存 2MB / 高内存 8MB 每 CPU）先溢出丢数据。

## 11. 关键改造点清单

| 区域 | 文件:行 | 改动 |
|---|---|---|
| raw 旁路注入 | `src/traced/probes/ftrace/cpu_reader.cc:269`（read 后 / `:358` parse 前） | deferred-raw 分支：memcpy 整页进 RawFtraceRingBuffer 后 return |
| raw 缓冲类（新增） | `src/traced/probes/ftrace/`（新文件） | `RawFtraceRingBuffer`：内存段 + 可选磁盘溢出环形 |
| 触发时解析 | `src/traced/probes/probes_producer.cc:641` / ftrace data source | clone flush 回调里把 raw 页 `ParsePagePayload` 写入 SMB |
| 解析复用 | `src/traced/probes/ftrace/cpu_reader.cc`（`ParsePagePayload` / 参考 `ReadFrozen:1175`） | 事后解析整页 |
| 配置 | `protos/perfetto/config/ftrace/ftrace_config.proto` | 新增 `DeferredRawCapture` |
| 通路 C concat | `src/perfetto_cmd/perfetto_cmd.cc`（snapshot 落盘附近，参考 `OnSessionCloned` ~`:1431`） | 第二会话输出 concat 入主快照 |
| watcher（新增） | 新工具 | 读 PSI/loadavg 越阈值发 trigger |

> 注：触发、CLONE、stop_delay、clock snapshot 均复用原生，无需改 `tracing_service_impl.cc` 的核心逻辑。

## 12. 风险与缓解

1. **触发瞬间解析峰值 CPU**：N 大时一次性解析几百 MB，恰逢业务可能正异常。→ 分块解析 + 主动让出 CPU（限速块大小可配）；必要时降优先级线程解析。这是本方案的真实代价，需在落地时压测。
2. **ftrace format 描述符一致性**：触发时解析依赖常驻期已 setup 的 event format。→ deferred-raw 期间禁止对该 data source 重配；format 随会话固定。
3. **内核 pipe 及时 drain**：read 频率过低 → 内核 buffer 先溢出。→ memcpy 很快，保持默认/合理 `drain_period_ms`；可用 `drain_buffer_percent` watermark。
4. **incremental state**：通路 A 的 SDK interned data 跨 snapshot 自解码依赖周期性 `ClearIncrementalState`（原生 `CLONE_SNAPSHOT` 已有考量），沿用即可；B 的 ftrace 事件自包含，无此问题。
5. **大 N 内存物理上限**：纯内存装不下几分钟全量。→ 由"内存为主 + 磁盘溢出环形"承接；并对外明确 N 与内存/磁盘上限的换算关系。

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
