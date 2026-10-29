# 四用户、每用户单 PDU Session 吞吐测试

沿用上一轮的核心镜像、Pod 和默认资源配置，建立 4 个真实用户，每用户恰好
1 个 PDU/PFCP Session。四用户同时发流，上下行分别测量；数据绕过 OAI 无线
协议栈和物理网卡，经过真实 N3/GTP-U、UPF 会话规则和 N6。

## 结果

以下为接收端应用有效载荷速率，十进制 Gbit/s。正式轮次均为 30 秒测量、
3 秒预热。每用户 1 个 iperf 进程、4 条传输流，共 16 条传输流；这些流属于
4 个 PDU Session，并不是 16 个用户或 16 个 PDU Session。

| 口径 | 四用户总上行 | 四用户总下行 |
| --- | ---: | ---: |
| TCP，两轮平均 | **17.603** | **16.119** |
| TCP，两轮中的最高观测值 | 17.653 | 16.250 |
| UDP，重复通过低丢包标准的发送档位 | **22.500** | **18.000** |

UDP 接收速率取三位小数也为 22.500 / 18.000 Gbit/s。两个选定档位各测两轮，
**四个用户在这四轮的测量窗口内均未观察到丢包**。低丢包标准自行定义为：
总丢包率及每个用户的丢包率都 ≤0.1%。零观测丢包仅对应有限测量窗口。
TCP 上行两轮为 17.653 / 17.553，下行为 15.988 / 16.250 Gbit/s。

### 每用户吞吐

TCP 列为两轮平均；UDP 列为最后一轮的接收速率。各用户均有独立 IP、SEID、
上下行 TEID，测试前后检查身份和会话不变，并确认每个 Session 的对应方向
字节计数都增长。完整 SUPI 和 TEID 在原始快照中。

| IMSI 末三位 | UE IP | PFCP SEID | TCP 上行 | TCP 下行 | UDP 上行 | UDP 下行 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 100 | 10.45.0.3 | 2520 | 4.414 | 4.172 | 5.625 | 4.500 |
| 101 | 10.45.0.101 | 653 | 4.630 | 4.134 | 5.625 | 4.500 |
| 102 | 10.45.0.102 | 3483 | 4.259 | 4.057 | 5.625 | 4.500 |
| 103 | 10.45.0.103 | 3333 | 4.300 | 3.757 | 5.625 | 4.500 |

这些值体现本次并发流量的分配，不代表调度器承诺固定公平份额。TCP 速率来自
实际接收端 JSON；UDP 客户端交换得到的接收统计与独立服务端日志逐项核对。

### UDP 边界扫描

载荷 1400 字节，内层 IPv4 包 1428 字节，含 QFI 的 N3 外层为 1472 字节。
以 0.5 Gbit/s 步长细化边界；表中接收速率未计入协议头。

| 方向 | 总发送档位 | 实际接收 Gbit/s | 总丢包率 | 每用户均通过 ≤0.1% |
| --- | ---: | ---: | ---: | --- |
| UL | 22.0 | 22.000 | 0.001821% | 是 |
| UL | 22.5 | 22.500 | 0% | 是，最后复测仍为 0% |
| UL | 23.0 | 22.975 | 0.111138% | 否，最差用户 0.122010% |
| UL | 24.0 | 22.575 | 5.938050% | 否 |
| DL | 17.0 | 17.000 | 0% | 是 |
| DL | 18.0 | 18.000 | 0% | 是，最后复测仍为 0% |
| DL | 18.5 | 18.376 | 0.665786% | 否 |
| DL | 19.0 | 17.400 | 8.434473% | 否 |

因此本轮报告当前测试条件下重复通过的档位 22.5 / 18 Gbit/s，不把高丢包时的
接收速率当成可用容量，也不将这些离散、有限窗口结果视为核心网理论极限。

## 默认配置与测量边界

- 基线提交 `6c221375e`，Helm revision 213，核心 Pod UID
  `df54c1ae-2dbe-4751-9dd5-18d514c22934`。整个测试及清理期间未重建核心 Pod，
  未改核心转发源码、CPU 配额、绑核或队列。
- UPF 4 个逻辑 CPU `45-46,117-118`，对应 2 个物理核；VPP 2 个逻辑 CPU
  `47,119`，对应 1 个物理核。实际 UPF 配置为 1 个 Session Worker，N3/N6
  各 1 个 memif 队列，8192 ring、2048 buffer，保留监控、rate stats 和默认配额。
- 从原有单 UE 环境启动 nrue-101、nrue-102、nrue-103 三个已有部署，104/105
  仍为 0 副本。每用户 PSI 1、QFI 1、5QI 9，且恰好 1 个 PDU 和 PFCP Session。
- 测试 VPP 使用 CPU 60–63，UE 应用使用 64–67，DN 应用使用 68–71；与核心、
  gNB 和四个 OAI UE 分开物理核。两条新增测试 memif 采用核心侧 slave/zero-copy，
  UPF 原有 memif1/0、memif2/0 保持原配置。
- TCP 两端 iperf3 3.22；UDP 发送端 3.22、接收端单线程 3.15，与上一轮相同。
  UDP 下行由 DN 主动向 UE 发起，核对实际源/目的地址；不是通过 `-R` 推断方向。
- 同机共享内存路径不经过 X710 网线，故结果可以超过 10 Gbit/s。现场 ens5f0
  实际协商 1000 Mb/s、ens5f1 无链路；物理口计数保存在每轮快照。
- 签约上下行 AMBR 仍各为 1 Gbit/s。实际转发超过该值，本测试衡量数据面处理
  吞吐，不验证 AMBR 限速功能。
- 多进程按各自接收窗口求和。最后 UDP 四进程开始时间相差约 0.321 秒；它们
  存在完整的并发稳态区间，但不是严格同步的线速认证。CPU 从共同稳态窗口取样。

完整环境、镜像 digest、资源和二进制哈希见 [environment.json](environment.json)。

## 容量限制与对照

**四个 Session 都归 Worker 0，用户数增加没有自动增加 Worker。** 本轮 SMF 和
UPF 统计均确认四个独立会话，UPF 的 `worker` 字段全部为 0。实际配置中的
`session_workers.count` 为 1；会话归属由
[upf_dataplane_assign_session()](../../../src/upf/dataplane.c) 决定。
因此四用户仍共享默认的会话处理、N3/N6 派发和 VPP 转发资源，总吞吐没有
随用户数变为四倍。

最终 UDP UL 的 Worker 45 约 100%、N3 dispatcher 46 约 98.7%；DL 的 Worker 45
约 100%、N6 dispatcher 117 约 99.7%。其中 45/117 是同一物理核的两个 SMT。
核心 VPP Worker 119 约 100%，包含忙轮询，不能只凭利用率判定它的容量。
本轮没有验证改变 SMT 布局或增加 Worker 的收益。

**过载损失主要位于 UPF 输入到成功返回的区间。** 包计数包含预热及控制包，
窗口比 iperf 的 30 秒更长，只用于定位，不能替代应用层丢包率：

| 过载轮次 | 核心发往 UPF 的包数 | UPF 成功返回核心 VPP | 核心转发到测试接收侧 |
| --- | ---: | ---: | ---: |
| UDP UL 24 Gbit/s | 70,712,358 | 66,882,211 | 66,882,211 |
| UDP DL 19 Gbit/s | 55,982,186 | 51,360,843 | 51,360,843 |

这两轮测试侧 UDP FIFO-full 错误没有增加。UL 24 Gbit/s 的共同稳态窗口内，
UPF `nr_throttled` 增加 30，`throttled_time` 累计增加约 2.199 秒；UL 23 Gbit/s
增加 2 次、约 30.6 ms。它们保留默认配额，过载上行结果可能同时受节流影响。
DL 19 Gbit/s 同窗口无节流，仍有明显损失。上述 cgroup 累计计数不能直接等同
端到端停顿时间。

最终两轮 TCP 和两个 UDP 正式窗口中，UPF/VPP 新增节流均为 0，内存 failcnt
增量均为 0；UPF 内存 charge 约 203 MiB、VPP 约 334–335 MiB。未观察到内存
容量限制。上一轮定位的逐包 PFCP report 清零热点未修改；本轮未重新做指令
采样，相关历史证据见[单用户报告](../2026-10-29/README.md)。

**测试端余量按方向分别判断。** 临时用核心 VPP GTP-U 代替 UPF，保留四用户
及相同测试端，得到以下 15 秒测量、3 秒预热的对照；这些值未计入核心结果：

| 旁路对照 | 实际发送 Gbit/s | 实际接收 Gbit/s | UDP 丢包率 |
| --- | ---: | ---: | ---: |
| TCP UL | 22.201 | 22.212 | 不适用 |
| TCP DL | 23.554 | 23.567 | 不适用 |
| UDP UL，请求总速率 24 | 22.994 | 22.916 | 0.307141% |
| UDP DL，请求总速率 24 | 24.000 | 24.000 | 0% |

TCP 和 UDP 下行对照有明确余量。UDP 上行旁路未达到请求发送速率，且有丢包，
因此不将其当成测试工具具备充足上行余量的证明；旁路本身也改变了核心 VPP
承担的 GTP-U 工作。上行结论结合实际核心过载轮次达到发送目标、接口包数差额
及接收 FIFO 计数，仍保留接近容量边界时的工具限制。

与上一轮单用户最终复测 TCP 17.617 / 16.547、UDP 23 / 18 Gbit/s 相比，本轮
四用户总吞吐处于同一量级。两轮会话数、运行时负载及部分统计口径不同，
这些小幅差异不作为严格的性能回退结论。

## 复现与清理

先确保四个用户均完成真实会话建立，且没有其他活动 PDU Session。现有
nrue-101/102/103 部署可通过以下命令启动：

```bash
kubectl scale deployment nrue-101-oai-nr-ue nrue-102-oai-nr-ue nrue-103-oai-nr-ue --replicas=1

BENCH_DIR=/tmp/xcn-four-user-retest
python3 perf-tools/scripts/iperf_core.py setup \
  --work-dir "$BENCH_DIR" --session-count 4 --test-core-zero-copy \
  --iperf-binary /tmp/xcn-iperf-20261028/iperf-3.22/src/iperf3

python3 perf-tools/scripts/iperf_core.py run --work-dir "$BENCH_DIR" \
  --label tcp-ul --direction ul --processes 4 --streams 4 --seconds 30 --omit 3
python3 perf-tools/scripts/iperf_core.py run --work-dir "$BENCH_DIR" \
  --label tcp-dl --direction dl --processes 4 --streams 4 --seconds 30 --omit 3

docker cp /tmp/xcn-iperf-20261028/iperf-3.15/src/iperf3 \
  xcn-iperf-ran:/usr/local/bin/iperf3-server
python3 perf-tools/scripts/iperf_core_udp.py --work-dir "$BENCH_DIR" \
  --server-binary iperf3-server --label udp-ul --direction ul \
  --mbps 22500 --processes 4 --streams 4 --seconds 30 --omit 3
python3 perf-tools/scripts/iperf_core_udp.py --work-dir "$BENCH_DIR" \
  --server-binary iperf3-server --label udp-dl --direction dl \
  --mbps 18000 --processes 4 --streams 4 --seconds 30 --omit 3

python3 perf-tools/scripts/iperf_core.py cleanup --work-dir "$BENCH_DIR"
```

脚本将进程按 SUPI 排序分配给用户，每用户本轮恰好一个进程。`--mbps` 是所有
进程、所有流的总发送目标，不是每用户目标。运行和 UDP 脚本从 setup 状态恢复
会话数；默认单用户用法仍保留。少于四个进程会在发流前被拒绝。

已删除新增 memif 88/89、socket、测试路由和辅助容器；旁路隧道及路由已恢复。
保留原核心和四个真实 UE，默认 CPU 配额和绑核保持不变。核心 11/11 就绪、
各容器重启计数 0；清理后四个真实 UE 到网关各自复测 **5/5 ping 成功**，
暖机 ping 也原样记录在 [verification.json](verification.json)。

## 证据

- [summary.json](summary.json)：19 个有效轮次，含 1 个短测和 4 个旁路对照；
  TCP 均值仅计算四个 30 秒正式轮次。记录逐用户、CPU、配额和接口计数差额。
- [raw.tar.gz](raw.tar.gz)：逐进程 JSON、独立接收日志、前后快照、每秒指标、
  实际 UPF 配置、配额层级、复算脚本和选择清单；不包含登录密钥或原始完整 Pod 配置。
- [environment.json](environment.json)：本轮实际镜像、CPU/资源、链路、工具及脚本哈希。
- [verification.json](verification.json)：清理、默认绑核、核心状态及四个真实 UE 连通性。
- [SHA256SUMS](SHA256SUMS)：报告与证据文件校验和。
