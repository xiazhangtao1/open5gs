# Open5GS UPF performance tools

## Session 多 Worker

UPF 可在 N3/N6 均为 memif 时按 PFCP Session 分配 `1..16` 个数据 Worker：

```yaml
upf:
  dataplane:
    session_workers:
      enabled: true
      count: 4
      queue_size: 8192
```

N3/N6 的 `memif.queues` 必须与 `count` 相等。N3 按 TEID、N6 按 UE IP
查找固定 Session owner；分发不绕过 PDR/FAR/QER/URR。一个 Session 只使用
一个 Worker，因此扩展性测试必须建立至少与 Worker 数相同的独立 PFCP Session。
Worker 使用普通 `SCHED_OTHER`，禁止把 busy-poll 分发线程改为无限制
`SCHED_FIFO`。

这些工具用于绕过 OAI UE/gNB 的空口性能限制，直接压测 Open5GS UPF 的 N6/TUN 和 N3/GTP-U 数据面。

## 工具

- `udp_gen.c`: 下行 N6 侧 UDP 生成器。在 UPF 容器内运行，从 `ogstun` 网关地址向 UE 地址发 UDP；最后一个可选参数可固定源端口，用于命中 VPP NAT 回程五元组。
- `gtpu_gen.c`: 上行 fake RAN GTP-U 生成器。在 gNB Pod 内运行，使用当前 PDU session 的上行 TEID 向 UPF `2152/udp` 注入 G-PDU。
- `scripts/dl_diag.sh`: UPF 容器内下行统计脚本。
- `scripts/ul_diag.sh`: UPF 容器内上行统计脚本。
- `scripts/du_diag.sh`: UPF 容器内上下行并发统计脚本。
- `scripts/run_ul_once.sh`: 本机协调脚本，同时启动 UPF 侧上行统计和 gNB 侧 GTP-U 注入。
- `scripts/run_du_once.sh`: 本机协调脚本，同时启动 UPF 侧上下行统计/下行发包和 gNB 侧上行注入。

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
