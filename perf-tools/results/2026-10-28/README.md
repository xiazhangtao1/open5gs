# 默认配置：单用户、单 PDU Session 的 iperf 实测

上下行分别测试，数值为接收端应用有效载荷速率，单位为十进制 Gbit/s。
一个 PDU Session 内使用 4 个 iperf 进程、每进程 4 条传输流，共 16 条流。
该接入方式在核心侧额外承担两条测试 memif 的拷贝成本；后续排除这部分
测量开销的结果和瓶颈分析见 [10 月 29 日记录](../2026-10-29/README.md)。

| 测量 | 上行 | 下行 | 条件 |
| --- | ---: | ---: | --- |
| TCP：三轮中的最大值 | 16.054 | 16.583 | 每轮 30 秒，预热 3 秒 |
| TCP：三轮平均 | 15.944 | 16.487 | 三轮方向交替 |
| TCP：单条连接参考 | 5.191 | 5.410 | 每方向 30 秒 |
| UDP：已验证的低丢包档位 | 17.500 | 18.500 | 每方向 30 秒，丢包阈值 ≤0.1% |
| UDP：上述档位实际丢包 | 0.001056% | 0.001970% | UL 495/46,877,223；DL 详见原始 JSON |
| UDP：过载接收值 | 18.289 | 19.194 | 实际发送 18.500 / 20.000 Gbit/s |
| UDP：过载丢包 | 1.140056% | 4.041542% | 每方向 30 秒 |

UDP 载荷为 1400 字节；IPv4 内层包 1428 字节，带 QFI 的 N3 GTP-U 包为
1472 字节。低丢包档位使用 0.5 Gbit/s 步长细化：上行发送 18 Gbit/s 的
30 秒测试丢包 0.173884%，下行发送 19 Gbit/s 丢包 0.616032%，均未满足
本文自行采用的 ≤0.1% 阈值。这里的档位是本次采样条件下的实测值，未证明
任意持续时间下的绝对最大值或零丢包能力。过载结果不能当作无损吞吐。

## 测量边界

- 部署基线为 `c2b90f2e316959ad48107885a85b87bc6aa53ffc`，Helm revision 212。
  UPF 4 个逻辑 CPU `44-45,116-117`，VPP 2 个逻辑 CPU `46,118`；分别对应
  2 个和 1 个物理核。保留默认 CFS 配额、单队列 memif、8192 ring、2048 buffer、
  rate stats、monitor/trace；默认 NAT44 关闭。镜像、节点、地址和监控绑定核的
  环境覆盖记录在 [environment.json](environment.json)。
- OAI 建立真实 PFCP/PDU 会话：UE `10.45.0.3`、PSI 1、SEID 1272、QFI 1、5QI 9。
  数据包使用实际上下行 TEID。每轮前后均检查 SMF PDU 和 UPF 会话数量为 1，
  并检查 IP、SEID、TEID 不变。
- 数据路径为：`iperf UE ↔ 测试 VPP GTP-U ↔ 核心 VPP N3 ↔ UPF ↔ 核心 VPP N6 ↔ 测试 VPP ↔ iperf DN`。
  测试端通过额外 memif 接入；UPF 原有 N3/N6 memif、PFCP 规则和 CPU 数量未变。
  OAI UE/gNB 只用于建会话，测试数据绕过无线协议栈和实体网卡。
- 测试 VPP 使用 CPU 60–63，UE iperf 使用 64–67，DN 使用 68–71，均与核心网、
  OAI 使用的物理核分开。地址 `198.19.0.2` 为同机测试 DN。
- X710 的有线口 `ens5f0` 当时实际协商为 1000 Mb/s，`ens5f1` 无链路。测试期间
  memif 有数千万数据包，物理 VF 只有少量背景/控制包，超过 10 Gbit/s 的值来自
  服务器内部共享内存路径，并非网卡线速。TCP 内层 IP 包约 1500 字节、N3 外层约
  1544 字节；TCP 与 UDP 的每包有效载荷不同，不应直接按同一 PPS 比较。
- 默认订阅的信令 AMBR 仍为上下行 1 Gbit/s，未修改。本实现实际转发超过该值；
  本报告测量处理能力，不证明 AMBR 限速行为正确。

## 测试工具与旁路

TCP 两端使用官方 iperf3 3.22。UDP 使用官方 3.22 发送端和单线程 3.15 接收端，
避免 3.22 多线程与当前 VCL 共享工作上下文的开销。下行由 DN 主动连接 UE
接收端，不使用 `-R`；检查两端 IP 以及 UPF 下行计数确认方向。UDP 客户端不显式
`-B`，由对应 VCL namespace 选择地址，并检查实际连接地址。

UDP 汇总使用接收端的字节、时间、序列号丢包计数；已核对客户端交换得到的
`sum_received` 与独立接收端 JSON 的 bytes/seconds/bits_per_second 一致。
总丢包率按所有流的丢包数除以总包数计算，不平均各流百分比。

| 同机工具链旁路对照 | 上行 Gbit/s | 下行 Gbit/s | 说明 |
| --- | ---: | ---: | --- |
| TCP，绕过 UPF | 23.680 | 25.539 | 4×4 流，15 秒 |
| UDP，绕过 UPF | 22.206 | 22.889 | 4×4 流，6 秒；丢包 0.329% / 0.528% |

旁路保留两端及测试 memif，在核心 VPP 临时执行相同 TEID 的 GTP-U 封解装。
它用于验证工具链余量，其不同处理路径不能作为 UPF 开销的精确差分。
此前原生 Linux TAP 和 VCL 单流接收的受限结果没有作为核心网最大值。

iperf3 3.22 源包 SHA256：`1c0d0fb02c52626111d6e132db80edfbf27bbaff8bd9245df2a371dcb0b35a92`。
3.15 源包 SHA256：`bdb77c11f72bce90214883159577fa24412013e62b2083cf5f54391d79b1d8ff`。
来源分别为 [官方 3.22 release](https://github.com/esnet/iperf/releases/tag/3.22) 和
[官方 3.15 源包](https://downloads.es.net/pub/iperf/iperf-3.15.tar.gz)。
两者使用 `--disable-shared --disable-profiling --without-openssl --without-sctp` 构建。

## 原始证据

- [summary.json](summary.json)：32 轮 TCP/UDP 汇总，包括扫描、旁路和最终 30 秒结果。
- [tcp-raw.tar.gz](tcp-raw.tar.gz)、[udp-raw.tar.gz](udp-raw.tar.gz)：客户端/接收端 JSON、
  每轮前后接口、错误、运行时、线程、配额、PDU、UPF 计数及测试配置。
- 脚本：[iperf_core.py](../../scripts/iperf_core.py)、[iperf_core_udp.py](../../scripts/iperf_core_udp.py)。

2026-10-28 下午现场另一次部署已替换本报告的 Pod 和会话；本报告是当时基线的历史记录。
后续环境复测和瓶颈 A/B 分开记录，不能把不同 Pod、镜像或 CPU 布局的结果混为一轮。
