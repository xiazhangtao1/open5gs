# UPF performance results

## 统一外部 epoll 与公平 I/O 预算（2026-07-26）

N3/N6 两个 libmemif socket 改由同一个外部 epoll 管理，仍保留单 dispatcher
和单生产者模型。libmemif 中断回调只标记 qid pending；dispatcher 在 N3/N6
之间交替优先，并在每个方向内部 round-robin 选择 qid。每个 qid 每轮默认最多
处理 1024 包或 50 微秒，Session owner、worker 队列以及
PDR/FAR/QER/URR/GTP-U 原处理函数均未改变。

运行时累计统计包括 epoll wait/event/error、每 qid interrupt/burst/包数、
burst min/avg/max、预算让出、当前及最大 pending 时间、解析和 dispatch 丢包、
refill 错误、worker queue depth/high-water/full/push-fail，以及 N3/N6 TX
ring-full/alloc-fail/drop。

实际部署使用两个真实 PDU/PFCP Session（`10.45.0.2`、`10.45.0.3`）和两个
Session Worker。VPP packet-generator 从 N6 以约 10 微秒节奏注入 100 Mbps、
2 秒、1428-byte IPv4 UDP：

| N6 注入 | N6 RX 计数 | Worker 0/1 | N3 输出 | Open5GS 丢包 |
|---:|---:|---:|---:|---:|
| 17,506 | 17,506 | 8,752 / 8,754 | 17,506 | 0 |

本轮 `dispatch-drop=0`、两个 worker 的 `queue-full/push-fail=0`、N3/N6
`TX drop/ring-full/alloc-fail=0`、`rx-error/refill-error=0`、epoll error=0；
N6 qid 最大 pending 为 48 微秒。测试后已禁用并删除临时 packet-generator
流和接口。

### 实际吞吐复测

使用相同两个真实 PDU/PFCP Session、两个 Session Worker、1428-byte inner
IPv4 UDP 和每条流约 10 微秒 `maxframe` 节奏，每档持续 10 秒。下行从 VPP
`ip4-input` 注入 N6；上行从真实 PFCP 建链消息取得两组 UL TEID，再以 RAW-IP
PCAP 从 VPP N3 FIB 10 注入。两方向均完整执行 Open5GS 的
GTP-U/PDR/FAR/QER/URR 处理。fabric VF 未插网线造成的后续 ARP/NAT 丢包不计入
Open5GS 段丢包。

下行以 `memif1/0 tx` 为 N6 输入、`memif2/0 rx` 为 N3 输出。1.2G、2G、4G
各重复三次，其余为单次：

| 目标 inner | N6 输入/每轮 | N3 输出 | Open5GS 段丢包 | 实际输出 |
|---:|---:|---:|---:|---:|
| 600M | 525,210 | 525,210 | 0 | 600M |
| 1.2G | 1,050,420 | 平均 1,046,263 | 平均 0.396%（0.343%～0.428%） | 平均 1.195G |
| 2.0G | 1,750,700 | 平均 1,664,886 | 平均 4.902%（2.809%～8.255%） | 平均 1.902G |
| 3.0G | 2,626,050 | 2,510,851 | 4.387% | 2.868G |
| 4.0G | 3,501,400 | 平均 3,270,192 | 平均 6.603%（5.488%～8.063%） | 平均 3.736G |
| 6.0G | 5,252,100 | 4,912,345 | 6.469% | 5.612G |
| 8.0G | 7,002,800 | 6,520,572 | 6.886% | 7.449G |

上行以 `memif2/0 tx` 为 N3 输入、`memif1/0 rx` 为 N6 输出：

| 目标 inner | N3 输入 | N6 输出 | Open5GS 段丢包 | 实际输出 |
|---:|---:|---:|---:|---:|
| 600M | 525,210 | 525,079 | 0.025% | 600M |
| 1.2G | 1,050,420 | 1,000,127 | 4.788% | 1.143G |
| 2.0G | 1,750,700 | 1,654,661 | 5.486% | 1.890G |
| 3.0G | 2,626,050 | 2,191,844 | 16.535% | 2.504G |
| 4.0G | 3,501,400 | 2,947,540 | 15.818% | 3.367G |
| 6.0G | 5,252,100 | 4,166,711 | 20.666% | 4.760G |
| 8.0G 目标 | 6,225,896 | 4,786,014 | 23.127% | 5.468G |

8G 上行档采用严格 10 秒启停，发生器实际只向 N3 memif 提交 7.113G，未达到
完整 8G。压力结束时 `epoll errors=0`，两 worker 的
`queue-full/push-fail/drops=0`，N3/N6 的 `dispatch-drop/rx-error/refill-error`
均为 0；两个方向的流量实际都落在 qid 1，因此本轮 qid round-robin 没有获得
多队列并行收益。丢包对应 N3/N6 TX `ring-full/alloc-fail`，过载时最大 pending
达到约 400ms。

与统一 epoll 前相同双 Session/10 微秒口径相比，1.2G 下行由单次 0.638% 降为
三轮平均 0.396%，但 2G 由单次 2.660% 上升为平均 4.902%；4G 实际输出仍约
3.736G。结论是本阶段提高了 N3/N6 公平性并消除了两次独立空轮询，但没有明显
提高低丢包稳定吞吐，严格零丢包下行仍只确认到 600M。8G 目标下能输出约
7.45G 属于高丢包过载能力，不能作为稳定吞吐。

## TX qid 精确计数与 memif ring A/B（2026-07-26）

N3、N6 的 TX 统计由方向汇总改为逐 qid 统计。每个 qid 现在分别记录：

- `send-request`：进入该 qid 发送函数的报文数；
- `alloc-call/request/granted/short/fail`：buffer 申请次数、申请描述符数、实际
  获得数、短缺描述符数和完全失败的申请调用数；
- `tx-call/request/sent`：`memif_tx_burst()` 调用次数、提交数和实际发送数；
- `drop`：该 qid 在 Open5GS TX 路径确认丢弃的报文数。

计数使用 relaxed atomic 读取/更新，不增加锁，不改变 Session owner、PDR/FAR/
QER/URR、GTP-U 封装或 memif buffer 生命周期。实际日志确认 N3、N6 的 qid 0
和 qid 1 均能独立累计 TX；压力下 `tx-sent == tx-request`，差值发生在
`alloc-request - alloc-granted`，因此可以区分 buffer allocation 短缺和
`memif_tx_burst()` 部分发送。

A/B 使用两个真实 PDU/PFCP Session、两个 Session Worker、1428-byte inner
IPv4 UDP，每档目标持续 10 秒并额外留 2 秒排空。`10us` 将每条流的
`maxframe` 设为每 10 微秒目标包数的向上取整；`burst256` 使用此前默认的
`maxframe=256`。上行报文包含真实 Session 建链产生的 UL TEID 和
PDU Session Container（QFI=1），两方向都执行完整 UPF 语义。

8192-entry ring 在所有档位均将目标包数完整提交到 Open5GS：

| 方向 | 节奏 | 目标 | 输入包 | 输出包 | Open5GS 段丢包 | 输出折算 |
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

16384-entry ring 没有提高可持续吞吐。下表的“提交率”是 12 秒观察窗口内实际
进入对应输入 memif 的包数除以目标包数；段丢包只计算已经进入 Open5GS 的包。
提交率低不是发生器主动少发，而是 memif 输入 ring 长时间不能 refill，VPP
出现 `no free tx slots` 后受到反压：

| 方向 | 节奏 | 目标 | 实际输入 | 提交率 | Open5GS 段丢包 |
|---|---|---:|---:|---:|---:|
| 下行 | 10us | 1.2G | 1,050,420 | 100.000% | 0.021% |
| 下行 | 10us | 2.0G | 1,750,700 | 100.000% | 4.825% |
| 下行 | 10us | 4.0G | 2,411,248 | 68.865% | 0.120% |
| 下行 | burst256 | 1.2G | 747,409 | 71.153% | 0 |
| 下行 | burst256 | 2.0G | 1,243,249 | 71.014% | 0.035% |
| 下行 | burst256 | 4.0G | 2,479,664 | 70.819% | 0.036% |
| 上行 | 10us | 1.2G | 783,539 | 74.593% | 0 |
| 上行 | 10us | 2.0G | 1,207,625 | 68.980% | 0 |
| 上行 | 10us | 4.0G | 2,310,370 | 65.984% | 0.364% |
| 上行 | burst256 | 1.2G | 703,951 | 67.016% | 0 |
| 上行 | burst256 | 2.0G | 1,178,616 | 67.323% | 0 |
| 上行 | burst256 | 4.0G | 2,222,316 | 63.469% | 0.382% |

16384 下 N6 qid 1 的最大 pending 达到约 400ms，并出现
`memif1/0-tx no free tx slots`；上行 4G 也出现 `memif2/0-tx no free tx
slots`。更大的 ring 扩大了单个有效 RX qid 的排队和突发，不能替代真实多 qid
分流，反而降低当前 dispatcher 的 refill 及时性。因此测试部署已恢复 N3/N6
均为 8192。32768 未测试：当前 libmemif 配置边界明确要求
`log2_ring_size <= 14`，强行放宽会超出已验证范围。

## VPP/memif 与 Open5GS 责任隔离（2026-07-26）

本轮不修改UPF语义代码，在相同Pod、VPP 26.06、2 qid、8192-entry ring和
1428-byte IPv4包条件下，分别测试完整UPF路径、VPP queue placement和裸
libmemif slave TX→VPP RX。

### VPP queue placement

正式配置中队列并未挤在同一VPP worker：

- N6 `memif1/0` RX qid 0/1分别位于`vpp_wk_1`、`vpp_wk_2`；
- N3 `memif2/0` RX qid 0/1分别位于`vpp_wk_3`、`vpp_wk_4`；
- Open5GS和VPP使用不同的Guaranteed CPU cpuset，全部为`SCHED_OTHER`。

将N3两个RX qid临时迁移到`vpp_wk_5`、`vpp_wk_6`后，4G/10us下行两次输出
分别为2,964,811和3,113,619包，未优于原placement下同轮约3.18M～3.25M包。
测试后已恢复原placement。因此简单更换VPP RX worker不能解释或消除当前差值。

完整路径中途连续执行`show memif`会触发VPP CLI worker barrier，曾观察到单
ring瞬时积压7,218个描述符，并显著放大丢包；该组受干扰数据不用于吞吐结论。
无中途CLI的测试结束后，N3两个slave-to-master ring均为`head == tail`，VPP
能够把已提交描述符排空。

### 裸memif同方向对照

在当前VPP容器临时创建额外master memif，由UPF容器内临时libmemif客户端使用
两个CPU、两个slave TX qid，执行与Open5GS TX相同方向的
`memif_buffer_alloc(N) → payload copy → memif_tx_burst(N)`。VPP完成IPv4
input/lookup后走临时drop路由；测试接口、socket、路由和客户端随后全部删除。

| 持续时间 | libmemif TX提交 | VPP RX | VPP接收率 | payload吞吐 | tx-short |
|---:|---:|---:|---:|---:|---:|
| 2s | 29,227,520 | 29,218,560 | 99.969% | 166.948G | 0 |
| 10s | 176,116,480 | 176,111,872 | 99.997% | 201.195G | 0 |

最大压力下裸客户端也频繁得到`alloc-short/alloc-zero`，说明allocation短缺本身
只表示生产者瞬时追上ring；客户端重试后仍能持续远超10G，所有获得的buffer
均满足`tx-request == tx-sent`。因此VPP/memif裸消费能力不是当前3～4G完整UPF
结果的带宽上限。

为排除无网线fabric口的ARP异常路径，另在N3 FIB 10中临时为当前gNB
`172.30.180.7/32`安装drop路由。4G/10us两次N3 memif输出为3,169,944和
3,153,437包，与原ARP路径同一波动范围，没有恢复到完整3,501,400包；测试后
已删除drop路由。无网线ARP会增加VPP后续graph成本，但不是主要差值来源。

### Open5GS线程与队列证据

4G/10us压测约14.95秒的`/proc`线程差分：

| 线程 | 绑定 | CPU | 主动上下文切换 | 非主动上下文切换 |
|---|---:|---:|---:|---:|
| Session worker 0 | CPU45 | 45.8% | 869,181（约58.1K/s） | 85 |
| Session worker 1 | CPU46 | 44.4% | 875,606（约58.6K/s） | 41 |
| memif dispatcher | CPU47 | 63.8% | 208,009（约13.9K/s） | 77 |

两个worker并未被CFS持续抢占，也没有跑满CPU；非主动切换仅约3～6次/秒。
大量切换是线程主动阻塞/唤醒。运行计数中N6只有qid 1收包，累计平均RX burst
约2包。dispatcher几乎每个小burst都按Session owner向两个`ogs_queue`投递；
worker空队列时阻塞在`ogs_queue_pop()`的`pthread_cond_wait`，收到小batch后
唤醒，处理并flush TX，再次睡眠。累计约4.3M次worker主动切换与上述模型吻合。

责任结论：

- VPP/memif可持续消费能力已在同配置下确认远高于10G；
- 当前VPP RX placement合理，迁移worker无收益；
- `memif_tx_burst()`已提交包不继续丢；
- allocation短缺是Open5GS小burst睡眠/唤醒和不连续TX输出追上ring的表象；
- 当前首要优化对象是Open5GS的dispatcher→worker投递/唤醒模型，以及短时
  allocation不足时的有界重试，而不是继续扩大ring或修改VPP源码。

## Session Worker 第一阶段批量分类与投递（2026-07-25）

本阶段不实现 descriptor lease，memif RX buffer 仍在 dispatcher 完成 payload
复制后立即 refill。修改只涉及调度方式：

- 一个 N3/N6 `memif_rx_burst()` 只获取一次规则读锁并完成全部 Session 归属
  查询；
- N3 继续按 GTP-U TEID、N6 继续按 UE IP 找到固定 `owner_worker`；
- dispatcher 按 owner 整理索引，每个 worker 每个 RX burst 只入队一个 batch；
- 每个 worker 使用固定大小 SPSC 报文环和预分配 batch 描述符，热路径无
  `malloc`；
- worker 收到第一个 batch 后，非阻塞合并队列中已经到达的 batch，最多累计
  256 包，再统一获取规则读锁并执行 N3/N6 TX begin/flush。这里不等待凑满
  256 包，队列暂时为空就立即处理；
- PDR/FAR/QER/URR、PFCP Session、GTP-U 封装/解封装仍执行原处理函数，未增加
  fast path，也未放宽任何规则条件。

测试仍使用同一 OAI UE 的两个真实 PDU/PFCP Session（`10.45.0.2`、
`10.45.0.3`），inner IPv4 UDP 1428 bytes，两条流各承担一半负载。下表采用
每条流 `maxframe=ceil(PPS/100000)` 的约 10μs 小批量节奏，每档10秒。

| Worker / ring | 目标 inner | N6 memif 输入 | N3 memif 输出 | Open5GS 段丢包 |
|---:|---:|---:|---:|---:|
| 1 / 1 | 600M | 525,210 | 525,210 | 0 |
| 1 / 1 | 1.2G | 1,050,420 | 1,041,590 | 0.841% |
| 2 / 2 | 600M | 525,210 | 525,210 | 0 |
| 2 / 2 | 1.2G | 1,050,420 | 1,043,720 | 0.638% |
| 2 / 2 | 2.0G | 1,750,700 | 1,704,138 | 2.660% |

旧逐包 dispatcher 路径在同一双 Session/10μs口径下，单 Worker 600M 和1.2G
分别丢3.672%和4.833%；新实现分别降为0和0.841%。大 burst 口径下，新单
Worker 1.2G、2.0G分别丢1.060%、2.608%，旧实现分别为2.641%、8.210%。

压力测试后正常停止 UPF，worker0 统计 `2,980,116 packets / 0 drops`，
worker1 为 `2,977,514 / 0 drops`，确认 dispatcher 报文环和 batch 队列没有
丢包。剩余端口差值发生在 worker 完整 UPF 处理后的 N3 TX buffer/ring 节奏；
1.2G 双 Worker 测试同时记录到223次 VPP N6输入侧
`memif1/0-tx no free tx slots`。因此第一阶段已降低分类、任务池原子操作、
队列唤醒和锁开销，但尚未消除 payload copy，也不能据此宣称达到同机10G。

## 双 Session 下行：单 Worker 与双 Worker 对比（2026-07-25）

测试使用同一个 OAI UE 建立两个真实 PDU/PFCP Session，地址为 `10.45.0.2`
和 `10.45.0.3`。OAI UE 使用修复后的独立策略路由表 10001、10002；Open5GS
中同时存在两组 F-SEID、PDR/FAR/QER/URR 和 N3 TEID。inner IPv4 UDP 包长
1428 bytes，两条 Session 各承担一半负载，每档持续10秒。

发生器在正式 VPP 的 `ip4-input` 节点直接注入 N6 流量，以避开 OAI 空口及第三块
VF 的性能限制。`pg0 rx = memif1/0 tx` 表示所有发生包均交给 Open5GS；
`memif2/0 rx` 表示完成完整下行 UPF 语义和 GTP-U 封装的包。Open5GS 段丢包率
按 `(memif1 tx - memif2 rx) / memif1 tx` 计算。N3 物理 fabric 口未插网线，
后续 ARP 节流不计入 Open5GS 丢包。

### 单 Worker / 单 N3、N6 memif ring

| 目标 inner | N6 memif 输入 | N3 memif 输出 | 丢包率 | 实际 inner 输出 |
|---:|---:|---:|---:|---:|
| 200M | 175,070 | 175,070 | 0 | 200M |
| 400M | 350,140 | 350,140 | 0 | 400M |
| 500M | 437,670 | 437,670 | 0 | 500M |
| 550M | 481,440 | 481,440 | 0 | 550M |
| 600M | 525,210 | 518,336 | 1.309% | 592M |
| 800M | 700,280 | 665,103 | 5.023% | 760M |
| 1.0G | 875,350 | 861,335 | 1.601% | 984M |
| 1.2G | 1,050,420 | 1,022,682 | 2.641% | 1.168G |
| 1.5G | 1,313,020 | 1,222,953 | 6.860% | 1.397G |
| 2.0G | 1,750,700 | 1,606,976 | 8.210% | 1.836G |

在 packet-generator 默认 `maxframe=256` 的这组特定 burst 条件下，最高零丢包
档为 **550 Mbps**。600M以上结果并非严格单调，不能与下文使用独立 VCL/VF
均匀发生器测得的旧单 Worker 1.2G零丢包结果直接横向比较，也不能据此认定
单 Worker 的绝对能力下降到550M。

### 发包节奏和 Session 数复核

为限制补偿性突发，另将每条 packet-generator 流的 `maxframe` 设置为
`ceil(该流 PPS / 100000)`，即限制为每10μs目标包数的向上取整。VPP仍使用
时间累加器控制平均速率，该设置限制的是每次 graph 调用的最大包数，不保证
Linux/VPP调度严格每10μs唤醒一次。

| 单 Worker负载 | 流量命中 | maxframe | N6输入 | N3输出 | 丢包率 |
|---:|---|---:|---:|---:|---:|
| 600M | 单 Session | 1 | 525,210 | 512,174 | 2.482% |
| 600M | 双 Session各半 | 1/1 | 525,210 | 505,922 | 3.672% |
| 1.2G | 单 Session | 2 | 1,050,420 | 1,024,690 | 2.449% |
| 1.2G | 双 Session各半 | 1/1 | 1,050,420 | 999,654 | 4.833% |

细粒度平滑没有恢复旧路径的1.2G零丢包，反而使 worker 更难积累批量。双流比
单流进一步增加小 batch 交错，但单 Session流同样丢包，说明主要退化来自新
`dispatcher -> payload copy -> task queue -> worker` 路径破坏原有 memif burst
边界，而不是建立两个 PFCP Session本身。1.2G双流还观察到1,912次 N6 memif
`no free tx slots`，说明小批量处理不及时已向输入 ring 形成反压。

### 双 Worker / 双 N3、N6 memif ring

两条流经 VPP 五元组 hash 分别进入 N6 ring 0、ring 1，两个 PFCP Session 固定
归属不同 Session Worker。

| 目标 inner | N6 memif 输入 | N3 memif 输出 | 丢包率 | 实际 inner 输出 |
|---:|---:|---:|---:|---:|
| 600M | 525,210 | 525,210 | 0 | 600M |
| 800M | 700,280 | 700,280 | 0 | 800M |
| 1.0G | 875,350 | 875,350 | 0 | 1.0G |
| 1.2G | 1,050,420 | 1,050,420 | 0 | 1.2G |
| 1.5G | 1,313,020 | 1,308,957 | 0.309% | 1.495G |
| 2.0G | 1,750,700 | 1,740,250 | 0.597% | 1.988G |
| 2.5G | 2,188,370 | 2,159,441 | 1.322% | 2.467G |
| 3.0G | 2,626,050 | 2,575,871 | 1.911% | 2.943G |
| 4.0G目标 | 3,341,666 | 3,267,139 | 2.230% | 3.732G |

4.0G档中发生器实际只向 N6 memif 提交约 **3.818 Gbps**，因此不能声称已施加
完整4.0G负载。双 Worker 最高确认零丢包档为 **1.2 Gbps**；2.0G 时仍可输出
约1.988G且 Open5GS 段丢包低于1%。与单 Worker 相比，多 Session 分片确实生效，
但 dispatcher 的单线程收包、任务复制及每 Worker 的完整 UPF 逐包处理仍限制扩展。

## 下行 N3 memif buffer 批量预申请（2026-07-25）

本轮仅优化 N6→N3 的 libmemif buffer 申请和复制，不跳过或修改
PFCP/PDR/FAR/QER/URR/GTP-U 语义。N6 仍以 256 包 burst 接收并逐包执行完整
Open5GS 处理；N3 最多一次预申请 64 个连续 TX buffer，直接在其中完成 GTP-U
封装，把 payload copy 从两次降为一次。复杂报文仍执行原 Open5GS 路径，最终
复制到同一批 TX buffer。

VPP 26.06 的 slave TX ring 不能在任意时刻回退未用描述符：ring 接近满时，
`memif_set_next_free_buffer()` 会返回 `Invalid argument`。最终实现不回退
预申请尾部，而是将最多 63 个未用连续 buffer 保留到下一个 N6 burst 使用；
断线时清空本地所有权状态。这样始终保持 libmemif `next_buf`、ring head 和
发送数组连续，不会为批量申请改变或放宽 UPF 功能条件。

测试使用真实 UE `10.45.0.2` 建立 PFCP 会话，由临时 VPP 26.06/VCL Pod 通过
无网线 `fabric_network` VF 生成四条均匀 UDP 流；inner IPv4 包长 1428 bytes。
测试结束后已恢复正式 N6 NAT、删除测试专用 N3 路由和临时 Pod。

| 目标/时长 | 发生器提交 | N6 memif 输入 | N3 memif/VF 输出 | Open5GS 段丢包 | 实际输出 |
|---:|---:|---:|---:|---:|---:|
| 10M/2s | 1,751 | 1,751 | 1,751 | 0 | 10.00M |
| 1.20G/20s | 2,100,824 | 2,100,824 | 2,100,824 | 0 | 1.200G |
| 1.25G/30s | 3,282,635 | 3,282,635 | 3,281,134 | 1,501（0.0457%） | 1.249G |
| 1.30G/20s | 2,275,958 | 2,271,569 | 2,256,229 | 15,340（0.675%） | 1.289G |
| 1.50G/20s | 2,626,046 | 2,610,587 | 2,578,802 | 31,785（1.218%） | 1.473G |

旧镜像在同样 1.20G/20s 测试中，N6 memif 输入 2,091,487 包、N3 输出
2,069,976 包，Open5GS 段丢包 1.029%，实际输出约 1.182G。新实现将该档位
提升为完整 1.200G、零丢包。1.25G 已有 0.0457% 轻微丢包，因此当前严格
零丢包稳定值按 **1.20 Gbps** 记录；过载时可输出约 1.47G，但不能视为稳定
能力。1.30G 以上同时出现 VPP `memif1/0-tx no free tx slots`，说明下一阶段
瓶颈仍是单 worker 完整 UPF 语义和单 memif ring，而不是本次批量描述符游标。

## perf 定位与同步热路径优化（2026-07-21）

本轮不增加 fast path，也不跳过任何 PDR/FAR/QER/URR/PFCP 处理。修改仅包括：

- runtime 从 Meson 默认 debug（`-O0`）改为 `debugoptimized`（`-O2 -g`），保留
  调试符号和断言；
- 保持原规则锁覆盖范围、获取顺序和单 worker 并发模型不变，将 worker 与 PFCP
  主线程之间的 pthread mutex 改为 FIFO ticket lock；解锁由原子 RMW 改为单写者
  release-store。

在专用 memif worker 满载时采集 `cycles:u`，热点对比如下。各轮发生器供给和 VF
丢包有波动，百分比用于定位 CPU 开销，不应直接按比例推算吞吐。

| 构建/同步方式 | lock | unlock | 两次 `memif_poll_event`/`epoll_pwait` | 说明 |
|---|---:|---:|---:|---|
| `-O0` + pthread | 约 6.9% | 约 11.4% | 约 68% | 原始 runtime 实际无优化 |
| `-O2` + pthread | 6.02% | 21.85% | 主要剩余热点 | O2 后 pthread 成为首要可控热点 |
| `-O2` + ticket/RMW unlock | 6.12% | 15.58% | 约 68% | RMW unlock 仍锁总线 |
| `-O2` + ticket/store unlock | 19.89% | <0.1% | 约 67% | 同步总热点降至约 19.9% |

最终版本使用真实 PFCP 会话（UE `10.45.0.2`，本轮 UL TEID `0x1e60`）验证。
10 Mbps/2 秒冒烟测试中，发生器、N3 VF、N3 memif、N6 memif、N6 VF 和接收
VF 均为 1,732 包，Open5GS 日志无错误；同一镜像下连续两次重建 NR-UE，PFCP
会话均完整执行 `Removed -> Added`。并发 ticket lock 另以 4 线程、每线程
2,000,000 次临界区执行压力测试，最终计数和 ticket 均为 8,000,000。

不限速 10 秒测试中，发生器提交 17,095,321 包（inner 19.53 Gbps），N3 VF
实际收到 14,581,151 包，N6 VF 发出 9,506,606 包，对应 N6 inner 约
**10.86 Gbps**。此前同环境 `-O2` + pthread 单次峰值约 9.96 Gbps，本次约提升
9%；但这是无网线 `fabric_network` VF-to-VF 内部交换且为高丢包峰值，不代表
跨服务器 10G 物理口的无损能力。过载差值发生在 Open5GS 向 N6 memif 发送时，
日志明确为 `Ring buffer full`；同时 N3 VF 有 `rx-miss`，不是 CFS throttling
或线程中途被调度走导致。

最终 perf 中约 67% 周期仍消耗在 N3、N6 各一次零超时
`memif_poll_event -> epoll_pwait`。下一阶段优先合并或减少空轮询系统调用；单
memif ring 满载也需要多 queue/会话分片解决。不能为了继续提高数字而绕过
Open5GS UPF 语义。

## Open5GS 专用 memif worker + burst（2026-07-18）

本阶段保留 Open5GS UPF 的全部 PFCP/PDR/FAR/QER/URR/GTP-U 语义，仅优化
Open5GS 与 VPP 26.06 之间的数据面：N3/N6 libmemif 事件从 Open5GS 主事件线程
移到一个专用 worker；每次最多 256 包执行 `memif_rx_burst`、连续规则处理、
一次 `memif_buffer_alloc` 和一次 `memif_tx_burst`。TX 队列保存 `ogs_pkbuf`
引用而不是复制 `ogs_pkbuf`，因此没有为批量队列增加第三次 payload copy；当前
memif RX 到 `ogs_pkbuf`、再到对端 memif region 仍各有一次 payload copy。

PFCP 主线程修改规则和专用 worker 查询规则之间使用同一互斥锁，避免 session、
PDR/FAR/QER/URR 生命周期竞争。`ogs_pkbuf_ref()` 使用原子引用计数，释放路径用
CAS 处理并发递减。N3/N6 共用一个数据面 worker，当前没有并行修改 Open5GS
session/rule 对象。

测试继续使用无网线 `fabric_network` 第三个 VF 发生流量。所有数值均为单次
测试；X710 VF 单队列 `rx-miss` 和临时 VCL sink FIFO 会先饱和，不能把发生器
提交数直接视为 UPF 已收到数。

### 上行 N3 → N6

| 生成 outer | 生成包 | N3 VF/memif | N6 memif/VF | N3 rx-miss | Open5GS 段差值 | 实际 inner |
|---:|---:|---:|---:|---:|---:|---:|
| 10M/2s | 1,732 | 1,732 | 1,732 | 0 | 0 | 9.89M |
| 1,000M/10s | 865,648 | 801,819 | 787,245 | 63,829 | 14,574 | 899.35M |
| 1,200M/10s | 1,038,848 | 954,034 | 954,034 | 84,814 | 0 | 1,089.89M |
| 1,500M/10s | 1,297,732 | 1,157,668 | 1,128,364 | 140,064 | 29,304 | 1,289.04M |
| 2,000M/5s | 865,647 | 789,988 | 721,247 | 75,659 | 68,741 | 1,647.91M |
| 无节流/5s | 4,373,476 | 3,901,276 | 1,749,543 | 472,200 | 严重 ring overload | 3,997.36M |

同为 1 Gbps/10 秒时，旧逐包实现 N3 到 N6 丢 42,638 包，新实现丢 14,574
包，Open5GS 段丢包减少约 66%，实际 inner 从 871.86M 提升到 899.35M。
1.2 Gbps 档中，包一旦进入 N3 VF，Open5GS、N6 memif 和 N6 VF 的 954,034
包完全一致，说明 burst 路径已经消除该档位的 Open5GS 段丢包。无节流约
4.0 Gbps 只是高丢包峰值，不是稳定吞吐。

### 下行 N6 → N3

首次下行测试发现 N3 memif 只有 FIB 10、没有启用 IPv4，Open5GS 已送入 N3
memif 的 1,751 包全部在 VPP `ip4-not-enabled` 丢弃。Helm 配置现已将 N3
memif unnumbered 到 `dpdk-n3`。修复后 10M/2s 测试中，N6 VF、N6 memif、
N3 memif、N3 VF 和接收 VF 均为 1,751 包，验证了反向 burst 及 GTP-U 封装。

1 Gbps/10 秒单次下行中，发生器提交 875,348 包，N6 VF 收到 653,722 包
（`rx-miss=221,626`），N3 memif/N3 VF 发出 653,463 包，VPP/Open5GS 段只
再少 259 包，实际 inner 约 746.52M。该数据主要反映 N6 VF RX 和临时 VCL
sink 已过载，不代表下行低丢包上限。

当前结论：worker/burst 改造可行且已同时覆盖 N3/N6 上下行；1.2G 上行档
消除了 Open5GS 段丢包，但单 memif ring、单数据面 worker、两次 payload copy
以及 VF 单 flow/单 RX queue 仍限制同机 10G。下一阶段若继续冲击 10G，应先
引入多 memif queue 和多会话分片，并让每个数据面 worker 独占 session/rule
分片，避免用一个全局规则锁串行化。

## fabric_network 无网线 VF 内部交换（2026-07-18）

将正式 xcn VPP 的两个 VF 从 `intel.com/external_network` 切换为
`intel.com/fabric_network`。该资源对应 PF `ens5f1`，物理口未插网线且 PF
administratively DOWN；VF 被设置为 `link-state enable`，VPP 显示
`carrier up`、`Link speed: unknown`。测试额外创建一个临时 VPP/VCL Pod，
使用同一 PF 的第三个 fabric VF 发送 GTP-U 并接收 N6 包，全程走 X710
内部 VF-to-VF switching，不经过物理线缆。

真实 UE 建立会话后，发生器使用当前 UL TEID。内层目的使用静态邻居加
drop route，确保 N6 包确实发送到接收 VF，同时不产生 ICMP/UDP 回包。

| 生成 outer | 生成包 | N3 VF/memif | N6 VF TX | N3 rx-miss | N3/N6 ring 差值 | 实际 inner |
|---:|---:|---:|---:|---:|---:|---:|
| 10M/2s | 1,732 | 1,732 | 1,732 | 0 | 0 | 9.89M |
| 794.9M | 688,109 | 671,640 | 656,757 | 16,469 | 14,883 | 750.28M |
| 1,000M | 865,648 | 805,820 | 763,182 | 59,828 | 42,638 | 871.86M |
| 1,160M | 1,006,038 | 957,293 | 943,670 | 48,745 | 13,623 | 1,076.43M |
| 2,085M/5s | 950,698 | 899,106 | 882,047 | 51,592 | 17,059 | 1,913.14M |

无节流峰值测试总损失约 7.2%，但实际 inner 已达到约 1.91 Gbps，证明之前
external 测试的约 0.85 Gbps 峰值确实受到外部 1 Gbps 链路条件影响，而不是
fabric VF 或 memif 固定只能达到 1 Gbps。各档单次结果有明显波动，不应把
1.91 Gbps 当作无损能力。

当前剩余热点分为两段：N3 单 GTP-U flow 所在 VF RX queue 的 `rx-miss`，以及
VPP N3 memif 到 Open5GS、Open5GS 到 N6 memif 的单 ring/逐包处理。下一阶段
应优先实现多 memif queue 与 libmemif burst，再配合多会话/多流 RSS 验证，
而不是继续增加空闲 VPP worker。

## external_network + N3/N6 双 memif + 双 X710 VF（2026-07-18）

本轮将 N3 和 N6 都改为独立 SR-IOV VF + VPP raw-IP memif。Open5GS UPF
继续负责 PFCP、GTP-U 解封装和 PDR/FAR/QER/URR 语义；VPP 负责 N3/N6 高速
收发、N3 独立 FIB 10、N6 NAT44。两个 iAVF 各配置 4 RX/4 TX queue、4096
descriptor，两个 memif ring 均为 8192。VPP/UPF 各使用 8 个独占 CPU，VPP
使用 8 GiB 大页、8 GiB main heap 和 16 GiB regular memory。

需要注意：虽然物理环境预期为 X710 10G，本次 VPP 实际报告 N3/N6 两个 VF
的 `Link speed` 都只有 **1.000 Gbps**。因此本轮可验证架构和当前 1G 链路下
的峰值，不能据此声称已验证同机 10G。

测试使用真实 UE 建立 `10.45.0.2` 会话，再由 gNB Pod 内的 `gtpu_gen` 直接
向 N3 VIP `10.2.0.226:2152` 注入当前 UL TEID，内层目标为本机静默 UDP
sink `10.2.0.119:9999`。包长为 outer 1444 bytes、inner IP 1428 bytes。

| 生成 outer | 生成包 | N3 memif | N6 memif/N6 VF TX | N3 VF rx-miss | 实际 inner | 结论 |
|---:|---:|---:|---:|---:|---:|---|
| 10M/2s | 1,732 | 1,732 | 1,732 | 0 | 9.89M | 端到端逐包一致 |
| 600M | 519,399 | 475,161 | 475,161 | 44,244 | 542.8M | VF 后无额外丢包 |
| 800M | 692,519 | 545,415 | 545,415 | 147,121 | 623.1M | VF 后无额外丢包 |
| 1,000M | 865,651 | 750,510 | 746,220 | 115,143 | 852.4M | 饱和峰值，开始出现 ring 压力 |

1 Gbps 饱和测试中，VPP N3 memif 出现 432 次 `no free tx slots`，N3/N6
memif 之间另有约 4,290 包差值；N6 memif 与 N6 VF TX 仍一致。N6 memif
逐包失败日志已改为每秒最多一条汇总，10 秒压力测试只输出 2 条 warning，
不会因逐包刷日志放大过载。VPP main heap 峰后使用 3.57 GiB/8 GiB。

NAT44 固定到 `vpp_wk_2`，与 `memif1/0` RX queue 同 worker 后，先前的 NAT
handoff congestion 已消失。当前首要限制是 N3 单 GTP-U 流落在单 VF RX
queue 并产生 `rx-miss`；达到饱和后才出现单 ring/Open5GS 逐包处理压力。
要验证 10G，需先修复 PF/VF 链路协商或资源配置，使 VF 实际报告 10 Gbps，
然后再考虑多 memif queue、libmemif burst 和 Open5GS 多 worker/会话分片。

## VPP 26.06 + memif + X710 VF（2026-07-18）

拓扑保留 Open5GS UPF 的 PFCP/PDR/FAR/QER/GTP-U 语义，仅将 N6 TUN 替换为
raw-IP libmemif；VPP 26.06 sidecar 使用 `intel.com/external_network` X710 VF、
NAT44 和 1 RX/1 TX queue。测试仍用真实 UE 建立会话，再由 `gtpu_gen` 直接
注入当前 UL TEID，绕过 OAI 空口。

功能验证：

- VPP 与 Open5GS 协商 4096-entry 双向 memif ring，断线后可自动重连。
- UE `10.45.0.2` 到 `8.8.8.8` ping 5/5；NAT 映射为 `10.2.0.224`。
- 端到端 OAI TCP 仅约 UL 125M / DL 209M，瓶颈在 OAI，不作为核心网能力。

绕空口上行结果（1428-byte inner IP，10 秒）：

| UPF CPU | 生成 inner | UDP socket drop | memif/VPP 额外 drop | 结论 |
|---:|---:|---:|---:|---|
| 1 | 790.60M | 95/692161，0.0137% | 0 | pass |
| 1 | 988.92M | 376/865651，0.0434% | 0 | pass |
| 1 | 1189.92M | 7058/1041594，0.678% | 0 | 接近拐点 |
| 1 | 1371.53M | 70591/1200572，5.88% | 0 | overload |
| 2 | 988.88M | 323/865613，0.0373% | 0 | pass |
| 2 | 1149.60M | 4227/1006299，0.420% | 0 | 接近拐点 |
| 2 | 1418.51M | 69931/1241717，5.63% | 0 | overload |

2 CPU 实际 cpuset 为同一物理核的两个超线程。所有持续压测的丢包都对应
Linux `UdpRcvbufErrors`；包进入 memif 后未观察到额外丢包。当前逐包
`recvfrom -> PFCP rule -> memif_buffer_alloc/tx` 实现的低丢包边界约 1.0~1.15G，
说明单纯替换 TUN 尚不足以达到同机 10G。下一步需要对 N3 使用
`recvmmsg`/多队列，并对 memif TX/RX 做真正的 burst 批处理，再考虑多 UPF
worker/会话分片。

测试日期：2026-07-08

环境：

- Open5GS UPF hostNetwork: `10.2.0.119`
- UE PDU address: `10.45.0.2`
- N6/TUN gateway: `10.45.0.1`
- 包大小：
  - 下行 `udp_gen` IP 包长 `1428` bytes
  - 上行 `gtpu_gen` outer GTP-U 包长 `1444` bytes，inner IP 包长 `1428` bytes
- 每组压测时长：`10s`
- 计数口径：
  - 下行用 `udp_gen sent_pkts` 对比 `ogstun tx_packets`/GTP-U OUTPUT。
  - 上行用 `gtpu_gen sent_pkts` 对比 `ogstun rx_packets`。
  - 并发时同时观察 `ogstun tx/rx`、iptables GTP-U INPUT/OUTPUT、`Udp InErrors`、softnet。

## 结论摘要

- UPF 从 `1.5 CPU` 提升到 `2 CPU` 后，下行从约 `500-570Mbps` 明显提升到 `1.25Gbps` 无丢包。
- UPF `2 CPU` 时，上行低丢包能力约 `1.7Gbps`，高于下行。
- UPF `2 CPU` 时，并发低丢包点约 `DL 700M + UL 700M`；`DL 1G + UL 500M` 基本能跑住。
- UPF 降到 `1 CPU` 后，下行和并发退化明显；`DL 1G + UL 500M` 下行丢包约 `26.5%`。
- 测试期间 cgroup `nr_throttled=0`，没有观察到 CFS throttling。
- 当前 Open5GS 数据面没有按 AMBR/QER MBR 做实际限速，超过订阅 `1Gbps` 属于预期行为。

## 2 CPU 结果

配置：

```yaml
fivegc:
  upf:
    requests:
      cpu: 2
      memory: 512Mi
    limits:
      cpu: 2
      memory: 2Gi
  smf:
    requests:
      cpu: 1
      memory: 256Mi
    limits:
      cpu: 1
      memory: 512Mi
```

CPU quota:

```text
quota/period = 200000/100000
nr_throttled = 0
```

### Downlink only

| Target | Sent packets | GTP-U/ogstun output | Drop | Result |
|---:|---:|---:|---:|---|
| 500M | 437675 | 437675 | 0 | pass |
| 800M | 700280 | 700280 | 0 | pass |
| 1.0G | 875346 | 872771 | 2575 | ~0.29% drop |
| 1.2G | 1050416 | 1050416 | 0 | pass |
| 1.25G | 1094208 | 1094208 | 0 | pass |
| 1.3G | 1138041 | 991566 | 146475 | overload |
| 1.5G | 1313024 | 877528 | 435496 | overload |

低丢包下行边界在 `1.25G ~ 1.3G` 之间。

### Uplink only

| Target | Sent packets | ogstun rx | Drop | Result |
|---:|---:|---:|---:|---|
| 500M | 432824 | 432824 | 0 | pass |
| 800M | 692519 | 692418 | 101 | ~0.015% drop |
| 1.2G | 1038850 | 1038741 | 109 | ~0.01% drop |
| 1.5G | 1298526 | 1298169 | 357 | ~0.028% drop |
| 1.7G | 1471667 | 1468051 | 3616 | ~0.25% drop |
| 1.8G | 1558352 | 1531559 | 26793 | ~1.7% drop |
| 2.0G | 1731297 | 1525352 | 205945 | overload |

低丢包上行边界约 `1.7Gbps`。

### Bidirectional

| Target | DL effective | DL drop | UL effective | UL drop | Result |
|---:|---:|---:|---:|---:|---|
| 500D + 500U | ~500M | 0% | ~494M | ~0.14% | stable |
| 700D + 700U | ~700M | 0% | ~690M | ~0.39% | low-drop boundary |
| 725D + 725U | ~683M | ~5.8% | ~679M | ~5.4% | overload begins |
| 750D + 750U | ~668M | ~11% | ~660M | ~11% | overload |
| 800D + 800U | ~699M | ~12.6% | ~689M | ~12.9% | overload |
| 700D + 900U | ~700M | 0% | ~854M | ~4.1% | high aggregate, lossy UL |
| 650D + 900U | ~650M | 0% | ~866M | ~2.7% | asymmetric candidate |
| 650D + 1000U | ~650M | 0% | ~925M | ~6.4% | lossy UL |
| 1000D + 500U | ~999.7M | ~0.03% | ~492M | ~0.42% | stable |

## 1 CPU 结果

当前提交中的 `helm/xcn/values.yaml` 记录的是这组配置：

```yaml
fivegc:
  upf:
    requests:
      cpu: 1
      memory: 512Mi
    limits:
      cpu: 1
      memory: 2Gi
  smf:
    requests:
      cpu: 1
      memory: 256Mi
    limits:
      cpu: 1
      memory: 512Mi
```

CPU quota:

```text
quota/period = 100000/100000
nr_throttled = 0
```

### Downlink only

| Target | Sent packets | Output packets | Drop | Result |
|---:|---:|---:|---:|---|
| 500M | 437673 | 437673 | 0 | pass |
| 800M | 700278 | 700278 | 0 | pass |
| 1.0G | 875350 | 865851 | 9499 | ~1.1% drop |
| 1.2G | 1050416 | 1000234 | 50182 | ~4.8% drop |

### Uplink only

| Target | Sent packets | ogstun rx | Drop | Result |
|---:|---:|---:|---:|---|
| 800M | 692519 | 692183 | 336 | ~0.05% drop |
| 1.2G | 1038850 | 1038831 | 19 | pass |
| 1.5G | 1298526 | 1297027 | 1499 | ~0.12% drop |
| 1.8G | 1558358 | 1508037 | 50321 | ~3.2% drop |

### Bidirectional

| Target | DL effective | DL drop | UL effective | UL drop | Result |
|---:|---:|---:|---:|---:|---|
| 1000D + 500U | ~735M | ~26.5% | ~493M | ~0.04% | DL overload |
| 700D + 700U | ~690M | ~1.4% | ~684M | ~1.3% | usable, worse than 2 CPU |

## AMBR note

The test subscription used:

```javascript
ambr: {
  downlink: { value: 1, unit: 3 },
  uplink: { value: 1, unit: 3 }
}
```

SMF copies Session AMBR into PFCP QER MBR and sends it to UPF, but current Open5GS UPF data plane does not enforce `qer->mbr` with policing or shaping. Therefore bypass tests can exceed `1Gbps`.
## Session 多 Worker 第一阶段（2026-07-25）

- Open5GS 增加独立 memif dispatcher 和可配置的 `1..16` Session Worker。
- Session 建立时采用双候选最少连接算法固定 owner；N3 按 TEID、N6 按 UE IP
  分发，Worker 仍执行完整 PDR/FAR/QER/URR 和 GTP-U 处理。
- 控制面使用读写屏障，PFCP/定时器修改独占写锁；数据 Worker 可并行持有读锁。
- 每个 Worker 独占 N3/N6 memif TX ring 和批量状态，分发任务使用固定对象池，
  不执行每包 `malloc/free`。
- VPP 26.06 与 Open5GS 已实际协商2组 N3 和2组 N6 ring，每组8192描述符；
  两个 Worker 均持续运行且调度策略为 `SCHED_OTHER`。
- 进程等待 CPUManager 最终下发不含 CPU0 的 Guaranteed cpuset 后再绑核。本轮
  Pod cpuset 为 `49-52,121-124`，Worker 0、Worker 1、dispatcher 分别实际绑定
  CPU 49、50、51；主线程和控制线程保留完整 Pod cpuset，全部仍为 `SCHED_OTHER`。
- 真实 UE `10.45.0.2` 建立 PFCP Session 后，从 VPP N6 注入100包，N6 memif
  发送100包、Open5GS完成下行语义与GTP-U封装、N3 memif接收100包，无 Open5GS
  丢包。N3 VF因 fabric物理口未接线发生ARP节流，不能作为Open5GS丢包计算。
- 当前现场只有一个活动 Session，不能据此给出2/4 Worker吞吐扩展比；正式性能
  验收需要至少 `max(2W,16)` 个真实 PFCP Session。

## SPSC batch ring 与 busy-poll 回退结论（2026-07-26）

曾将每 Worker 的 `ogs_queue + mutex + pthread_cond` 替换为独占 SPSC batch
ring，并测试0/20/50us混合轮询。两个真实Session、8192 ring、1428-byte inner
IPv4、10us发包、目标1.2G的有效结果：

| busy-poll | 方向 | 实际输入 | 输出 | Open5GS段丢包 | 实际输出 |
|---:|---|---:|---:|---:|---:|
| 0us | 下行 | 899,483 | 899,483 | 0 | 1.028G |
| 0us | 上行 | 894,571 | 894,571 | 0 | 1.022G |
| 20us | 下行 | 1,050,420 | 1,037,168 | 1.262% | 1.185G |
| 20us | 上行 | 1,050,420 | 1,033,922 | 1.571% | 1.181G |
| 50us | 下行 | 1,006,854 | 1,000,943 | 0.587% | 1.143G |
| 50us | 上行 | 1,050,420 | 947,177 | 9.829% | 1.082G |

0us发生器未实际提交满1.2G，不能记为1.2G零丢包。20us在双Session 1.2G时
已覆盖约19us的平均单Session包间隔，持续流量下接近一直忙轮询，但仍低于原路径
下行1.2G零丢包、上行约0.117%丢包的基线。Worker ring/drop虽为零，快速消费
小batch却减少了原睡眠形成的隐式聚合，恶化后续TX buffer申请/flush节奏。

因此提交`3fd65501a`已由`ce76310e8`回退，运行配置恢复原队列/条件变量路径。
结论是不能将“减少锁和唤醒”等同于吞吐提升，也不应继续尝试无限忙轮询；后续
必须先观测并优化实际batch大小与TX申请/flush节奏。

回退镜像`sha256:8eb5e0c9e1964d77601322b618597a23f763f28c0da6abcebbb8f8ad3c150dab`
部署后，两个真实Session重新建立；100M/2秒下行输入/输出均为17,506包，
Worker分别处理8,754/8,752包，`queue-full/push-fail/drop=0`。

## 双 dispatcher + descriptor lease（2026-07-27）

本轮实现以下数据面结构，未修改 VPP 源码：

- N3、N6 各一个专用 dispatcher、epoll 和 CPU；
- N3 按 TEID、N6 按 UE IP 查找 Session owner；
- 普通 G-PDU/IP 报文由 dispatcher 向 Worker 传递 memif RX descriptor 引用，
  不再复制到 task payload；
- Worker 继续执行原来的 PDR/FAR/QER/URR、GTP-U 封装/解封装和批量 TX；
- Worker 完成后标记 completion，入口 dispatcher 按 RX sequence 批量
  `memif_refill_queue()`；
- generation ID 隔离断线前后的旧 completion；控制/异常报文采用复制回退；
- 两个方向可同时向同一 Session Worker 投递，因此用短临界区保护每个 Worker
  的 task/batch 发布；每个 Worker 仍独占对应的 N3/N6 TX qid。

最终验证镜像为`xcn-runtime:descriptor-lease-final`
（Docker image ID `sha256:80b385b5d86ab80899daf8f0da5fb977a49564faabaf6be12fdff9921576b45e`）。

功能冒烟使用两个真实 PFCP Session（`10.45.0.2`、`10.45.0.3`）：
100M、2秒下行输入/输出均为17,506包，两个 Worker 分别处理8,752/8,754包；
dispatcher、Worker queue、refill、TX 均无错误，N6最大在途140，测试结束
`in-flight=0`。这证明 descriptor 生命周期和原 UPF 语义路径可以正常闭环。

### 下行压力结果

两个 Session、两个 Worker、两个配置 qid（入口仍只有 qid 1 有流量）、
8192-entry ring、1428-byte inner IPv4、每档10秒。输入/输出均取 VPP memif
接口实际计数，不使用目标包数替代实际提交数。

`busy_poll_us=20`：

| 节奏 | 目标 | 实际输入 | 输出 | Open5GS段丢包 |
|---|---:|---:|---:|---:|
| 10us | 1.2G | 1,050,420 | 1,050,420 | 0 |
| 10us | 2.0G | 1,750,700 | 1,725,778 | 1.424% |
| 10us | 4.0G | 3,501,400 | 3,391,246 | 3.146% |
| burst256 | 1.2G | 1,050,420 | 1,048,176 | 0.214% |
| burst256 | 2.0G | 1,703,378 | 1,700,468 | 0.171% |
| burst256 | 4.0G | 3,501,400 | 3,436,804 | 1.845% |

2.0G burst256 的发生器只实际提交1,703,378包，因此不能记为“2G仅丢
0.171%”。当前严格零丢包仍只确认到下行1.2G/10us，descriptor lease 没有
把稳定能力直接提升到2G。

### Worker 等待策略 A/B

下表为相同 descriptor-lease 版本的下行10us结果。4G档位若发生器未发满，
“实际输入”按实际 memif TX 计数记录。

| 模式 | 目标 | 实际输入 | 输出 | Open5GS段丢包 |
|---|---:|---:|---:|---:|
| 阻塞（0） | 1.2G | 1,050,420 | 1,046,059 | 0.415% |
| 阻塞（0） | 2.0G | 1,750,700 | 1,714,796 | 2.051% |
| 阻塞（0） | 4.0G | 3,435,663 | 3,253,457 | 5.304% |
| 混合轮询（20us） | 1.2G | 1,050,420 | 1,050,420 | 0 |
| 混合轮询（20us） | 2.0G | 1,750,700 | 1,725,778 | 1.424% |
| 混合轮询（20us） | 4.0G | 3,501,400 | 3,391,246 | 3.146% |
| 持续忙轮询（-1） | 1.2G | 1,050,420 | 1,045,951 | 0.425% |
| 持续忙轮询（-1） | 2.0G | 1,750,700 | 1,745,831 | 0.278% |
| 持续忙轮询（-1） | 4.0G | 3,358,789 | 3,286,186 | 2.162% |

持续忙轮询的5秒重复测试也存在波动：1.2G三次丢包
`0/0/0.327%`，2G三次为`0.090/0.187/0.951%`。它能降低部分档位的唤醒
延迟，但会令部分档位和发生器提交率变差，不能解决 descriptor 回收和 TX
buffer allocation 压力，因此最终运行配置恢复20us混合轮询，所有线程继续
使用`SCHED_OTHER`。

### 上行与剩余限制

上行使用本轮真实 Session 建立产生的两组 UL TEID；每次 UPF/gNB 重建后重新
获取。20us混合轮询的实际 Open5GS memif 段结果为：

| 节奏 | 目标 | 实际输入 | 输出 | Open5GS段丢包 |
|---|---:|---:|---:|---:|
| 10us | 1.2G | 967,934 | 961,352 | 0.680% |
| 10us | 2.0G | 1,737,634 | 1,669,070 | 3.946% |
| 10us | 4.0G | 3,289,028 | 3,126,680 | 4.936% |
| burst256 | 1.2G | 897,148 | 895,004 | 0.239% |
| burst256 | 2.0G | 1,551,001 | 1,551,001 | 0 |
| burst256 | 4.0G | 2,871,613 | 2,818,561 | 1.847% |

这些上行档位发生器均不同程度未发满，尤其 burst 模式，不能作为目标速率的
稳定能力声明；这里只用于定位 Open5GS 段。

运行计数确认：

- dispatcher drop、Worker queue-full/push-fail/drop、RX/refill/stale 均为0；
- 两个 Worker 处理包数近似各50%，每个 Worker 使用独立 TX qid；
- N3 RX `in-flight-max=8192`，达到当前单个有效入口 ring 上限；
- 压力差值对应输出方向 `memif_buffer_alloc()` 短分配/失败，或 VPP入口
  `no free tx slots`，不是 `memif_tx_burst()` 成功后继续丢包；
- 入口仍只有 qid 1 工作。单 qid 的 descriptor 必须按原 RX 顺序 refill，
  两个 Session 跨 Worker 完成时会形成有序回收的 head-of-line。

因此本轮完成了“消除 dispatcher payload copy”的目标，但没有解决单有效入口
qid、跨 Worker 有序回收和输出 TX ring 可用性的根因。下一步若继续优化，应先
减少 completion 扫描/epoll 空轮询开销并实现有界的完成批量回收；更根本的扩展
仍需要利用 VPP 现有能力将不同 Session 导入不同 RX qid，使
`RX qid -> owner Worker -> TX qid` 对齐。不能通过无限忙轮询或继续增大单 ring
来替代真实多 qid 分流。

## 六 Session / 六 Worker（2026-07-27）

为避免少量 Session 在原“两候选最少连接”算法下只覆盖部分 Worker，新 Session
owner 改为从全部 Worker 中选择当前 Session 数最少者，SEID 仅用于相同负载时
打散起始位置。该修改只决定 Session 固定归属，不改变 PDR/FAR/QER/URR、GTP-U
封装/解封装或 PFCP 语义。

验证镜像为 `xcn-runtime:six-worker-balance`，Docker image ID
`sha256:dc47f8c2c1a064f690fbdc9f37dd57094e4c320d3cbc97186efb3f7d040939ea`。
配置如下：

- 6 个 PFCP Session、6 个 Session Worker；
- N3/N6 均配置 6 个 memif qid、8192-entry ring；
- N3/N6 各一个专用 dispatcher，Worker `busy_poll_us=20`；
- UPF Guaranteed Pod 获得 8 个独占逻辑 CPU，线程为 `SCHED_OTHER`；
- 1428-byte inner IPv4，单方向5秒，内部 VPP packet-generator；
- 测试不经过未插网线的 fabric 物理口，ARP/NAT/VF 下游计数不计入
  Open5GS memif 段丢包。

OAI UE 同时请求6个 Session 时，gNB 和 UPF 均成功建立6组 DRB/TEID/PFCP
Session，但 UE 只接受4个 NAS PDU Session Accept，另2个因并发下行 NAS
完整性计数失败被丢弃。顺序 telnet 追加到第3个 Session 时当前 OAI UE 镜像
还会触发 stack buffer overflow。因此下表是“核心网内已有6个真实 PFCP
Session，发生器绕开 UE/gNB 空口”的 UPF 性能，不代表 OAI UE 已能稳定暴露
6个业务 TUN。

100M冒烟输入/输出均为26,259包。Worker 0..5 分别处理
`4377/4377/4377/4377/4374/4377` 包，六个 N3 TX qid 均有对应流量，
dispatcher/Worker/TX 均零 drop，证明6个 Session 已一一归属6个 Worker。

### 下行

`memif1/0 tx` 为 N6 实际送入，`memif2/0 rx` 为 N3 实际输出：

| 节奏 | 目标 | 实际送入 | 实际输出吞吐 | 段丢包 |
|---|---:|---:|---:|---:|
| 10us | 1.2G | 1.2000G | 1.2000G | 0 |
| 10us | 2.0G | 1.9999G | 1.9999G | 0 |
| 10us | 4.0G | 4.0000G | 3.9901G | 0.2471% |
| 10us | 6.0G | 6.0000G | 5.8624G | 2.2935% |
| 10us | 8.0G | 8.0000G | 7.6638G | 4.2031% |
| burst256 | 1.2G | 1.2000G | 1.2000G | 0 |
| burst256 | 2.0G | 2.0000G | 2.0000G | 0 |
| burst256 | 4.0G | 4.0000G | 3.9632G | 0.9212% |
| burst256 | 6.0G | 6.0000G | 5.9931G | 0.1151% |
| burst256 | 8.0G | 8.0000G | 7.8513G | 1.8590% |

短测存在调度波动，不能据单次 burst256 6G 的0.1151%认定稳定6G。10秒
10us复测中发生器没有跑满目标：4G档实际送入3.414G、输出3.414G，仅丢127包
（0.0043%）。因此当前可严格声明的下行零丢包能力为2G；约3.4G可达到近零
丢包，但稳定边界仍需多轮长测。

### 上行

`memif2/0 tx` 为 N3 实际送入，`memif1/0 rx` 为 N6 实际输出：

| 节奏 | 目标 | 实际送入 | 实际输出吞吐 | 段丢包 |
|---|---:|---:|---:|---:|
| 10us | 1.2G | 1.2000G | 1.2000G | 0 |
| 10us | 2.0G | 2.0000G | 2.0000G | 0 |
| 10us | 4.0G | 4.0000G | 4.0000G | 0 |
| 10us | 6.0G | 5.9995G | 5.9609G | 0.6429% |
| 10us | 8.0G | 6.9267G | 6.8957G | 0.4469% |
| burst256 | 1.2G | 1.2000G | 1.2000G | 0 |
| burst256 | 2.0G | 2.0000G | 2.0000G | 0 |
| burst256 | 4.0G | 4.0000G | 3.9489G | 1.2773% |
| burst256 | 6.0G | 6.0000G | 5.9436G | 0.9397% |
| burst256 | 8.0G | 8.0000G | 7.8963G | 1.2958% |

当前可严格声明的上行零丢包能力为4G。8G burst256 已实际送满，但有1.2958%
段丢包；10us 8G档只送入6.9267G，不能作为8G验证。

累计计数显示六个 Worker 各处理约721万包且 `drop/queue-full/push-fail=0`，
负载近似均分；但 N3 和 N6 仍都只有 RX qid 1 有流量，其
`in-flight-max=8192` 均达到 ring 上限，其余5个 RX qid 为0。压力档的差值
对应 VPP 入口 `no free tx slots`，不是 Worker 执行完整 UPF 语义时主动丢包。
因此六 Worker 已解决 Session 处理核并行问题，当前首要限制转为单有效入口
qid 的 descriptor 有序回收/反压。将 `in-flight-max` 改成16M没有意义：
descriptor 数受当前8192-entry memif ring约束，除非真正把 Session 流量分散到
多个 RX qid。

## 6 Worker下1 Session/6 Session A/B（2026-07-30）

本轮在相同运行实例上固定6个Session Worker、N3/N6各6个配置qid和8192-entry
ring，只改变发生器实际灌入1个还是6个已建立的PFCP Session。UPF使用8个
CPUManager独占逻辑CPU，线程保持`SCHED_OTHER`和20us混合轮询。报文为
1428-byte inner IPv4 UDP，采用10us均匀节奏，每档5秒。

上行TEID不是复用旧pcap：本轮重新抓取PFCP Session Establishment，按PDR 2/4
的Access侧F-TEID为`10.45.0.8..13`生成6份新pcap。1Mbps冒烟174包全部通过，
6个Worker计数近似均分且内部drop为0。正式测试中所有档位的packet-generator
均实际送满目标包数。

计数口径：

- 下行输入为`memif1/0 tx`，Open5GS输出为`memif2/0 rx`。
- 上行输入为`memif2/0 tx`，Open5GS输出为`memif1/0 rx`。
- 吞吐按inner 1428字节计算；未插线fabric VF之后的ARP/NAT丢包不计入。

### 1 Session，6 Worker配置

只有`10.45.0.8`收到测试流量，因此该Session固定owner Worker承担实际语义
处理，其余5个Worker空闲。

| 方向 | 目标/实际输入 | 输入包 | 实际输出 | 输出包 | Open5GS段丢包 |
|---|---:|---:|---:|---:|---:|
| 下行 | 0.5G | 218,835 | 0.5000G | 218,835 | 0 |
| 下行 | 0.6G | 262,605 | 0.5900G | 258,238 | 1.6630% |
| 下行 | 0.7G | 306,370 | 0.6786G | 297,017 | 3.0528% |
| 下行 | 0.8G | 350,140 | 0.7871G | 344,485 | 1.6151% |
| 下行 | 1.2G | 525,210 | 1.1955G | 523,222 | 0.3785% |
| 下行 | 2.0G | 875,350 | 1.7833G | 780,496 | 10.8361% |
| 下行 | 4.0G | 1,750,700 | 3.8136G | 1,669,120 | 4.6599% |
| 下行 | 6.0G | 2,626,050 | 5.5406G | 2,425,001 | 7.6559% |
| 下行 | 8.0G | 3,501,400 | 7.6468G | 3,346,834 | 4.4144% |
| 下行 | 10.0G | 4,376,750 | 9.4353G | 4,129,601 | 5.6469% |
| 上行 | 1.2G | 525,210 | 1.2000G | 525,210 | 0 |
| 上行 | 2.0G | 875,350 | 2.0000G | 875,350 | 0 |
| 上行 | 4.0G | 1,750,700 | 4.0000G | 1,750,700 | 0 |
| 上行 | 6.0G | 2,626,050 | 6.0000G | 2,626,050 | 0 |
| 上行 | 8.0G | 3,501,400 | 8.0000G | 3,501,400 | 0 |
| 上行 | 10.0G | 4,376,750 | 10.0000G | 4,376,750 | 0 |

单Session下行波动明显：额外的1.0G和1.2G重复测试分别输出0.9655G/
1.1617G、丢包3.4537%/3.1879%，所以不能把首次1.2G的0.3785%当成稳定边界。
0.5G重复测试仍为218,835包输入、218,835包输出，因此本轮可重复的严格零丢包
边界只确认到0.5G。高压时能输出9.4353G是有5.6469%丢包的过载吞吐，不代表
稳定支持9.4G。

单Session上行10G又重复两次，均为4,376,750包输入、4,376,750包输出，三次
均零丢包。因此本测试口径下可确认Open5GS内部N3 memif到N6 memif的单Session
上行达到10G零丢包；该结论不包含未插线VF的物理端到端链路。

### 6 Session，6 Worker

总速率平均分配到`10.45.0.8..13`，每个Session固定归属不同Worker。

| 方向 | 目标/实际输入 | 输入包 | 实际输出 | 输出包 | Open5GS段丢包 |
|---|---:|---:|---:|---:|---:|
| 下行 | 1.2G | 525,210 | 1.2000G | 525,210 | 0 |
| 下行 | 2.0G | 875,350 | 2.0000G | 875,350 | 0 |
| 下行 | 4.0G | 1,750,700 | 3.9719G | 1,738,412 | 0.7019% |
| 下行 | 6.0G | 2,626,050 | 5.7548G | 2,518,746 | 4.0861% |
| 下行 | 8.0G | 3,501,400 | 7.6724G | 3,358,015 | 4.0951% |
| 下行 | 10.0G | 4,376,750 | 9.6151G | 4,208,301 | 3.8487% |
| 上行 | 1.2G | 525,210 | 1.2000G | 525,210 | 0 |
| 上行 | 2.0G | 875,350 | 2.0000G | 875,350 | 0 |
| 上行 | 4.0G | 1,750,700 | 3.9903G | 1,746,457 | 0.2424% |
| 上行 | 6.0G | 2,626,050 | 5.9234G | 2,592,518 | 1.2769% |
| 上行 | 8.0G | 3,501,400 | 7.7975G | 3,412,751 | 2.5318% |
| 上行 | 10.0G | 4,376,750 | 9.7123G | 4,250,852 | 2.8765% |

重复测试中，下行2G再次零丢包；下行4G第二次为零丢包，因此4G结果范围是
`0..0.7019%`，不能声明稳定零丢包。上行2G再次零丢包；上行4G第二次丢包
0.2412%，与首次基本一致；上行10G第二次输出9.4583G、丢包5.4174%，说明
10G压力档波动较大。保守的重复零丢包边界是6 Session下行2G、上行2G。

累计运行计数显示Worker 1..5各处理约601万包且drop/queue-full/push-fail均为0；
包含所有单Session测试的Worker 0约4303万包。N3/N6入口仍只有qid 1工作，
N3`in-flight-max=8192`、N6`in-flight-max=7445`，dispatcher drop、
RX/refill/stale均为0。结果说明：

- 下行GTP-U封装能从6 Session/6 Worker并行获益，零丢包边界从本轮单Session
  的0.5G提高到可重复的2G。
- 上行单Session反而更好，因为单owner按输入顺序完成，不产生跨Worker
  completion重排；6 Session共享同一个入口qid时，有序refill重新形成
  head-of-line和ring反压。
- “配置6个Worker”不等于一个Session会并行使用6个Worker。Session归属保证
  语义一致性，一个Session仍只由一个owner Worker处理。

## 独立VCL/VF发生器隔离（2026-07-30）

为验证正式VPP内置packet-generator是否因占用VPP Worker而降低memif消费速度，
在不修改Open5GS和VPP源码的前提下创建临时VPP 26.06/VCL Pod。临时Pod申请
1个`intel.com/fabric_network` VF、2个CPUManager独占逻辑CPU和4GiB大页，
通过无网线X710 fabric VF内部交换向正式VPP的N6 VF发送流量。正式VPP测试期间
临时删除`dpdk-n6`的NAT44 outside feature，使目标UE IP的测试包直接路由到
N6 memif；测试结束后已恢复NAT并删除临时Pod、ConfigMap和所有PG stream。

固定条件：

- 当前6 Worker运行实例，只向Session `10.45.0.8`发流；
- inner IPv4 UDP 1428 bytes，目标1.2G，持续20秒；
- 内置PG为单stream、`maxframe=2`；
- 独立VCL使用`10.2.0.227..230`四个源IP和四个源端口，每流300M；
- 下行执行完整PDR/FAR/QER/URR和GTP-U封装；
- N3 fabric物理口未插线，N3 memif后的ARP丢包不计入核心段。

10M/2秒VCL冒烟中，发生器提交1,751包，正式N6 VF、N6 memif和N3 memif均为
1,751包，证明独立发生器确实经过正式N6 VF、Open5GS语义处理和N3 memif输出。

### 吞吐与丢包

| 发生器 | 发生包/速率 | 正式N6 VF IPv4输入 | Open5GS N6 RX | Worker处理/N3发送请求 | N3 memif输出 | 核心段丢包 |
|---|---:|---:|---:|---:|---:|---:|
| 内置PG | 2,100,840 / 1.2000G | 不经过VF | 2,100,840 | 2,100,840 | 2,092,823 / 1.1954G | 8,017 / 0.3816% |
| VCL/VF第1次 | 2,100,734 / 1.1999G | 1,992,314 / 1.1380G | 1,968,038 / 1.1241G | 1,912,676 | 1,911,405 / 1.0918G | 80,909 / 4.0611% |
| VCL/VF第2次 | 2,100,580 / 1.1999G | 1,985,694 / 1.1342G | 1,939,958 / 1.1081G | 1,852,538 | 1,850,732 / 1.0571G | 134,962 / 6.7967% |

核心段丢包以正式VPP向N6 memif尝试发送的IPv4包为分母、以N3 memif实际收到
的包为输出；独立发生器到正式N6 VF之前的`rx-miss`另行统计，不混入该百分比。

### 精确丢包位置

| 测试 | N6 VF `rx-miss` | N6 memif无空位 | dispatcher/Worker queue-full | N3 TX allocation | 合计核心段差值 |
|---|---:|---:|---:|---:|---:|
| 内置PG | 不适用 | 0 | 0 | 8,017 | 8,017 |
| VCL/VF第1次 | 108,420 | 24,276 | 55,362 | 1,271 | 80,909 |
| VCL/VF第2次 | 114,887 | 45,736 | 87,420 | 1,806 | 134,962 |

内置PG测试中，N6 RX qid 1接收2,100,840包，dispatcher drop和Worker drop均为
0；N3 TX qid 0的`send-request - tx-sent`恰为8,017，说明该轮差值仍全部发生
在N3 TX buffer allocation。

独立VCL四条流经N6 VF RSS和正式VPP后实际进入N6 memif qid 0/1/5，但Session
归属算法将它们全部投递给Worker 0。第2次测试中：

- 正式VPP尝试送入N6 memif 1,985,694包，Open5GS实际RX 1,939,958包，
  差45,736包，对应VPP `memif1/0-tx no free tx slots`；
- Worker 0队列高水位达到8192，新增87,420个dispatch drop，处理
  1,852,538包；
- N3 TX qid 0收到1,852,538个发送请求，实际发送1,850,732包，allocation
  丢1,806包；
- 三段差值`45,736 + 87,420 + 1,806 = 134,962`，与接口计数完全闭合。

结论：

1. 独立VCL/VF没有恢复1.2G零丢包，且两次均明显差于内置PG，因此“内置PG抢占
   正式VPP Worker”不是当前单Session下行退化的主要原因。
2. 外部VF路径更接近真实输入，也额外暴露X710 VF `rx-miss`和N6 memif
   input-ring反压。多RX qid并不等于单Session能使用多个Session Worker；这些
   qid最终都汇聚到同一个owner Worker。
3. 内置PG条件下主要看到N3 TX allocation；外部多qid突发条件下，主要损失已
   前移到N6 input ring和Worker队列。当前瓶颈是单Session owner的持续消费速度
   及端到端反压，不应只归因于VPP N3 memif消费。
4. 单纯扩大Worker队列可以延迟短测中的queue-full，但本轮Worker实际持续输出
   只有约1.06～1.09G；若平均输入继续大于处理速度，最终仍会填满并增加延迟，
   不能作为持续吞吐优化。

## 直接路径、聚合、反压与N3重试A/B（2026-07-30）

固定VPP 26.06、1428-byte inner IPv4 UDP、1 Session、目标1.2G、20秒，
逐项验证建议的四种改进。所有临时代码均未改变PDR/FAR/QER/URR、PFCP或
GTP-U语义；出现回退的实现已从代码和部署中撤销。

### Session Worker直接路径

临时设置`sessionWorkers.enabled=false`，memif自动使用单队列，N6 dispatcher
直接执行完整UPF处理。内置PG输入2,100,840包，N3输出2,057,259包，核心段
丢43,581包（2.074%）。独立VCL/VF发生2,082,546包，正式VPP N6输入
1,904,662包，N3输出1,890,859包；以正式VPP输入计算核心段丢13,803包
（0.725%）。

与同类独立VCL Session Worker路径4.061%～6.797%的核心段丢包相比，直接路径
减少约3.3～6.1个百分点，证明dispatcher到Worker的排队、唤醒和反压确实构成
回退；但直接路径仍未恢复零丢包，不能把全部问题归因于Session Worker架构。
诊断结束后已恢复6 Worker。

### 自适应聚合和队列反压

曾实现跨RX burst按owner累计64包或8微秒超时投递。1.2G/20秒输入
2,100,840包、输出1,927,050包，丢173,790包（8.273%）。运行计数中约
187.7万次为超时flush，达到64包触发的flush仅212次；在10微秒平滑发包下，
8微秒阈值退化成近乎逐包投递，破坏了原Worker内部合批，因此已回退。

随后单独保留Worker队列满时的有界重投，但该轮`queue-full/retry`均为0，
损失全部发生在N3 TX allocation。该改动对当前限制没有收益，也已回退。

### N3 TX allocation有界重试

仅在`memif_buffer_alloc()`返回0时，最多重试3次、总等待不超过10微秒。
1.2G/20秒完整输入2,100,840包，输出1,950,301包，丢150,539包
（7.165%）。共执行858,895次重试、累计忙等约2.577秒，只挽回25次申请；
等待期间worker不能继续完成已有任务，反而加剧队列和ring反压。该实现已立即
回退，部署恢复到`six-worker-balance`基线。

恢复后的两轮基线分别为：

| 轮次 | 实际输入 | N3输出 | 核心段丢包 | 实际输出 |
|---|---:|---:|---:|---:|
| 基线1 | 2,058,348（发生器未送满） | 2,054,480 | 3,868 / 0.188% | 约1.174G |
| 基线2 | 2,100,840 | 2,096,531 | 4,309 / 0.205% | 约1.198G |
| perf轮 | 2,100,840 | 2,100,070 | 770 / 0.0367% | 约1.200G |

### 当前代码perf

对perf轮同时以99Hz采样N6 dispatcher、实际活跃Session worker和负责该流的
VPP worker，三个线程共3,749个cycles样本且无丢失。各线程按自身样本归一化：

- Session worker：`session_worker_main`28.20%，未解析的vDSO时间读取20.56%，
  `pthread_mutex_unlock`13.91%，`__vdso_clock_gettime`7.82%，`queue_pop`
  6.03%，`ogs_get_monotonic_time`4.69%。
- N6 dispatcher：`pthread_mutex_lock`28.80%，
  `drain_queue_completions`3.85%，`do_epoll_wait`2.60%，
  `memif_refill_queue`1.95%，`memif_rx_burst`1.58%。
- VPP worker：主要为DPDK input 9.00%、memif input 6.31%以及VLIB节点调度；
  内置PG约2.03%，未出现单一异常热点。

当时证据提示应验证数据面高频时间读取和任务队列锁竞争，并在不破坏原有合批
的前提下优化队列协议。后续A/B结果见下一节。不能继续在TX ring无空位时让
唯一Session worker原地微秒级忙等；这种局部重试会降低而不是提高持续吞吐。

## Worker计时、queue bulk-pop与completion复核（2026-07-30）

固定VPP 26.06、6 Worker、6个已建立PFCP Session，只向`10.45.0.2`发送
1428-byte inner IPv4 UDP；下行目标1.2G、20秒、10微秒节奏。每项单独构建、
部署和验证；未达到验收标准的代码均已回退，未改变PDR/FAR/QER/URR、PFCP或
GTP-U语义。

### worker高频计时

临时将20微秒busy-poll期间的截止时间检查从每次空轮询一次改为每64次一次。
100M/2秒功能冒烟输入/输出均为17,506包。优化实例首次建立Session后的三轮
1.2G测试均输入/输出2,100,840包、Open5GS段零丢包。该实例perf中，活跃worker
的未解析时间读取符号降至2.13%；主要样本变为`session_worker_main`49.72%、
`queue_pop`15.63%、`memif_tx_burst`10.67%，说明计时热点确实下降。

但重启UE后同一IP重新归属Worker 3/TX qid 3，连续三轮结果为：

| 轮次 | 实际输入 | N3输出 | N3 TX allocation丢包 |
|---|---:|---:|---:|
| 优化实例1 | 2,100,499 | 2,019,751 | 80,748 / 3.844% |
| 优化实例2 | 2,074,675 | 2,048,370 | 26,305 / 1.268% |
| 优化实例3 | 2,047,334 | 2,004,968 | 42,366 / 2.069% |

三轮dispatcher drop、Worker drop、queue-full、push-fail均为0，差值均发生在
N3 TX buffer allocation。恢复`six-worker-balance`基线并再次重建UE后，
`10.45.0.2`改归属Worker 0/TX qid 0：

| 轮次 | 实际输入 | N3输出 | N3 TX allocation丢包 |
|---|---:|---:|---:|
| 恢复基线1 | 2,067,709 | 2,021,458 | 46,251 / 2.237% |
| 恢复基线2 | 2,100,840 | 2,052,968 | 47,872 / 2.279% |

恢复基线后的100M/2秒冒烟仍为17,506包输入/输出、零丢包。由于优化和基线落在
不同owner/TX qid，跨重启数据不能构成严格代码A/B；它同时证明当前环境的qid
映射和VPP消费节奏会产生百分点级波动。计时修改虽降低CPU热点，却没有证明
同owner/qid下可重复的稳定吞吐收益，因此按保守验收原则回退。

### 现有队列bulk-pop

临时增加一次加锁取出队列中全部已有batch的接口，dispatcher仍按原RX burst
立即投递，不跨burst等待。core queue单元测试和100M功能冒烟均通过，但首次
1.2G测试实际输入1,916,230包、N3输出1,421,383包，丢494,847包
（25.824%）。dispatcher和Worker内部drop仍为0，损失全部发生在N3 TX
allocation。

bulk-pop把多个已有RX batch连续交给worker，形成更集中的N3 TX buffer申请，
加剧生产/消费节奏不匹配。该改动已立即从`dataplane.c`、`ogs-queue.c/.h`和
queue单元测试中完整撤销。

### completion SPSC可行性复核

当前descriptor lease的completion路径本来不使用`ogs_queue`、mutex或
condition variable：worker验证generation/sequence后以原子store标记lease
完成；N3/N6 dispatcher按原RX sequence扫描连续完成项，并按connection/qid
批量调用`memif_refill_queue()`。

因此新增“每worker独占completion SPSC”不能消除perf中观察到的任务队列mutex，
还会引入多worker completion的有序合并、断线generation和停止排空复杂度。
该项没有可量化收益且增加descriptor生命周期风险，故未实施。

本轮最终状态：运行实例和源代码均恢复`six-worker-balance`基线；没有保留
任何上述实验数据面改动。下一轮必须先实现不重启Session、不改变owner/qid的
严格A/B，并针对每TX qid分别记录VPP消费间隔和allocation可用性。

## 同Pod/Session/qid热切换与调度诊断（2026-07-30）

增加N3 TX逐qid诊断计数和`run_dl_qid_diag.sh`。数据面处理、Session owner、
PDR/FAR/QER/URR、PFCP和GTP-U语义均未改变。TX burst间隔采样默认关闭，仅在
容器内存在`/tmp/open5gs-n3-tx-timing`时热启用；开关不重启UPF或Session。

固定Pod `xcn-5gc-6b6f8c8f78-nthfw`、6个已建立PFCP Session，只向
`10.45.0.2`发送。该Session在整个A/B期间始终归属Worker 2/N3 TX qid 2。
1428-byte inner IPv4 UDP、1.2G、20秒、10微秒节奏的结果：

| 状态 | 轮次 | N6输入 | N3输出 | N3 TX allocation丢包 |
|---|---:|---:|---:|---:|
| timing off（前） | 1 | 2,100,840 | 1,989,899 | 110,941 / 5.2808% |
| timing off（前） | 2 | 2,100,840 | 2,060,931 | 39,909 / 1.8997% |
| timing off（前） | 3 | 2,100,840 | 1,765,347 | 335,493 / 15.9695% |
| timing on | 1 | 2,100,840 | 1,740,752 | 360,088 / 17.1402% |
| timing on | 2 | 2,100,840 | 1,758,293 | 342,547 / 16.3052% |
| timing on | 3 | 2,100,840 | 1,770,488 | 330,352 / 15.7248% |
| timing off（后） | 1 | 2,100,840 | 1,746,832 | 354,008 / 16.8517% |
| timing off（后） | 2 | 2,100,840 | 1,755,124 | 345,716 / 16.4561% |
| timing off（后） | 3 | 2,100,840 | 1,716,389 | 384,451 / 18.3008% |

切回off后性能没有恢复，因此不能把前期某轮零丢包或本轮变化归因于每burst
读取时钟。该热开关只用于测量观测开销，不是性能优化。

完整诊断轮输入2,100,840包、N3输出1,706,614包，差394,226包
（18.7652%），差值全部对应qid 2的`memif_buffer_alloc()`短申请/零申请。
dispatcher drop、Worker内部drop、RX/refill error均未构成主要差值。N3实际
发送burst的累计平均大小为1包，最大64包，否定了“大TX burst集中填满ring”
这一首要假设。

同一诊断窗口的线程`schedstat`增量：

| 线程 | 作用 | CPU运行时间 | runqueue等待 | 非自愿切换 |
|---|---|---:|---:|---:|
| Open5GS tid 20 | Session Worker 2 | 13.302s | 68.8ms | 22 |
| Open5GS tid 25 | N6 dispatcher | 10.676s | 146.8ms | 169 |
| VPP tid 12 | 消费N3 memif qid 2 | 24.448s | 10.021s | 690 |

Open5GS worker的调度等待不足其运行时间的1%，不支持“Open5GS线程工作中频繁
被切走导致丢包”。主要调度缺口在VPP qid消费线程。

当前VPP cpuset为`49-52,121-124`，实际对应4个物理核的超线程对：
`49/121`、`50/122`、`51/123`、`52/124`。VPP启动了1个main和7个持续轮询
worker，占满全部8个逻辑CPU。`show interface rx-placement`确认N3 memif
qid 2由`vpp_wk_3`消费，其初始lcore为121，与main的lcore49互为超线程兄弟。
CPUManager随后又把各VPP线程的Linux affinity扩展到整个cpuset，Linux可在这
8个逻辑CPU间迁移线程。不同测试时段中main、packet-generator、DPDK polling
和memif polling获得的执行份额会变化，因此即使Session owner/qid不变，也会
产生百分点级吞吐波动。

500ms VPP ring采样和UPF约11秒统计快照均只看到qid 2为空，但
`memif_buffer_alloc()`在测试中大量返回0，说明ring枯竭发生在远短于500ms的
瞬态窗口；低频ring快照不能作为“从未满过”的证据。代码中的零申请episode
按相邻失败调用统计，间隔超过1ms即划分新episode，避免把测试结束后的空闲期
误算为连续失败。

本轮结论：

1. Open5GS不是大burst生产者，且其worker调度延迟很小；首要证据指向VPP
   memif消费线程的运行份额/轮询间隔波动。
2. 这不表示libmemif单队列本身只有约1G能力；裸memif线速实验没有同时运行
   当前main + 7 busy-poll worker、DPDK双VF、PG和完整UPF链路。
3. 下一轮应先做CPU资源隔离A/B：保持CPUManager机制不变，给VPP更多成对逻辑
   CPU，但只启用所需数量的VPP worker，为每个busy-poll worker保留不运行
   另一个VPP busy-poll线程的兄弟逻辑CPU；不得仅把ring继续放大或在Open5GS
   worker中忙等重试。

最终`qid-diag-v3`运行验证：6个PFCP Session全部重建成功；100M/2秒冒烟
17,506包输入/输出、核心段零丢包。随后1.2G/20秒发生器实际送出2,076,458包，
N3输出1,933,495包，N3 TX allocation丢142,963包（6.8849%）。该Session本次
归属qid 5，共识别19个零申请episode，最大连续失败56,746次、真实最大持续
180,640微秒；测试结束等待15秒后`zero-current-us`仍为0，确认空闲期不再被
误计入失败持续时间。

## VPP独立物理核绑核A/B（2026-07-30）

保持CPUManager机制和超线程配置不变，将VPP Guaranteed CPU从8个逻辑CPU
提高到16个，固定启动6个VPP Worker。运行cpuset为
`46-53,118-125`，对应8个完整物理核超线程对。入口supervisor在CPUManager
完成分配后，将VPP main和6个Worker逐线程重新绑定；所有线程保持
`SCHED_OTHER`。

同一Pod、同一VPP PID、同一组6个PFCP Session，只向`10.45.0.2`发送。
该Session在整个A/B期间保持N3 TX qid 1。1428-byte inner IPv4 UDP、
10微秒节奏，每种模式连续3轮：

| 目标 | 阶段 | 布局 | 总输入包 | 总丢包 | 平均丢包率 |
|---:|---|---|---:|---:|---:|
| 1.2 Gbps | A1 | dense | 6,302,520 | 4,311 | 0.0684% |
| 1.2 Gbps | B | isolated | 6,302,520 | 2,850 | 0.0452% |
| 1.2 Gbps | A2 | dense | 6,302,520 | 6,213 | 0.0986% |
| 2.0 Gbps | A1 | dense | 10,504,200 | 71,541 | 0.6811% |
| 2.0 Gbps | B | isolated | 10,504,200 | 105,549 | 1.0049% |
| 2.0 Gbps | A2 | dense | 10,504,200 | 28,178 | 0.2683% |

1.2G九轮均显著好于此前8逻辑CPU下1.8997%至18.3008%的同类结果，说明
“增加成对CPU资源、限制VPP busy-poll Worker数、重新逐线程绑核”具有明确
运行价值。但这组资源变更需要重建Pod，和旧配置之间不是严格的同进程热A/B，
不能把全部改善只归因于某一个因素。

物理核隔离本身没有呈现单调收益：2G的`dense -> isolated -> dense`丢包率
没有随模式可逆变化。补充2G单轮`schedstat`测试结果同样波动：

| 阶段 | 布局 | 丢包率 |
|---|---|---:|
| A1 | dense | 1.0828% |
| B | isolated | 0.3155% |
| A2 | dense | 0.4454% |

因此默认保留`isolated`以避免VPP polling线程确定性地竞争同一物理核，但不把
它描述为单独的性能保证。packet-generator、DPDK polling、memif polling、
主机IRQ/内核任务的相位仍会使结果波动。`show interface rx-placement`显示
packet-generator使用VPP Worker 0，N3 memif qid 1由Worker 3消费；不同阶段
的runqueue等待没有与丢包率形成稳定单调关系。

最终Helm revision 92实际获得cpuset
`50-57,122-129`，运行状态为main CPU50、六个Worker CPU51至56，
全部为单CPU affinity和`SCHED_OTHER`。重建后六个PFCP Session全部建立。
100M/2秒冒烟输入/输出17,506包。最终isolated单轮1.2G/20秒输入
2,100,840包、N3输出2,094,775包，丢6,065包（0.2887%）；差值严格对应
N3 TX qid 3的allocation drop，dispatcher、Worker内部处理及RX/refill均为
零丢包。该单轮高于前述三轮isolated平均值，再次说明不能用一次结果代替多轮
统计。

### 单Session高负载扫描

保持同一Pod、VPP PID 7、六个已建立PFCP Session和`isolated`绑核，只向
`10.45.0.2`发送，按10微秒均匀节奏分别运行20秒。以下均为完整单轮，发生器
实际输入达到目标包数：

| 目标速率 | N6输入 | N3输出 | 丢包 | 丢包率 | 有效吞吐 |
|---:|---:|---:|---:|---:|---:|
| 4 Gbps | 7,002,800 | 6,871,383 | 131,417 | 1.8766% | 3.9249 Gbps |
| 6 Gbps | 10,504,200 | 10,253,853 | 250,347 | 2.3833% | 5.8570 Gbps |
| 8 Gbps | 14,005,600 | 13,854,196 | 151,404 | 1.0810% | 7.9135 Gbps |
| 10 Gbps | 17,507,000 | 16,998,483 | 508,517 | 2.9047% | 9.7095 Gbps |

四档合计差1,041,685包，严格等于N3 TX qid 3的allocation drop从6,065增至
1,047,750的增量。Session Worker drop、queue-full、dispatcher drop以及
RX/refill error均为0。说明单Session路径可以处理接近10G的有效吞吐，但当前
无法宣称4/6/8/10G零丢包稳定运行；丢包率不随目标速率单调增长，仍存在明显
VPP消费/测试发生器相位波动。

随后尝试扩展`dense A1 -> isolated B -> dense A2`时，两个测试编排进程意外
重叠并同时操作VPP内置packet-generator。一进程发包期间，另一进程执行
`delete packet-generator interface pg0`，VPP 26.06在
`ip4_lookup_node_fn_x86_64_v4`触发SIGSEGV，容器exit 250并自动重启。该组
交错日志无效，未计入性能结果。这是测试控制面并发删除PG接口导致，不是正常
UPF业务路径在4G压力下崩溃。

为防止再次发生，`run_pg_dl_multi.sh`增加每Pod packet-generator排他锁，
`run_vpp_affinity_ab.sh`增加覆盖整个A/B生命周期的每Pod排他锁。锁冲突时
脚本直接失败，不再并发修改PG stream、接口或affinity模式。

### 六Session/六Worker高负载扫描

保持VPP `isolated + SCHED_OTHER`、六个已建立PFCP Session和六个UPF
Session Worker，向`10.45.0.2`至`10.45.0.7`同时发送。每档运行20秒，
1428-byte inner IPv4 UDP、10微秒均匀节奏；总pps按整数尽量平均分配到六个
Session。例如总计6G时每Session为1G。

| 总目标速率 | 每Session目标 | N6输入 | N3输出 | 丢包 | 丢包率 | 有效吞吐 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 Gbps | 0.1667 Gbps | 1,750,700 | 1,750,700 | 0 | 0% | 1.0000 Gbps |
| 2 Gbps | 0.3333 Gbps | 3,501,400 | 3,501,400 | 0 | 0% | 2.0000 Gbps |
| 4 Gbps | 0.6667 Gbps | 7,002,800 | 7,002,800 | 0 | 0% | 4.0000 Gbps |
| 6 Gbps | 1.0000 Gbps | 10,504,200 | 10,504,041 | 159 | 0.001514% | 5.9999 Gbps |
| 8 Gbps | 1.3333 Gbps | 14,005,600 | 13,990,607 | 14,993 | 0.107050% | 7.9914 Gbps |
| 10 Gbps | 1.6667 Gbps | 17,507,000 | 17,464,161 | 42,839 | 0.244696% | 9.9755 Gbps |

六档发生器均达到目标输入包数。UPF累计计数确认六个Session分别由Worker
0至5处理，并分别使用N3 TX qid 0至5；六个Worker内部drop、queue-full和
push-fail均为0。六档合计57,991包差值均落在各N3 TX qid的
`memif_buffer_alloc()`短申请/零申请，dispatcher drop和RX/refill error为0。

与同条件单Session结果相比，多Session/多Worker分流收益明显：

| 总目标速率 | 单Session丢包率 | 六Session丢包率 |
|---:|---:|---:|
| 4 Gbps | 1.8766% | 0% |
| 6 Gbps | 2.3833% | 0.001514% |
| 8 Gbps | 1.0810% | 0.107050% |
| 10 Gbps | 2.9047% | 0.244696% |

这组单轮结果证明Session归属和六Worker并行实际生效，且在当前内部PG/memif
验证路径上4G可零丢包、10G有效吞吐达到9.9755G。但8G/10G仍非零丢包，
并且当前N6入口流量仍全部由memif RX qid 1接收后再按UE IP分发到六个
Session Worker；因此不能把本结果表述为N6真实多RX qid线速，也不能用单轮
结果替代长期稳定性测试。

### VPP 0/1 Worker CPU布局对比（2026-08-01）

UPF固定8个逻辑CPU、六个Session Worker、六个PFCP Session和六组N3/N6
memif ring。流量均匀分配到`10.45.0.2`至`10.45.0.7`，1428-byte inner
IPv4 UDP、10微秒节奏、每档20秒。仅改变VPP CPU和Worker布局：

| 场景 | VPP cpuset示例 | VPP运行线程 | 绑核方式 |
|---|---|---|---|
| 1 | `50,122` | main CPU50，0 Worker | 分配一个物理核的两个超线程，仅使用一个逻辑CPU |
| 2 | `49,121` | main CPU49、Worker CPU121 | main/Worker共享同一物理核的两个超线程 |
| 3 | `45,46,117,118` | main CPU45、Worker CPU46 | main/Worker分别使用两个物理核 |

三种场景的完整单轮结果相同：

| 总目标速率 | 每Session目标 | N6输入 | N3输出 | 丢包 | 丢包率 | 有效吞吐 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 Gbps | 0.1667 Gbps | 1,750,700 | 1,750,700 | 0 | 0% | 1.0000 Gbps |
| 2 Gbps | 0.3333 Gbps | 3,501,400 | 3,501,400 | 0 | 0% | 2.0000 Gbps |
| 4 Gbps | 0.6667 Gbps | 7,002,800 | 7,002,800 | 0 | 0% | 4.0000 Gbps |
| 6 Gbps | 1.0000 Gbps | 10,504,200 | 10,504,200 | 0 | 0% | 6.0000 Gbps |
| 8 Gbps | 1.3333 Gbps | 14,005,600 | 14,005,600 | 0 | 0% | 8.0000 Gbps |
| 10 Gbps | 1.6667 Gbps | 17,507,000 | 17,507,000 | 0 | 0% | 10.0000 Gbps |

场景3累计计数确认六个UPF Worker各处理约9,045,000包，N3 TX qid 0至5的
`alloc-short`、`alloc-fail`和`drop`全部为0。N6入口仍全部来自qid 1，随后
按UE IP分发到六个Session Worker。唯一VPP Worker轮询N3/N6全部12个memif
队列以及两块VF的8个DPDK RX队列。

该结果不能据此认定生产环境只需要0或1个VPP Worker，原因是测试流量由VPP
内置packet-generator产生，核心计数口径止于回到VPP的N3 memif；fabric VF
未接网线，真实N3/N6 DPDK收发、外部IRQ/RX burst、NAT44和网卡线速均未进入
被测路径。0/1 Worker下`set nat workers 2`不满足VPP要求，NAT44插件未启用，
因此这两种配置不具备当前推荐配置的完整N6功能。

本轮说明的是：此前六VPP Worker配置出现的N3 TX allocation波动并非完整UPF
语义处理能力不足；在内部PG测试路径中，一个VPP main或一个VPP Worker就能
及时消费六个N3 TX ring。多VPP Worker的队列分配、轮询相位及PG竞争反而会
影响该合成测试结果。基于该结果，VPP默认值调整为2逻辑CPU、1 VPP Worker、
dense，同一物理核的两个超线程分别运行main和Worker；真实外部VF全链路容量
仍需在有线环境继续验证。

### VPP默认两逻辑CPU运行验证（2026-08-01）

Helm revision 98实际部署默认策略：`vpp.resources.cpu=2`、
`vpp.cpu.workers=auto`、`initialMode=dense`。CPUManager分配`45,117`，入口脚本
识别为同一Socket、同一物理Core 9的两个超线程；`vpp_main`绑定CPU45，唯一
`vpp_wk_0`绑定CPU117，调度策略均为`SCHED_OTHER`。

VPP 26.06只有一个Worker时拒绝`set nat workers`命令，因此单Worker默认不再
生成该命令，交由NAT44使用唯一Worker。实际检查NAT44已正确配置
`memif1/0 in`和`dpdk-n6 out`，N3/N6 memif与两个DPDK VF均为up，Pod 9/9
Running且0重启。Open5GS保持8逻辑CPU、6个Session Worker和两个dispatcher，
六个PFCP Session（10.45.0.2至10.45.0.7）全部重建成功。

六Session均匀下行100Mbps、10微秒节奏、2秒冒烟：VPP PG输入17,506包，N6
memif发送17,506包，N3 memif接收17,506包，核心路径零丢包。fabric VF未插
网线导致N3侧ARP未解析，不计为UPF/memif丢包。

### VPP默认两逻辑CPU的单Session下行性能（2026-08-01）

保持Helm revision 98、VPP两逻辑CPU（1 main + 1 Worker）、Open5GS 8逻辑
CPU（6 Session Worker + N3/N6 dispatcher）和六个已建立PFCP Session不变，
仅向`10.45.0.2`一个Session发送流量。报文为1428字节IPv4 UDP，10微秒节奏，
每档20秒：

| 目标速率 | PG/N6输入包 | N3 memif输出包 | 核心路径丢包率 | 有效吞吐 |
|---:|---:|---:|---:|---:|
| 1 Gbps | 1,750,700 | 1,750,700 | 0% | 1.000 Gbps |
| 2 Gbps | 3,501,400 | 3,501,400 | 0% | 2.000 Gbps |
| 4 Gbps | 7,002,800 | 7,002,800 | 0% | 4.000 Gbps |
| 6 Gbps | 10,504,200 | 10,504,200 | 0% | 6.000 Gbps |
| 8 Gbps | 14,005,600 | 14,005,600 | 0% | 8.000 Gbps |
| 10 Gbps | 17,507,000 | 17,507,000 | 0% | 10.000 Gbps |

测试后该Session所属Worker 5累计`drops=0`、`queue-full=0`、`push-fail=0`，
N3 TX qid 5累计`alloc-short=0`、`alloc-fail=0`、`drop=0`。Pod保持9/9
Running、0重启。以上结论止于VPP内置PG→N6 memif→Open5GS完整UPF语义→
N3 memif→VPP；fabric VF未插网线，N3侧ARP未解析不计为核心路径丢包，也不
代表真实外部VF收发已经验证到10Gbps。

## UPF实时速率统计功能与性能A/B（2026-08-01）

新增本地Unix socket CLI，按SUPI、PFCP Session、QFI承载和PDR/QER规则展示
上下行实时速率、pps及累计包/字节。统计只在N3/N6实际发送成功后记账；数据
线程只更新Worker本地计数，并在现有TX batch结束时发布，不在逐包路径读取
时钟、分配内存、打印日志、获取mutex或执行跨核原子累加。采样和CLI格式化由
独立低频线程完成，默认采样周期1秒。

实际部署使用VPP 26.06、VPP 2逻辑CPU（1 main + 1 Worker）、UPF 8逻辑CPU
（6 Session Worker + N3/N6 dispatcher）、6个PFCP Session。CLI正确识别同一
`imsi-460110000000100`下`10.45.0.2..7`六个Session，并显示各自UPF N4 SEID、
owner Worker、QFI和PDR/QER方向。六Session均衡注入1.2Gbps时，用户汇总实时
值为1.199991Gbps，每Session约200Mbps；对应累计字节和发生器实际包数一致。
`--supi`、`--ue-ip`、`--seid`过滤、JSON输出以及user/session/bearer/rule四级
输出均通过运行验证。配置为disabled时不创建socket，数据面计数路径完全跳过。

性能A/B使用1428-byte inner IPv4 UDP、10微秒节奏、六Session均衡流量、每轮
20秒，计数口径为VPP N6 memif实际输入到N3 memif实际输出：

| 统计模式 | 目标/实际输入 | 输入包 | 实际输出 | 输出包 | Open5GS段丢包率 |
|---|---:|---:|---:|---:|---:|
| enabled | 10.0000Gbps | 17,507,000 | 10.0000Gbps | 17,507,000 | 0% |
| disabled | 10.0000Gbps | 17,507,000 | 10.0000Gbps | 17,507,000 | 0% |

开启统计的两轮输入累计为27,999,995,520字节，CLI用户级累计值严格相等。
本轮未观察到统计功能导致的吞吐或丢包回退。该结论覆盖VPP内置PG→N6 memif
→Open5GS完整UPF语义→N3 memif→VPP；fabric VF未插网线，因此不代表外部VF
全链路10Gbps已验证。该轮镜像部署于Helm revision 102，保持
`rateStats.enabled=true`。

## UPF速率CLI动态表格排版验证（2026-08-01）

文本输出改为由`open5gs-upfctl`接收完整快照后动态计算列宽。SUPI、UE IP、
DNN和方向左对齐，SEID、Worker、QFI/PDR/QER、Mbps、pps和累计计数右对齐，
表头与数据之间增加分隔线。列宽取当前表头和全部数据的最大宽度，因此累计
数值增长、速率位数变化、IPv6地址或较长标识不会破坏对齐。JSON输出保持不变。

实际Pod对user/session/bearer/rule四级零流量表格均验证通过。六Session均衡
1.2Gbps下行、1428-byte inner IPv4 UDP、10微秒节奏、10秒测试中，实时用户
汇总显示约1.199Gbps，每Session约200Mbps；VPP N6 memif输入1,050,420包，
N3 memif输出1,050,420包，Open5GS段丢包为0。非零Mbps、pps和十位累计字节
同时出现时各列仍保持对齐。

普通未分配TTY的`kubectl exec ... --watch --interval 0.5`运行3秒，确认每轮均
输出ANSI光标归位/清屏序列和一张完整表格，真实终端不再逐秒向下堆叠。脚本
和重定向场景应使用`--json`。

最终镜像`xcn-runtime:rate-stats-table-v2`部署于Helm revision 104；文本
`--watch`启用单屏刷新，`--watch --json`不输出ANSI控制序列。
