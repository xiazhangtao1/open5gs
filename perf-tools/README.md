# Open5GS UPF performance tools

## Session 多 Worker

UPF 可在 N3/N6 均为 memif 时按 PFCP Session 分配 `1..16` 个数据 Worker。
推荐让 Helm 根据 UPF 容器的整数 CPU limit 自动计算：

```yaml
resources:
  fivegc:
    upf:
      requests:
        cpu: 8
      limits:
        cpu: 8
networking:
  upf:
    dataplane:
      sessionWorkers:
        enabled: true
        count: auto
        reservedCpus: 2
        queueSize: 8192
        busyPollUs: 20
    n3:
      memif:
        queues: auto
    n6:
      memif:
        queues: auto
```

自动模式按`worker = UPF逻辑CPU - reservedCpus`计算，因此上例为
`8 - 2 = 6`个Worker。CPU requests/limits必须相等且为整数；
`reservedCpus`至少为2，计算结果必须在`1..16`。数值型`count`继续支持手工
覆盖，`queues: auto`会跟随自动或手工Worker数。

N3/N6 的实际`memif.queues`必须与Worker数相等。N3按TEID、N6按UE IP
查找固定 Session owner；分发不绕过 PDR/FAR/QER/URR。一个 Session 只使用
一个 Worker，因此扩展性测试必须建立至少与 Worker 数相同的独立 PFCP Session。
Worker 使用普通 `SCHED_OTHER`。`busy_poll_us=0` 表示空闲时阻塞，正数表示
处理完一批后继续轮询指定微秒，`-1` 表示持续忙轮询；当前实测默认使用
`20us`。禁止把 busy-poll 线程改为 `SCHED_FIFO`。

N3/N6 各有一个专用 dispatcher 和外部 epoll，避免两个方向在同一入口线程
互相阻塞。Dispatcher 在每个 qid 每轮最多处理 `io_packet_budget` 个包或
`io_time_budget_us` 微秒，并在 qid 之间 round-robin。普通 G-PDU/IP 报文
以 descriptor lease 交给 Session Worker，不再在 dispatcher 复制 payload；
Worker 完成完整 UPF 处理后，由原 dispatcher 按 RX 顺序批量 refill。
控制报文、异常报文和 chained buffer 保持复制/回退处理。

`stats_interval` 指定累计运行时统计的日志周期（秒），除 burst、pending、
dispatch drop、queue high-water 和 memif TX ring-full 外，还包含
`in-flight`、`in-flight-max`、`refill-call`、`refill-packets` 和 `stale`。
`in-flight-max` 达到 memif ring 大小时，表示 descriptor 有序回收已成为入口
反压点；不能把这种情况简单归因于 Worker 睡眠。

这些工具用于绕过 OAI UE/gNB 的空口性能限制，直接压测 Open5GS UPF 的 N6/TUN 和 N3/GTP-U 数据面。

## 工具

- `udp_gen.c`: 下行 N6 侧 UDP 生成器。在 UPF 容器内运行，从 `ogstun` 网关地址向 UE 地址发 UDP；最后一个可选参数可固定源端口，用于命中 VPP NAT 回程五元组。
- `gtpu_gen.c`: 上行 fake RAN GTP-U 生成器。在 gNB Pod 内运行，使用当前 PDU session 的上行 TEID 向 UPF `2152/udp` 注入 G-PDU。
- `scripts/dl_diag.sh`: UPF 容器内下行统计脚本。
- `scripts/ul_diag.sh`: UPF 容器内上行统计脚本。
- `scripts/du_diag.sh`: UPF 容器内上下行并发统计脚本。
- `scripts/run_ul_once.sh`: 本机协调脚本，同时启动 UPF 侧上行统计和 gNB 侧 GTP-U 注入。
- `scripts/run_du_once.sh`: 本机协调脚本，同时启动 UPF 侧上下行统计/下行发包和 gNB 侧上行注入。
- `scripts/run_pg_dl_multi.sh`: VPP packet-generator 多 Session 下行发生器，
  将总速率平均分配到 `1..16` 个 UE IPv4。
- `scripts/run_pg_ul_multi.sh`: VPP packet-generator 多 Session 上行发生器，
  将总速率平均分配到 `1..16` 个当前 Session GTP-U pcap。
- `scripts/run_dl_qid_diag.sh`: 固定Pod、Session和qid的下行诊断脚本，同时采集
  Open5GS/VPP线程`schedstat`、CPU/NUMA/超线程拓扑、VPP memif ring快照、
  VPP runtime以及Open5GS各N3 TX qid计数。
- `scripts/run_vpp_affinity_ab.sh`: 在同一Pod、PFCP Session和VPP进程内按
  `dense(A1) -> isolated(B) -> dense(A2)`热切换VPP线程绑核，逐阶段采集
  实际affinity、`SCHED_OTHER`策略、`schedstat`、RX placement和吞吐计数。

多 Session 脚本默认按每 10us 计算一次 `maxframe`，设置
`PACING_10US=0` 可切换为 `maxframe=256` 的原突发方式。例如：

```bash
PACING_10US=1 perf-tools/scripts/run_pg_dl_multi.sh \
  "$POD" 4000 5 10.45.0.8 10.45.0.9 10.45.0.10

PACING_10US=0 perf-tools/scripts/run_pg_ul_multi.sh \
  "$POD" 4000 5 /tmp/session1.pcap /tmp/session2.pcap /tmp/session3.pcap
```

脚本输出的 `expected` 是目标包数，性能结论必须使用 `show interface` 中
实际送入和输出的 memif 包数；packet-generator 未达到 `expected` 时不能按
目标速率声明核心网吞吐。

固定同一Pod、Session和qid采集下行诊断：

```bash
POD=xcn-5gc-xxxxxxxxxx-xxxxx
VPP_MEMIF_SAMPLE_MS=500 perf-tools/scripts/run_dl_qid_diag.sh \
  "$POD" 1200 20 10.45.0.2 /tmp/open5gs-dl-qid-diag.log
```

进行VPP超线程共享/物理核隔离的严格A/B：

```bash
POD=xcn-5gc-xxxxxxxxxx-xxxxx
RUNS_PER_MODE=3 perf-tools/scripts/run_vpp_affinity_ab.sh \
  "$POD" 1200 20 10.45.0.2 /tmp/open5gs-vpp-affinity-ab.log
```

`dense`故意依次使用同一物理核的两个超线程，`isolated`则让VPP main和六个
Worker各占一个不同物理核的一个逻辑CPU。脚本不重启Pod、VPP或Session，并在
正常结束、失败和中断时均恢复`isolated`。两种模式都只使用`SCHED_OTHER`。
使用前应确认`/run/vpp/affinity-plan.json`包含至少7个
`physical_core_groups`；测试结论仍须以同一Session owner/TX qid为前提。

N3 TX burst间隔采样默认关闭，避免每次发送读取时钟影响性能。可在不重启
Pod、UPF进程或Session的情况下热切换：

```bash
# 开启；最多等待一个UPF统计周期（当前约11秒）生效。
kubectl -n xcn exec "$POD" -c upf -- \
  touch /tmp/open5gs-n3-tx-timing

# 关闭。
kubectl -n xcn exec "$POD" -c upf -- \
  rm -f /tmp/open5gs-n3-tx-timing
```

日志出现`N3 memif TX timing diagnostics enabled/disabled`后再开始A/B。必须
保持Pod UID、PFCP Session、UE IP、Session owner、worker和TX qid均不变；
跨Pod或UE重建的测试只能作为新基线，不能作为代码优化A/B。

## 编译

在 Ubuntu 主机上执行：

```bash
gcc -O2 -Wall -Wextra -o /tmp/udp_gen perf-tools/udp_gen.c
gcc -O2 -Wall -Wextra -o /tmp/gtpu_gen perf-tools/gtpu_gen.c
```

## 获取当前环境参数

```bash
kubectl -n xcn get pods -o wide
kubectl get pods -o wide

POD=<xcn-5gc-pod>
GNB_POD=<gnb-pod>
GNB_IP=<gnb-pod-ip>

kubectl -n xcn exec "$POD" -c upf -- ip route get "$GNB_IP"
```

`ip route get` 输出里的 `dev <iface>` 就是 `VETH`。

上行 TEID 需要从真实 UE 上行包里取。UDP N3 可直接在 UPF 抓包：

```bash
kubectl -n xcn exec "$POD" -c upf -- \
  tcpdump -i any -nn -vv -xx -c 2 "udp port 2152 and host $GNB_IP"

kubectl exec <nrue-pod> -- ping -c 2 -W 1 10.45.0.1
```

抓包里 gNB 到 UPF 的 GTP-U 头部 `TEID` 即 `UL_TEID`。

N3 memif 模式下 VF 由 DPDK 接管，应使用 VPP pcap trace：

```bash
kubectl -n xcn exec "$POD" -c vpp -- \
  vppctl -s /run/vpp/cli.sock pcap trace rx max 2000 intfc dpdk-n3 file gtpu.pcap
kubectl exec <nrue-pod> -- ping -I oaitun_ue1 -c 3 -W 1 10.2.0.119
kubectl -n xcn exec "$POD" -c vpp -- \
  vppctl -s /run/vpp/cli.sock pcap trace off
kubectl -n xcn cp "$POD":/tmp/gtpu.pcap /tmp/gtpu.pcap -c vpp
tcpdump -nn -r /tmp/gtpu.pcap \
  'udp dst port 2152 and dst host <N3-advertise-address>' -XX
```

## 部署工具

```bash
kubectl cp /tmp/udp_gen xcn/$POD:/tmp/udp_gen -c upf
kubectl cp /tmp/gtpu_gen default/$GNB_POD:/tmp/gtpu_gen
kubectl cp perf-tools/scripts/dl_diag.sh xcn/$POD:/tmp/dl_diag.sh -c upf
kubectl cp perf-tools/scripts/ul_diag.sh xcn/$POD:/tmp/ul_diag.sh -c upf
kubectl cp perf-tools/scripts/du_diag.sh xcn/$POD:/tmp/du_diag.sh -c upf

kubectl -n xcn exec "$POD" -c upf -- chmod +x /tmp/udp_gen /tmp/dl_diag.sh /tmp/ul_diag.sh /tmp/du_diag.sh
kubectl exec "$GNB_POD" -- chmod +x /tmp/gtpu_gen
```

## 下行测试

```bash
kubectl -n xcn exec "$POD" -c upf -- env \
  GNB_IP="$GNB_IP" VETH="$VETH" UE_IP=10.45.0.2 UPF_TUN_IP=10.45.0.1 \
  /tmp/dl_diag.sh 1000 10
```

计数方式：

- `udp_gen sent_pkts`: N6 侧注入包数。
- `ogstun tx_packets`: UPF 从 TUN 下行读出并向 N3 发出的包数。
- `ogstun tx_dropped`: TUN 下行方向丢包。
- `iptables OUTPUT gtpu-dl-test`: GTP-U 下行输出包数。

## 上行测试

```bash
export POD GNB_POD GNB_IP VETH UL_TEID
export UPF_IP=10.2.0.119 UE_IP=10.45.0.2 UPF_TUN_IP=10.45.0.1

kubectl -n xcn exec "$POD" -c upf -- env \
  GNB_IP="$GNB_IP" VETH="$VETH" /tmp/ul_diag.sh 20
```

或用协调脚本：

```bash
env POD="$POD" GNB_POD="$GNB_POD" GNB_IP="$GNB_IP" VETH="$VETH" \
  UL_TEID="$UL_TEID" UPF_IP=10.2.0.119 UE_IP=10.45.0.2 UPF_TUN_IP=10.45.0.1 \
  perf-tools/scripts/run_ul_once.sh 1200 10
```

### VPP memif N3/N6 计数口径

N3/N6 都为 memif 时不再使用 `ogstun` 或 Linux UDP socket 计数。仍可复用
`gtpu_gen` 绕过 OAI 空口，但必须使用当前会话的 UL TEID，并对比 VPP 四段
计数：

```bash
# 测试前清零，测试后读取四段计数和错误。
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock clear interfaces
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock clear errors

kubectl exec "$GNB_POD" -- /tmp/gtpu_gen \
  "$UPF_IP" "$UL_TEID" 10.45.0.2 10.2.0.119 10 1000 1400

kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock show interface
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock show errors
```

测试目标应运行静默 UDP sink（例如 `nc -u -l -p 9999 >/dev/null`），避免 ICMP
或 UDP 回包污染单上行计数。正常情况下应满足：

```text
gtpu_gen sent_pkts - dpdk-n3 rx-miss ≈ memif2/0 tx packets
memif2/0 tx packets >= memif1/0 rx packets = dpdk-n6 tx packets
```

`memif2/0` 是 VPP 到 Open5GS 的 N3，`memif1/0` 是 Open5GS 到 VPP 的 N6。
两者差值表示 Open5GS 处理或 memif ring 压力；`memif1/0` 与 `dpdk-n6` 的差值
表示 VPP NAT/转发压力。检查 `show errors` 中的 `no free tx slots`、NAT handoff
congestion 和 VF `rx-miss`。每次 UE/UPF 重建后都必须重新抓取 UL TEID。
使用 VPP packet-generator 在上下行测试之间切换时，必须先删除前一方向的所有
stream；仅执行 `packet-generator disable` 不会删除未完成的 stream，下一次
`enable` 会继续发送并污染接口计数。

计数方式：

- `gtpu_gen sent_pkts`: fake RAN 注入到 UPF 的 GTP-U 包数。
- `ogstun rx_packets`: UPF 解封装后写入 TUN/N6 方向的包数。
- `Udp InErrors` 增量: UDP socket 接收侧丢包。

## 上下行并发

```bash
env POD="$POD" GNB_POD="$GNB_POD" GNB_IP="$GNB_IP" VETH="$VETH" \
  UL_TEID="$UL_TEID" UPF_IP=10.2.0.119 UE_IP=10.45.0.2 UPF_TUN_IP=10.45.0.1 \
  perf-tools/scripts/run_du_once.sh 1000 500 10
```

## 注意事项

- 脚本会临时插入 `iptables` 计数规则，结束时删除同一规则。
- 每次 gNB/UPF Pod 重建后，`GNB_IP`、`VETH` 和 `UL_TEID` 都可能变化，必须重新获取。
- `AMBR` 当前会被 SMF 下发到 PFCP QER MBR，但 Open5GS UPF 数据面未实际基于 `qer->mbr` 做限速。
- VPP memif 模式下，UE IPv4 数据面已验证；当前模板不提供 IPv6 N6 路由/NAT。
