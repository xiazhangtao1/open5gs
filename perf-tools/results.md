# UPF performance results

## VPP 26.06 + N3/N6 双 memif + 双 X710 VF（2026-07-18）

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
