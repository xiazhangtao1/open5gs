<p align="center"><a href="https://open5gs.org" target="_blank" rel="noopener noreferrer"><img width="100" src="https://open5gs.org/assets/img/open5gs-logo-only.png" alt="Open5GS logo"></a></p>

## Getting Started

Please follow the [documentation](https://open5gs.org/open5gs/docs/) at [open5gs.org](https://open5gs.org/)!

## 本分支 UPF 性能优化状态

本节记录性能测试中已经得到证据支持的重要结论，用于避免后续优化重复试错。
原始命令、逐轮计数和更完整的数据见
[perf-tools/results.md](perf-tools/results.md)。除非特别说明，当前测试使用
VPP 26.06、N3/N6 memif、1428-byte inner IPv4 UDP、两个真实 PDU/PFCP
Session，并完整执行 Open5GS 的 PDR/FAR/QER/URR 和 GTP-U 语义；未引入
UPG-VPP 或绕过 UPF 语义的 fast path。

### 当前可复现基线（2026-07-26）

当前正式配置为两个 Session Worker、两个 memif qid、8192-entry ring。每档
目标持续 10 秒；“输入”是实际进入 Open5GS 输入 memif 的包，“丢包”只计算
Open5GS N3/N6 memif 段，不包含未插网线的 fabric VF 后续 ARP/NAT 丢包。

| 方向 | 发包节奏 | 目标 | 输入包 | 输出包 | Open5GS 段丢包 | 实际输出 |
|---|---|---:|---:|---:|---:|---:|
| 下行 | 10us | 1.2G | 1,050,420 | 1,050,420 | 0 | 1.200G |
| 下行 | 10us | 2.0G | 1,750,700 | 1,721,524 | 1.667% | 1.967G |
| 下行 | 10us | 4.0G | 3,501,400 | 3,425,757 | 2.160% | 3.914G |
| 下行 | burst256 | 1.2G | 1,050,420 | 1,049,362 | 0.101% | 1.199G |
| 下行 | burst256 | 2.0G | 1,750,700 | 1,735,144 | 0.889% | 1.982G |
| 下行 | burst256 | 4.0G | 3,501,400 | 3,368,691 | 3.790% | 3.848G |
| 上行 | 10us | 1.2G | 1,050,420 | 1,049,187 | 0.117% | 1.199G |
| 上行 | 10us | 2.0G | 1,750,700 | 1,711,974 | 2.212% | 1.956G |
| 上行 | 10us | 4.0G | 3,501,400 | 3,271,774 | 6.558% | 3.738G |
| 上行 | burst256 | 1.2G | 1,050,420 | 1,039,621 | 1.028% | 1.188G |
| 上行 | burst256 | 2.0G | 1,750,700 | 1,731,947 | 1.071% | 1.979G |
| 上行 | burst256 | 4.0G | 3,501,400 | 3,287,499 | 6.109% | 3.756G |

这些数字说明当前已能在过载条件下输出约 3.7～3.9G，但不能据此宣称稳定支持
4G 或 10G。当前严格零丢包只确认到下行 1.2G；上行 1.2G 的最低实测丢包为
0.117%。后续必须同时报告目标速率、实际提交速率、实际输出和丢包率。

### 已确认的瓶颈与排除项

- VPP/memif 本身不是当前 3～4G 完整 UPF 路径的带宽上限。同 Pod、两个 qid、
  8192 ring 的裸 libmemif slave TX 到 VPP RX 对照，10 秒提交
  176,116,480 包，VPP 接收 176,111,872 包（99.997%），payload 折算
  201.195G。该结果是共享内存能力，不代表物理网卡吞吐，但足以排除
  “memif 只能跑 3～4G”。
- 完整路径压力下 `tx-sent == tx-request`；差值发生在
  `alloc-request - alloc-granted`。因此包在 Open5GS 申请 TX buffer 时丢弃，
  不是提交给 `memif_tx_burst()` 后由 VPP 继续丢弃。
- 两个 Session 已固定归属两个 Worker，但 N3、N6 的 RX 流量实际上都只进入
  qid 1，qid 0 为零。Session 多 Worker 已实现，入口多队列分流尚未实现；
  round-robin 调度不能把一个已有 qid 的流量拆到另一个 qid。
- 4G/10us 下两个 Session Worker 仅使用约 45% CPU，非主动上下文切换约
  3～6 次/秒，不是 CFS 把工作线程频繁抢占。每个 Worker 主动切换约
  58K 次/秒，原因是 RX 平均 burst 约 2 包时反复执行
  `pthread_cond_wait`/唤醒；当前首要瓶颈是 dispatcher 到 Worker 的小批量
  投递和睡眠/唤醒节奏。
- 将 memif ring 从 8192 增至 16384 没有提高吞吐，反而令单个有效 qid 的
  pending 达到约 400ms，部分档位只提交目标包数的 63%～75%。部署已恢复
  8192；当前 libmemif 还限制 `log2_ring_size <= 14`，不应继续尝试 32768。
- 迁移 VPP RX queue 到其他 VPP Worker 没有收益；为无网线 fabric 口增加临时
  drop 路由也没有恢复缺失吞吐。简单换 VPP 核或 ARP 路径不是主因。
- 统一外部 epoll、公平 round-robin 和每 qid 预算改善了 N3/N6 公平性和可观测
  性，但没有明显提高低丢包稳定吞吐。`SCHED_FIFO` 曾导致服务器交互失去响应，
  当前所有线程保持 `SCHED_OTHER`，不再以实时调度掩盖数据面节奏问题。

### 主要测试里程碑

| 日期 | 修改或隔离实验 | 重要结论 |
|---|---|---|
| 2026-07-08 | 原 UDP/TUN 路径基线 | 2 CPU 下行低丢包边界约 1.25～1.3G，上行约 1.7G；不是 CFS quota throttling。 |
| 2026-07-18 | N6 VPP/memif | 功能可行；包进入 Open5GS 后 memif/VPP 未继续丢，单 UDP 接收/逐包路径约 1.0～1.15G。 |
| 2026-07-18 | N3/N6 双 memif | external VF 当时实际协商为 1Gbps，不能用于判断 10G 上限；fabric 内部路径证明能够超过 1G。 |
| 2026-07-21 | Release 优化与 perf | 修正 `-O0` 后过载峰值显著提高；曾观测到约 67% CPU 位于零超时轮询，促成统一 epoll 改造。过载峰值不是稳定吞吐。 |
| 2026-07-25 | N3 TX 批量 buffer 申请 | 下行 1.2G 从有丢包改善为零丢包，1.25G 丢包约 0.0457%，证明真正的批量申请有效。 |
| 2026-07-25 | Session 多 Worker、批量分类与投递 | 两个真实 Session 可固定归属两个 Worker，完整 UPF 语义不变；消除了逐包任务池热点，但单 RX qid 和小 burst 唤醒仍限制扩展。 |
| 2026-07-26 | 统一 epoll 与运行时计数 | dispatcher、Worker queue、RX/refill 和 epoll 均无丢包；压力差值定位到 TX allocation/ring 可用性。 |
| 2026-07-26 | ring 8192/16384、10us/burst256 A/B | 8192 明显更适合当前单有效 qid；均匀 10us 在部分档位更好，但发包节奏不是唯一瓶颈。 |
| 2026-07-26 | 裸 memif、VPP placement、drop route 隔离 | 排除 VPP/memif 裸带宽、VPP queue placement 和无网线 ARP 为主要瓶颈，责任收敛到 Open5GS 生产/消费节奏。 |
| 2026-07-26 | SPSC ring、0/20/50us busy-poll A/B（已回退） | 20us 下行/上行仅输出1.185G/1.181G，段丢包1.262%/1.571%，低于原基线；持续流量时20us已近似全忙轮询，说明单纯去锁和无限忙轮询不能解决TX小批次问题。 |

### 下一步优化优先级

1. 保留当前 `ogs_queue + pthread_cond` 路径，先统计每次 Worker 实际合并包数、
   N3/N6 TX alloc/flush 次数及失败率，再设计有严格包数/时间上限的自适应微批
   聚合。SPSC + busy-poll 已实测回退，不应直接重做或改为无限忙轮询。
2. 对短时 `memif_buffer_alloc()` 不足增加微秒级有界重试/延后 flush，不允许
   无限自旋，也不能让一个方向饿死另一个方向。
3. 实现真实入口多 qid：N3 按 TEID、N6 按 UE IP 分流，并使
   `qid -> Session owner -> Worker -> TX qid` 一一对应。优先通过 Open5GS
   和 VPP 现有配置/API 完成，不修改 VPP 源码。
4. 在上述节奏问题解决后，再评估 descriptor lease，消除 dispatcher 的
   payload copy。该阶段的主要风险是 memif descriptor 生命周期、断线 generation
   和停止排空，而不是 UPF 规则语义。

### 后续测试记录规范

每次性能测试完成后，都必须先更新本节的“当前可复现基线”或“主要测试里程碑”，
再将完整数据追加到 `perf-tools/results.md`。记录至少包括：

- 日期、代码 commit/镜像、拓扑和物理链路速率；
- Session、Worker、RX/TX qid、ring、CPU/cpuset 配置；
- 包长、方向、发包节奏、目标速率、实际提交速率和持续时间；
- 输入包、输出包、Open5GS 段丢包率及精确丢包点；
- 是否经过物理网卡，是否包含 ARP/NAT/VF 等下游丢包；
- 本轮能证明什么、不能证明什么，以及下一项实验。

性能结论必须区分“零/低丢包稳定吞吐”和“高丢包过载峰值”，不得用目标速率代替
实际提交速率，也不得用共享内存内部吞吐代替 X710/E810 物理口吞吐。

## Sponsors

If you find Open5GS useful for work, please consider supporting this Open Source project by [Becoming a sponsor](https://github.com/sponsors/acetcom). To manage the funding transactions transparently, you can donate through [OpenCollective](https://opencollective.com/open5gs).

<p align="center">
  <a target="_blank" href="https://open5gs.org/#sponsors">
      <img alt="sponsors" src="https://open5gs.org/assets/img/sponsors.svg">
  </a>
</p>

## Community

- Problem with Open5GS can be filed as [issues](https://github.com/open5gs/open5gs/issues) in this repository.
- Other topics related to this project are happening on the [discussions](https://github.com/open5gs/open5gs/discussions).
- Voice and text chat are available in Open5GS's [Discord](https://discordapp.com/) workspace. Use [this link](https://discord.gg/GreNkuc) to get started.

## Contributing

If you're contributing through a pull request to Open5GS project on GitHub, please read the [Contributor License Agreement](https://open5gs.org/open5gs/cla/) in advance.

## License

- Open5GS Open Source files are made available under the terms of the GNU Affero General Public License ([GNU AGPL v3.0](https://www.gnu.org/licenses/agpl-3.0.html)).
- [Commercial licenses](https://open5gs.org/open5gs/support/) are also available from [NewPlane](https://newplane.io/) at [sales@newplane.io](mailto:sales@newplane.io).

## Support

Technical support and customized services for Open5GS are provided by [NewPlane](https://newplane.io/) at [support@newplane.io](mailto:support@newplane.io).
