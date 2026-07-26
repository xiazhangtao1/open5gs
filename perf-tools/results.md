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

## 每 Worker SPSC batch ring 与混合轮询（2026-07-26）

实现内容：

- Dispatcher 到每个 Worker 的 `ogs_queue + mutex + pthread_cond` 替换为固定
  容量单生产者/单消费者 batch ring。
- batch 描述符直接存放在 ring；payload 仍使用既有固定 task pool，Worker
  完成处理后才发布 `read_seq`，避免覆盖尚未消费的 payload。
- producer/consumer cursor 分离到不同 cache line，发布和消费分别使用
  release/acquire；Session owner 保证每个 ring 只有一个 producer 和 consumer。
- Worker 处理后在配置的 `busy_poll_us` 内使用 CPU relax 轮询，超时后通过
  nonblocking eventfd + poll 睡眠；睡眠标志发布后重新检查 ring，避免 lost wake。
- 停止时先设置 stopping，再唤醒 dispatcher 和全部 Worker，join 后关闭 eventfd
  和释放 ring。全部线程保持 `SCHED_OTHER`，CPUManager 机制未改变。

测试条件：

- 镜像：`xcn-runtime:spsc-busy-poll-dev`
- 两个真实 PDU/PFCP Session：`10.45.0.2`、`10.45.0.3`
- 两个 Session Worker、两个 N3/N6 qid、8192-entry ring
- 1428-byte inner IPv4、10us pacing、每档 10 秒、目标 1.2G
- 上行 TEID 由 host tcpdump 抓取真实 UE→UPF GTP-U 包获得；下行输出包中的
  gNB TEID 不能反向用于上行注入，否则只会测到 GTP-U Error Indication。

| busy-poll | 方向 | 发生器目标包 | 实际 Open5GS 输入 | 输出 | 段丢包 | payload 输出 |
|---:|---|---:|---:|---:|---:|---:|
| 0us | 下行 | 1,050,420 | 899,483 | 899,483 | 0 | 1.028G |
| 0us | 上行 | 1,050,420 | 894,571 | 894,571 | 0 | 1.022G |
| 20us | 下行 | 1,050,420 | 1,050,420 | 1,037,168 | 1.262% | 1.185G |
| 20us | 上行 | 1,050,420 | 1,050,420 | 1,033,922 | 1.571% | 1.181G |
| 50us | 下行 | 1,050,420 | 1,006,854 | 1,000,943 | 0.587% | 1.143G |
| 50us | 上行 | 1,050,420 | 1,050,420 | 947,177 | 9.829% | 1.082G |

所有档位的 Worker `ring-full`、Worker drop、dispatcher drop 均为零。20us
实际输出最高，最终 Helm 配置恢复为20us。0us 的 Open5GS 段虽零丢包，但发生器
只实际送入约1.02G，不能记为1.2G零丢包。50us 上行回退说明过长 busy-poll
可能令 Worker 更快消费小 batch，减少原睡眠路径形成的隐式聚合，继而恶化
TX buffer allocation/flush 节奏。

结论：SPSC ring 功能和并发模型验证通过，移除了通用队列锁及条件变量高频唤醒，
但本轮没有证明稳定吞吐超过改造前基线。下一步应优先做有严格包数/时间上限的
自适应微批聚合，并继续以 TX alloc-granted、batch size 和 pending time 定位，
不能仅延长 busy-poll 窗口。

最终加入 lost-wakeup 顺序一致性 fence、强制重建并部署
`sha256:a90f2c0c26e9c0016b5283529efa2461cb30a15f0f979f438407c09ebdddee52`
后，100M/2秒下行 smoke 输入/输出均为17,506包，两个 Worker 各处理
8,754/8,752包，`ring-full/drop=0`，证明睡眠 Worker 可被稳定唤醒。
