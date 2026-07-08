# Open5GS UPF performance tools

这些工具用于绕过 OAI UE/gNB 的空口性能限制，直接压测 Open5GS UPF 的 N6/TUN 和 N3/GTP-U 数据面。

## 工具

- `udp_gen.c`: 下行 N6 侧 UDP 生成器。在 UPF 容器内运行，从 `ogstun` 网关地址向 UE 地址发 UDP。
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

上行 TEID 需要从真实 UE 上行包里取：

```bash
kubectl -n xcn exec "$POD" -c upf -- \
  tcpdump -i any -nn -vv -xx -c 2 "udp port 2152 and host $GNB_IP"

kubectl exec <nrue-pod> -- ping -c 2 -W 1 10.45.0.1
```

抓包里 gNB 到 UPF 的 GTP-U 头部 `TEID` 即 `UL_TEID`。

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
