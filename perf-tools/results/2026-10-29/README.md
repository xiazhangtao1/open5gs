# 单用户单会话吞吐复测与瓶颈分析

本轮重新核对现场环境后，使用真实 PDU/PFCP 会话、同机 iperf 和 GTP-U 封解装
测试核心网。核心配置保持 UPF 4 个逻辑 CPU、VPP 2 个逻辑 CPU；数据绕过 OAI
无线协议栈和物理网卡。没有修改核心转发源码。

## 默认配置的最终复测

以下均为接收端应用有效载荷速率，十进制 Gbit/s。每方向分别测量 30 秒，预热
3 秒，4 个 iperf 进程 × 每进程 4 条传输流。它们属于同一个用户、同一个 PDU
Session，并不是 16 个 PDU Session；也不是上下行同时运行。

| 协议及口径 | 上行 Gbit/s | 下行 Gbit/s | UDP 丢包率 |
| --- | ---: | ---: | --- |
| TCP，最终复测 | 17.617 | 16.547 | 不适用；TCP 可靠交付不代表底层零丢包 |
| TCP，本轮零拷贝接入下的最高观测值 | 17.932 | 16.567 | 下行该轮含 perf 采样，最终复测见上一行 |
| UDP，最终通过 ≤0.1% 丢包阈值的档位 | 23.000 | 18.000 | 两方向均为 0 个观测丢包 |

UDP 载荷为 1400 字节，内层 IPv4 包 1428 字节，带 QFI 的 GTP-U 外层为
1472 字节。TCP 内层 IP 包约 1500 字节，N3 外层约 1544 字节。速率不包含这些
协议头；TCP 和 UDP 不能按同一有效载荷大小或 PPS 直接等同。

本文自行采用 ≤0.1% 作为低丢包标准，以 0.5 Gbit/s 步长细化边界：

| 最终扫描点 | 实际发送 Gbit/s | 实际接收 Gbit/s | 丢包率 |
| --- | ---: | ---: | ---: |
| UL 23.0 | 23.000 | 23.000 | 0% |
| UL 23.5 | 23.500 | 23.397 | 0.439726% |
| DL 18.0 | 18.000 | 18.000 | 0% |
| DL 18.5 | 18.500 | 18.480 | 0.109785% |
| DL 19.0 | 19.000 | 18.895 | 0.557760% |

早先同配置的一轮 DL 18.5 Gbit/s 曾零丢包，但最终复测没有再次通过阈值，
因此本表采用较保守的 18 Gbit/s。零丢包仅指本次有限窗口内没有观察到序列号
丢失，不是任意持续时间的保证。多个进程按各自接收区间求和，启动错开约
0.1 秒；不是严格同步的线速认证测试。

## 环境与测量边界

- 工作树基线 `fdaae9fd2`，现场 Helm revision 213。实际 UPF 镜像 digest 为
  `516977838188751f61023af4a4a7e3c4f02aeab32705cb87716fb568b727b71d`，VPP 为
  `c6905634e001db54e9920644265f7d3ebb29b9dd6a7c171ebb0ae2c7e4e4541c`。
  两个转发镜像与前一轮相同；监控镜像及 CPU 分配逻辑已经变化，故重新建立基线。
- Xeon Gold 6554S，双路、72 个物理核、144 个逻辑 CPU。UPF 使用
  `45-46,117-118`，对应两个物理核；VPP 使用 `47,119`，对应一个物理核。
  保留默认 CPU 配额 4/2、单 Session Worker、单队列 memif、8192 ring、2048 buffer、
  监控/信令采集/rate stats。NAT44 默认关闭，未解除 CFS 配额。
- 现场原有 6 个 UE 和背景业务。已停止背景流量、缩容额外 5 个 UE，并重建核心
  Pod、gNB、主 UE，形成干净的单会话环境。正式测试全程固定同一核心 Pod UID、
  UE `10.45.0.3`、PSI 1、PFCP SEID 2520、QFI 1、5QI 9；每轮检查会话数量和 TEID。
- 默认签约 AMBR 仍为上下行 1 Gbit/s，未修改。数据面实际超过该值，本报告不
  将处理能力测试当作 AMBR 限速正确性的证明。
- 有线 `ens5f0` 当前协商 1000 Mb/s，`ens5f1` 无链路。测试数据在共享内存中
  经过 N3/GTP-U 和 UPF 原始规则，未经过网线，故能超过网卡标称 10 Gbit/s。
  物理 VF 计数与数千万 memif 数据包的差异保存在每轮快照。

完整环境、资源、镜像与二进制哈希见 [environment.json](environment.json)。

## 排除测试端影响

路径为：`iperf UE ↔ 测试 VPP GTP-U ↔ 核心 VPP N3 ↔ UPF ↔ 核心 VPP N6 ↔ 测试 VPP ↔ iperf DN`。
OAI 只用于建立真实会话。正式 4 进程测试中，测试 VPP 使用 CPU 60–63，UE
端使用 64–67，DN 使用 68–71，与核心和无线仿真分开物理核。

前一轮额外测试 memif 的核心侧是 master，会在核心 VPP 增加收发拷贝。本轮
将**仅新增的两条测试链路**改为核心侧 slave/zero-copy，把额外接入拷贝移出
被测核心 CPU。UPF 原有 `memif1/0`、`memif2/0`、队列和 PFCP 规则未变。
原接入下 TCP UL 约 15.64 Gbit/s，零拷贝后约 17.62–17.93 Gbit/s；这属于
改进测量接入，不能宣称核心源码优化获得了这部分提升。

| 同一零拷贝接入、绕过 UPF 的对照 | 上行 Gbit/s | 下行 Gbit/s | 窗口 |
| --- | ---: | ---: | --- |
| TCP | 23.033 | 23.560 | 10 秒 + 3 秒预热 |
| UDP | 24.073 | 25.969 | 15 秒 + 3 秒预热 |

UDP 旁路实际发送 24.128 / 25.967 Gbit/s，丢包分别 0.238461% / 0.009855%。
上行工具余量较小，因此还核对了核心内的入口、成功输出和测试侧 FIFO 计数。
UL 23.5 Gbit/s 一轮中，含预热的核心测试 N3 RX 为 69,241,117 包，UPF 从 N6
成功返回 68,964,322 包，测试端 UDP FIFO-full 计数没有增加，说明该轮主要
损失在核心路径内，而非无线侧或 iperf 接收端。累计窗口与 iperf 的 30 秒窗口
不同，不能用这个差额替换表中的 UDP 丢包率。

TCP 两端使用官方 iperf3 3.22；UDP 发送端 3.22、接收端单线程 3.15，以避免
当前 VCL 与多线程 UDP 接收的兼容性/开销。下行由 DN 主动向 UE 发起，避免
`-R` 的额外 VCL 开销；检查实际本地/远端地址和 UPF 方向计数。UDP 实际接收
JSON 与客户端交换得到的 bytes/seconds/rate 已核对，丢包率按总丢包/总包数计算。

## 瓶颈证据

**额外接入拷贝曾是测量限制，排除后仍存在单会话 CPU 热点。** 原接入的负载
采样中，VPP Worker 约 88% 的用户态采样周期位于 memif RX/TX；零拷贝接入后
约为 64%。它仍要处理 UPF 原有 memif，并非完全消除核心的数据面成本。

**下行每包 report 清零是可定位的软件热点。** `ogs_pfcp_up_handle_pdr()` 对
`ogs_pfcp_user_plane_report_t` 执行全量初始化。实际二进制大小为 `0x2c8 = 712`
字节，编译为 `rep stosq`；常规转发也执行，并不代表每包发送一条 PFCP 消息。
在零拷贝 TCP DL 样本中，该指令命中函数内 397/420 个采样点；按采样 period
加权，占该会话 Worker 全部采样权重约 **25.1%**。不能把函数级百分比和这个
全线程分母直接混用。证据见 [hotspot.json](hotspot.json)、[pdr-asm.txt](pdr-asm.txt)
及原始 perf.data。

保持完整 712 字节清零语义的独立微基准中，`rep stosq` 中位数 16.332 ns/次，
libc `memset` 为 5.666 ns/次，约 2.88 倍差异。微基准在另一物理核上执行，
不能推算网元会提升 2.88 倍；它支持优先对比 libc/向量化清零实现，而不是
未经验证地省略 PFCP/URR 字段初始化。源码及数据见
[report_clear_bench.c](report_clear_bench.c)、[report-clear.csv](report-clear.csv)。

**线程拓扑呈现待验证的竞争因素。** 最终默认 UDP UL 中 Worker 45 和 N3
dispatcher 46 均接近满载，位于不同物理核；UDP DL 中 Worker 45 和 N6 dispatcher
117 均接近满载，却是同一物理核的两个 SMT。TCP 同时还处理反向 ACK，三个数据
线程均活跃，不能仅从线程占用推导换核收益，更不能推广到所有流量。
补充 UDP 绑核 A/B 时测试侧 VPP 停滞，未获得有效对照，因此 SMT 的可疑影响
没有被记为已证实的优化收益。恢复原布局后的核验单独留存。

**未发现 CFS 配额或内存容量是主限制。** 最终四个正式接收窗口中 UPF/VPP
新增节流均为 0、`memory.failcnt` 增量为 0，调度运行队列等待很低。内存 charge
约 UPF 194–196 MiB、VPP 334 MiB。轮询线程接近 100% 本身不能证明处理能力
饱和，因此结论同时依据路径计数、丢包扫描和指令采样。

**盲目增加进程、关闭未用 VF 没有解决主问题。** 关闭两个 VF 后 TCP
约 15.97/16.47 Gbit/s，与原接入默认结果接近，随后已恢复端口。TCP 既有 4 CPU
范围内的线程重排未呈现整体稳定收益。8 进程扩展也未提高 TCP，且测试端出现
高并发/小包 VCL 停滞；失败轮无完整后快照和汇总，未当作核心容量或核心故障。

单 PDU 按 `upf_dataplane_assign_session()` 分配给一个 Session Worker；单纯增加
`iperf -P` 或总 Session Worker 数不会把同一 PDU 自动拆到多个 Worker。后续优化
应先对报告初始化、批处理/完成回收和热点线程物理核布局做单变量对照，并验证
PFCP 报告、URR、包序与缓冲区生命周期。当前没有修改这些转发语义。

## 复现与清理

当前服务器保留了本轮使用的官方二进制。要求现场恰好存在一个默认 PDU
Session；脚本会拒绝会话变化、占用中的测试 memif 或与核心/无线侧共享物理核。

```bash
BENCH_DIR=/tmp/xcn-perf-retest
python3 perf-tools/scripts/iperf_core.py setup \
  --work-dir "$BENCH_DIR" --test-core-zero-copy \
  --iperf-binary /tmp/xcn-iperf-20261028/iperf-3.22/src/iperf3

python3 perf-tools/scripts/iperf_core.py run --work-dir "$BENCH_DIR" \
  --label tcp-ul --direction ul --processes 4 --streams 4 --seconds 30 --omit 3
python3 perf-tools/scripts/iperf_core.py run --work-dir "$BENCH_DIR" \
  --label tcp-dl --direction dl --processes 4 --streams 4 --seconds 30 --omit 3

docker cp /tmp/xcn-iperf-20261028/iperf-3.15/src/iperf3 \
  xcn-iperf-ran:/usr/local/bin/iperf3-server
python3 perf-tools/scripts/iperf_core_udp.py --work-dir "$BENCH_DIR" \
  --server-binary iperf3-server --label udp-ul --direction ul \
  --mbps 23000 --streams 4 --seconds 30 --omit 3
python3 perf-tools/scripts/iperf_core_udp.py --work-dir "$BENCH_DIR" \
  --server-binary iperf3-server --label udp-dl --direction dl \
  --mbps 18000 --streams 4 --seconds 30 --omit 3

python3 perf-tools/scripts/iperf_core.py cleanup --work-dir "$BENCH_DIR"
```

`bypass-on`/`bypass-off` 只临时改变测试路径，汇总会明确标注 `mode=bypass`。
[iperf_core_diag.py](../../scripts/iperf_core_diag.py) 从宿主 `/proc` 每秒采集线程和
cgroup；其 `--profile` 在命令开始后的 8–25 秒，以 99 Hz 记录 `cycles:u`，
需要预先存在 host-PID、具备 perf 权限的 `xcn-iperf-observer`，并将结果根目录
挂到 `/out`。该选项只用于至少 30 秒的诊断轮次，不以采样开销下的结果替代
全部无采样测量。

最终删除测试 memif 88/89、测试路由/socket 和辅助容器，恢复 UPF 线程默认绑核
及两个 VF。保留默认核心、单个主 UE；额外 5 UE 和背景流量保持停止。核心 Pod
11/11 就绪、各容器重启 0，清理后真实 OAI UE 到核心网关复测 5/5 ping 成功。
核验见 [verification.json](verification.json)。

## 证据索引

- [summary.json](summary.json)：有效轮次、发送/接收/丢包、稳态 CPU/配额/内存及无效轮次说明。
- [raw.tar.gz](raw.tar.gz)：逐进程 JSON、独立接收日志、前后路径快照、每秒指标、
  perf.data、采样报告、绑核与失败诊断记录。
- [environment.json](environment.json)：环境覆盖和实际镜像/二进制，未包含登录密钥。
- [上一轮记录](../2026-10-28/README.md)：历史接入方式的数据，不能混作当前零拷贝接入的最大值。
