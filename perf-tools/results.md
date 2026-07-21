# UPF performance results

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
