# VPP memif 独立性能测试

2026-10-29 实测。当前核心网的 VPP 保持 **1 个 Worker**，memif 为
master/copy、IP 模式、4 对队列、ring 8192、buffer 2048 字节。
结果为两个独立的 30 秒窗口的接收端平均值，均预热 3 秒；RX/TX 分开测量。

## 严格零丢包补测

零丢包定义为从 PG 实际提交报文至 memif 排空，**实际丢失包数恰好为 0**，
不使用百分比四舍五入，也不沿用下面的 ≤0.1% 标准。RX 另做补测，TX 沿用
原先两轮均为零丢包的正式结果。以下是各自两轮 30 秒有效测试的实收均值：

| 报文总长 | RX Gbit/s | RX Mpps | TX Gbit/s | TX Mpps |
| --- | ---: | ---: | ---: | ---: |
| 1400 B | 58.800 | 5.250 | 78.290 | 6.990 |
| 64 B | 2.560 | 5.000 | 17.601 | 34.377 |

| RX 补测档位 | 第一轮 Gbit/s / Mpps / 丢失包数 | 第二轮 Gbit/s / Mpps / 丢失包数 |
| --- | ---: | ---: |
| 1400 B，5.25 Mpps | 58.800318 / 5.250028 / 0 | 58.800147 / 5.250013 / 0 |
| 64 B，5 Mpps | 2.559999 / 4.999998 / 0 | 2.560000 / 4.999999 / 0 |

这两个 RX 档位是本轮**已重复验证的零丢包速率**，不能当作 memif 的绝对
零丢包上限。原来 10 秒扫描中零丢包的档位，在更长窗口中未必能重复：
1400 B / 5.5 Mpps 的本次 30 秒测试丢失 24,262 包；64 B / 10 Mpps 第一轮
零丢包，第二轮丢失 25,703 包。此前 5.75 / 37.5 Mpps 的低丢包 RX 值均不
属于严格零丢包结果。

小包高档位出现约 5 秒周期的短时突发丢包，对端计数为 `no free tx slots`。
这些突发丢包不能说明 memif 在 5 Mpps 就达到持续收包极限：定时 PG 还出现过设定
7.5 Mpps、实际约 42.406 Mpps 的异常轮次，已排除这些无效轮次；其原因
尚未最终定位。尝试临时隔离 readiness 的 CLI 访问，丢包仍然存在，不能
据此认定 readiness 是根因，原 CLI socket 已恢复。

为排除定时 PG 的影响，另使用不限速、固定批量发包。较高的固定批量也未
重复达到零丢包：`maxframe 2` 的短测约 24.699 Mpps 发送、丢失 64,642 包；
`maxframe 1` 的 10 秒探测约 12.480 Mpps 且零丢包，但接着的 30 秒测试
实际发送约 13.988 Mpps、丢失 88,350 包。因此表中保留两轮均有效且实际
丢失包数为零的结果，不把工具或瞬时突发造成的限制解释为 memif 函数极限。
所选四个 RX 补测窗口均无新增 CPU 配额节流、内存分配失败或 IP/GTP 查表处理。

## 原低丢包档位（≤0.1%）

| 报文总长 | 方向（相对核心 VPP） | 实收 Gbit/s | 实收 Mpps | 两轮最差丢包率 |
| --- | --- | ---: | ---: | ---: |
| 1400 B | RX | 64.400 | 5.750 | 0.00004471% |
| 1400 B | TX | 78.290 | 6.990 | 0% |
| 64 B | RX | 19.186 | 37.472 | 0.08130426% |
| 64 B | TX | 17.601 | 34.377 | 0% |

RX 重复通过的发送档位分别为 **5.75 / 37.5 Mpps**，实际发送与目标偏差
小于 1%，完整发送至排空的丢包率均 ≤0.1%。这是已验证的低丢包档位，
没有穷尽绝对上限；64 B RX 不能称为零丢包吞吐。TX 不限速发包，表中为
最小收发路径的持续实测速率，仍包含 DUT 内置 PG 的生成开销。

## 测量范围

使用正在运行的 VPP v26.06-release，新增一个同参数临时 memif，保留生产
memif 及 UPF 配置。主线程和唯一 Worker 使用同一物理核的两个 SMT 线程；
测试对端使用同 NUMA 的 4 个独立物理核作为 Worker。处理器为
Intel Xeon Gold 6554S。

测试对端是 VPP slave/zero-copy，用于排除 UPF/libmemif 应用和无线模拟器的
处理能力限制。被测核心 VPP 始终是 master/copy，没有切换为零拷贝模式。
RX 的四条队列均由同一个 DUT Worker 轮询；TX 由同一 Worker 使用其默认
TX 队列。四条队列并不代表四个 VPP 转发 Worker。

路径分别为：

- RX：对端 PG → memif → DUT 的禁用 IP 接口丢弃终点。
- TX：DUT PG → memif → 对端的禁用 IP 接口丢弃终点。

报文是有效 IPv4/UDP 数据，1400/64 B 均指 **IPv4 报文总长**；UDP 载荷分别
为 1372/36 B。计算为 `Gbit/s = PPS × 报文总长 × 8 / 10^9`，不含以太网、
FCS、前导码、IFG 或 GTP 开销。终点经过基础 IP 输入检查和 buffer 释放，
不做 IP 路由、UDP socket、UPF 规则或会话处理。

正式测试暂时关闭原有 N3/N6 DPDK 接口，去掉网卡收发和空轮询的影响；数据
完全通过服务器共享内存传输，因此不受 X710 物理端口的 10 Gbit/s 限制。
八个正式窗口中，DUT 的 IP 查表、UDP 查表、GTP 收发节点增量全部为零。

## 重复性与发包端余量

| 报文总长 / 方向 | 第一轮 Gbit/s / Mpps | 第二轮 Gbit/s / Mpps |
| --- | ---: | ---: |
| 1400 B RX | 64.399965 / 5.749997 | 64.399923 / 5.749993 |
| 1400 B TX | 78.275037 / 6.988843 | 78.304362 / 6.991461 |
| 64 B RX | 19.185954 / 37.472567 | 19.185310 / 37.471309 |
| 64 B TX | 17.594012 / 34.363304 | 17.607602 / 34.389848 |

隔离前的 PG → error-drop 基线为：DUT 单 Worker 在 1400/64 B 下分别达到
26.926/111.486 Mpps；对端四 Worker 合计达到 107.620/423.503 Mpps。
发包器单独运行有明显余量，但这不能消除其与 memif 共享 DUT CPU 的成本，
所以 TX 结果不等同于 memif TX 函数独占 CPU 的极限。

隔离后的较高 RX 探测档位 1400 B / 6.5 Mpps、64 B / 45 Mpps，丢包率分别
为 8.816% / 13.789%，均不计为通过。四队列仍在单个 VPP Worker 上串行
执行，不能用增加 UPF Session Worker 数量替代 VPP 收发能力扩展。

八个正式窗口均没有新增 CPU 配额节流或内存分配失败。吞吐/PPS 使用共享
统计段的接收包数、字节数与单调时钟差计算，每秒保存一次计数。
丢包使用 PG 实际提交数减排空后的接收数，包含预热阶段；PG 预填充 FIFO
缓冲数不算已发送报文。终点主动丢弃与 memif 传输丢包分开统计。

## 复现

工具为 [memif_only.py](../../scripts/memif_only.py)，依赖本机 Docker、kubectl
和缓存的匹配 VPP 镜像。先确认对端 CPU 可用，并按机器配置调整
`--peer-main` / `--peer-workers`。当前工具要求 DUT 恰好有一个 VPP Worker。
下面复现严格零丢包补测的 RX 档位；原低丢包对照分别使用 5.75 / 37.5 Mpps。
本次还新增 `--maxframe 1..256`，可配合 `--rate-mpps 0` 做固定批量对照。
定时发包必须另外核验实际发送速率，不能只看所配置的目标值。

```bash
memif_bench_dir=$(mktemp -d /tmp/memif-only.XXXXXX)
python3 perf-tools/scripts/memif_only.py setup --work-dir "$memif_bench_dir" \
  --peer-main 62 --peer-workers 64-67
trap 'python3 perf-tools/scripts/memif_only.py cleanup --work-dir "$memif_bench_dir"' EXIT

# 在维护窗口关闭原有数据接口；cleanup 会恢复原始 admin 状态。
python3 perf-tools/scripts/memif_only.py quiesce --work-dir "$memif_bench_dir" \
  --quiet-interface dpdk-n3 --quiet-interface dpdk-n6

for memif_round in 1 2; do
  python3 perf-tools/scripts/memif_only.py run --work-dir "$memif_bench_dir" \
    --label "rx1400-r$memif_round" --direction rx --size 1400 --rate-mpps 5.25
  python3 perf-tools/scripts/memif_only.py run --work-dir "$memif_bench_dir" \
    --label "rx64-r$memif_round" --direction rx --size 64 --rate-mpps 5
  for memif_size in 1400 64; do
    python3 perf-tools/scripts/memif_only.py run --work-dir "$memif_bench_dir" \
      --label "tx${memif_size}-r$memif_round" --direction tx --size "$memif_size"
  done
done
```

本次已实际验证最终工具的完整创建、隔离、双向低速收发和清理流程；原始
计数器、环境快照和扫描记录仅在本机保存。本文件仅发布汇总测量数据。
测试后临时 memif、PG 和辅助容器均已删除，N3/N6 接口恢复 UP；核心网
11/11 就绪，四个真实 UE 各自 ping 网关 5/5 成功，四个 Session 仍分别
属于 UPF Workers 0–3。
