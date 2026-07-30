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

### UPF部署模式

当前Helm Chart支持两套经过实际运行验证的完整模式：

- 无VF兼容模式：N3使用内核UDP/2152，N6使用`ogstun`，不创建VPP容器、不申请
  SR-IOV资源，并关闭Session Worker。
- 高性能模式：N3/N6均使用raw-IP memif，由VPP 26.06 sidecar分别连接两个VF，
  支持按UPF CPU数量自动配置`1..16`个Session Worker。

两种模式的完整安装、切换、检查命令和地址要求见
[helm/xcn/README.md](helm/xcn/README.md#upf-dataplane-deployment-modes)。
当前不支持UDP/TUN与memif混合组合；从一种模式切换到另一种模式后需要让UE
重新注册并重建PFCP Session。

### 最新1/6 Session性能A/B（2026-07-30）

固定6 Worker、10us节奏、1428-byte inner IPv4、每档5秒，所有发生器均实际
送满目标。这里只统计Open5GS N3/N6 memif段，不包含未插线fabric VF之后的
ARP/NAT/物理链路：

| 实际灌入Session | 方向 | 可重复零丢包边界 | 10G档实际输出 | 10G档段丢包 |
|---:|---|---:|---:|---:|
| 1 | 下行 | 本轮仅确认0.5G | 9.4353G | 5.6469% |
| 1 | 上行 | 10G，连续3次零丢包 | 10.0000G | 0 |
| 6 | 下行 | 2G | 9.6151G | 3.8487% |
| 6 | 上行 | 2G；4G约0.24%丢包 | 9.7123G | 2.8765%，重复为5.4174% |

6 Session下行明显受益于多Worker；单Session不会拆给6个Worker。单Session
上行由于只有一个owner按序完成，反而避免6 Worker共享单RX qid时的跨Worker
有序refill阻塞。完整逐档包数、重复测试和计数证据见
[perf-tools/results.md](perf-tools/results.md#6-worker下1-session6-session-ab2026-07-30)。

### 独立VCL/VF发生器隔离（2026-07-30）

为排除正式VPP内置packet-generator与memif input争用Worker，固定6 Worker、
1 Session、1428-byte inner IPv4和20秒时长，对1.2G下行做了同实例A/B：

| 发生器 | 实际发生 | N6侧实际输入 | N3 memif输出 | 核心段丢包 |
|---|---:|---:|---:|---:|
| 正式VPP内置PG | 1.2000G | 1.2000G | 1.1954G | 0.3816% |
| 独立VCL/VF，第1次 | 1.1999G | 1.1380G | 1.0918G | 4.0611% |
| 独立VCL/VF，第2次 | 1.1999G | 1.1342G | 1.0571G | 6.7967% |

独立VCL没有恢复1.2G零丢包，因此正式VPP内置PG争用不是主要原因。独立VF两次
分别先发生108,420/114,887包`rx-miss`；进入正式VPP后，4条流经RSS进入N6
memif qid 0/1/5，但都归属同一个Session Worker。第二次核心段134,962包差值
可精确拆为N6 memif无空位45,736包、Worker队列满87,420包、N3 TX申请失败
1,806包。增加队列只能吸收短突发，不能改变单Session Worker约1.06G的本轮
持续处理速率。完整计数见
[perf-tools/results.md](perf-tools/results.md#独立vclvf发生器隔离2026-07-30)。

### 历史双Session基线（2026-07-27）

该阶段配置为两个 Session Worker、N3/N6专用dispatcher、descriptor lease、
两个 memif qid、8192-entry ring和20us混合轮询。每档
目标持续 10 秒；“输入”是实际进入 Open5GS 输入 memif 的包，“丢包”只计算
Open5GS N3/N6 memif 段，不包含未插网线的 fabric VF 后续 ARP/NAT 丢包。

| 方向 | 发包节奏 | 目标 | 输入包 | 输出包 | Open5GS 段丢包 | 实际输出 |
|---|---|---:|---:|---:|---:|---:|
| 下行 | 10us | 1.2G | 1,050,420 | 1,050,420 | 0 | 1.200G |
| 下行 | 10us | 2.0G | 1,750,700 | 1,725,778 | 1.424% | 1.972G |
| 下行 | 10us | 4.0G | 3,501,400 | 3,391,246 | 3.146% | 3.874G |
| 下行 | burst256 | 1.2G | 1,050,420 | 1,048,176 | 0.214% | 1.197G |
| 下行 | burst256 | 2.0G | 1,703,378 | 1,700,468 | 0.171% | 1.943G |
| 下行 | burst256 | 4.0G | 3,501,400 | 3,436,804 | 1.845% | 3.926G |
| 上行 | 10us | 1.2G | 967,934 | 961,352 | 0.680% | 1.098G |
| 上行 | 10us | 2.0G | 1,737,634 | 1,669,070 | 3.946% | 1.907G |
| 上行 | 10us | 4.0G | 3,289,028 | 3,126,680 | 4.936% | 3.572G |
| 上行 | burst256 | 1.2G | 897,148 | 895,004 | 0.239% | 1.023G |
| 上行 | burst256 | 2.0G | 1,551,001 | 1,551,001 | 0 | 1.772G |
| 上行 | burst256 | 4.0G | 2,871,613 | 2,818,561 | 1.847% | 3.220G |

这些数字说明当前下行过载输出可达约3.9G，但不能据此宣称稳定支持4G或10G。
当前严格零丢包仍只确认到下行1.2G。上行发生器本轮未发满目标，表中的零丢包
只能代表实际1.772G输入下的Open5GS段结果，不能写成“2G零丢包”。后续必须
同时报告目标速率、实际提交速率、实际输出和丢包率。

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
- 当前已实现N3/N6专用dispatcher和descriptor lease，普通报文不再执行
  `memif RX -> task payload`复制；完整UPF语义仍由原处理函数执行。功能冒烟
  17,506包零丢包，但压力测试没有把稳定下行从1.2G提升到2G。
- descriptor lease压力下N3 RX `in-flight-max=8192`，已触及单个有效qid的
  ring上限。dispatcher/Worker queue/refill/stale均无错误；剩余反压来自单qid
  跨Worker完成后的有序refill，以及输出TX buffer allocation可用性。
- 六个PFCP Session已验证可一一归属六个Worker：下行2G、上行4G可零丢包，
  下行4G约0.247%、上行6G约0.643%（10us、5秒）。六个Worker均无内部drop，
  但N3/N6仍只有RX qid 1工作且`in-flight-max=8192`，入口单qid已成为主要限制。
- OAI UE同时请求六个Session时，gNB/UPF能建立六组DRB/TEID/PFCP Session，
  但当前UE有并发NAS完整性计数问题，只创建四个业务TUN；六Session性能数据由
  内部发生器绕开UE/gNB获得，不能等同于OAI UE六接口业务已验证。
- `busy_poll_us=0/20/-1`实测中，20us比纯阻塞稳定；持续忙轮询虽把一次2G下行
  丢包降到0.278%，但1.2G/4G和重复测试波动，且不能避免ring耗尽。因此默认
  保持20us、`SCHED_OTHER`，不采用无限忙轮询。
- 独立VCL/VF发生器两次1.2G下行没有恢复零丢包，反而暴露N6 memif
  `no free tx slots`和单Session owner Worker queue-full。正式VPP内置
  packet-generator争用不是主要原因；外部RSS把同一Session送入多个RX qid后，
  最终仍汇聚到一个Worker，是比单纯N3 TX allocation更完整的真实入口瓶颈。

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
| 2026-07-27 | N3/N6专用dispatcher、descriptor lease、0/20/-1 A/B | 消除了dispatcher payload copy且100M功能零丢包；1.2G下行仍是零丢包边界。持续忙轮询收益不稳定，压力转移到单入口qid的有序completion/refill和输出TX allocation。 |
| 2026-07-27 | 六Session/六Worker全局最少连接分配 | 六个Session实际均分到六个Worker；下行2G、上行4G零丢包，4G下行约0.247%、6G上行约0.643%。六Worker内部无drop，N3/N6单有效RX qid均触及8192 in-flight上限。 |
| 2026-07-30 | Helm按UPF CPU自动推导Worker与memif队列 | `worker = UPF逻辑CPU - reservedCpus`，默认保留2核给N3/N6 dispatcher；8核实机自动生成6 Worker和双侧6队列，并正确绑定8个CPUManager独占逻辑核。 |
| 2026-07-30 | UDP/TUN与双memif模式切换实测 | UDP/TUN模式无VPP/VF，6个PFCP Session重建，3个OAI业务TUN各5次ping零丢包；恢复双memif后N3/N6各6 ring connected，N6注入1750包、Open5GS从N3输出1750包，核心memif段零丢包。fabric物理口未插线，因此后者不代表物理端到端回包。 |
| 2026-07-30 | 独立VCL/VF发生器隔离 | 1 Session、1.2G、20秒下，内置PG输出1.1954G/丢0.3816%；独立VCL两次只输出1.0918G/1.0571G，核心段丢4.0611%/6.7967%。排除PG抢占为主因，定位到外部VF rx-miss、N6 input ring、单owner Worker队列及N3 TX四段反压。 |
| 2026-07-30 | 直接路径、聚合、反压、TX重试A/B | 直接路径证明dispatcher→Worker架构贡献部分回退，但仍非零丢包；64包/8us聚合、未触发的队列重投和3次/10us N3 TX重试均无收益或明显变差，已全部回退。基线恢复后1.2G/20秒输出约1.198～1.200G、丢包0.205%和0.0367%。 |
| 2026-07-30 | 当前下行perf | Session worker的时间读取约占15%以上、mutex unlock 13.91%、queue pop 6.03%；N6 dispatcher的mutex lock 28.80%。VPP侧主要为正常DPDK/memif节点。下一步优先减少高频计时和completion/dispatcher锁竞争。 |

### 下一步优化优先级

1. 优化descriptor completion/refill节奏：减少空epoll/逐槽扫描开销，按完成
   水位做有界批量回收，并保留generation、断线和停止排空保护。必须监控
   `in-flight-max`，避免再次占满8192 ring。
2. 减少Session worker每轮/每包的`clock_gettime`及单任务queue lock操作，
   优先按既有batch边界更新时钟、批量pop/publish；必须保持低速流有界延迟。
   已实测N3 TX allocation原地重试会阻塞唯一owner Worker并显著恶化丢包，
   不再采用该方案。
3. 实现真实入口多 qid：N3 按 TEID、N6 按 UE IP 分流，并使
   `qid -> Session owner -> Worker -> TX qid` 一一对应。优先通过 Open5GS
   和 VPP 现有配置/API 完成，不修改 VPP 源码。
4. descriptor lease已完成，不再重做payload内存池复制；继续保留控制/异常
   报文复制回退和完整UPF语义。

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
