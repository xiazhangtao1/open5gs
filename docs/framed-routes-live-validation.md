# IPv4 后路由真实会话验证

验证日期：2026-10-14，节点 `xnet-server`。本次完成当前 VPP/memif 模式的验证。
TUN 模式仅检查代码与配置，尚未切换现网实测，不计为通过。

## 测试链路与边界

使用当前已部署的 OAI RFsim gNB、nrUE 和 xcn 核心网，没有使用模拟核心网或
绕过无线协议栈的 GTP-U 注入器。RFsim 是软件无线环境，本次不代表硬件 CPE 验收。

| 项目 | 实际配置 |
| --- | --- |
| 用户 | IMSI `460110000000100` |
| 会话 | `cmnet`，SST `1`，SD `010101`，PSI `2` |
| UE 地址 | `10.45.0.3`，`oaitun_ue1` |
| 后方设备 | UE 内独立网络命名空间，`198.18.252.2/24` |
| UE 后方接口 | veth `frue0`，网关 `198.18.252.1/24`，MTU `1440` |
| DN 测试端 | `10.2.0.119` |
| 数据面 | VPP `26.06`，N6 `memif1/0`，N3 `memif2/0` |

```text
DN 10.2.0.119 ↔ VPP dpdk-n6 ↔ memif1/0 ↔ UPF
  ↔ memif2/0 ↔ VPP dpdk-n3 ↔ OAI gNB ↔ RFsim nrUE
  ↔ oaitun_ue1 ↔ UE 转发 ↔ veth ↔ 独立后方设备 198.18.252.2
```

通过生产 WebUI 给该用户添加临时 `198.18.252.0/24` 后路由，保存接口返回 200，
再重启 nrUE 建立新 PDU 会话。AMF/SMF 状态确认该用户已连接且会话 active，
UPF N3 地址为 `10.2.0.230`。后方设备使用独立协议栈，不将其地址直接配置在 UE 本机。
UE 临时开启 `ip_forward`，增加后方源地址到现有 PDU 路由表 `10002` 的策略路由；
UE 上没有为后方设备配置 SNAT。

## 实际结果

| 场景 | 结果 |
| --- | --- |
| 原 UE 到 DN 的基线 ping | 5/5 |
| 已配置订阅后路由，但未加 VPP 网段路由 | 后方设备到 DN，0/3 |
| 加入 VPP 网段路由后，后方设备到 DN | 20/20，0% 丢包 |
| DN 已有回程路由，但保持原 NAT44 策略 | DN 主动访问后方设备，0/3 |
| 为单个测试设备配置 NAT identity mapping 和精确路由后 | DN 主动访问后方设备，20/20，0% 丢包 |
| 双向 UDP 回显 | 每个方向 64、512、1400 字节各 20 包，共 60/60 |
| 双向 TCP 回显 | 每个方向 65536 字节，逐字节一致并记录 SHA256 |
| UE 隧道抓包 | 555 个 IPv4 包，包含双向 TCP 握手、数据和 UDP |

成功抓包中的后方源/目的地址为 `198.18.252.2`，证明数据经实际 UE 后路由转发，
不是只用 UE 自身 `10.45.0.3` 测试。双向 TCP/UDP 结果均取自修正测试条件后的完整复验。

## 当前 memif 部署需要的配置

页面后路由用于 UPF 查找 UE，VPP 静态路由用于将报文送到 UPF，两者不能互相替代。
本次临时命令如下；地址是专用测试网段，不应直接作为业务配置照搬。

```bash
kubectl exec -n xcn deploy/xcn-5gc -c vpp -- \
  vppctl ip route add 198.18.252.0/24 via memif1/0

# DN 位于本机，需把后方网段送往 VPP 的 N6 地址。
kubectl exec -n xcn deploy/xcn-5gc -c upf -- \
  ip route add 198.18.252.0/24 via 10.2.0.232 dev ens5f0

# 原部署开启 NAT44；仅给测试设备增加不转换地址的映射。
kubectl exec -n xcn deploy/xcn-5gc -c vpp -- \
  vppctl nat44 add identity mapping 198.18.252.2
kubectl exec -n xcn deploy/xcn-5gc -c vpp -- \
  vppctl ip route add 198.18.252.2/32 via memif1/0
```

identity mapping 会自动安装 `nat-low` 来源的本地接收 `/32` 路由。
如果只有 `/24` 路由，ping 可能收到 VPP 本地回复（本次 RTT 约 0.02 ms、TTL 64），
而 TCP/UDP 不通，不能记为后方设备通过。补充 CLI `/32` 后，FIB 确认实际出口是
`memif1/0`，经 UE 的 ping RTT 约 6 ms、TTL 62，TCP/UDP 和 UE 抓包同时通过。
本次没有全局关闭 NAT44，也没有启用全局 NAT forwarding。

## MTU 与透明代理

- 初始后方接口 MTU 1500 时，上行 TCP 大数据回显停顿。测试接口改为 1440 后，
  TCP 协商 MSS 1400，完成 64 KiB 回显。此结果不代表原 1500 MTU 路径的大报文已解决。
- DN 主机的 `OUTLINE_PROXY` 会把测试网段的 TCP 重定向到本地 12345 端口。
  下行 TCP 曾能建立连接却没有 SYN 到达 UE；只为 `198.18.252.0/24` 添加临时
  `RETURN` 例外后，下行 TCP 及 UE 隧道抓包通过。业务示例 `192.168.0.0/16`
  已在现有代理直连列表中，无需照搬该测试例外。

## 清理与后续 TUN 验证

删除临时 VPP `/24`、`/32` 路由、单地址 NAT identity mapping、DN 路由及代理例外，
关闭测试回显进程，删除 UE veth 和隔离命名空间，恢复 UE 原转发状态。
删除用户临时后路由后重新建立原会话；非 security 字段与测试前一致。
鉴权计数由正常重连更新，不回写旧值。

当前核心网仍为原 VPP/memif 部署。全局切换 TUN 会中断现有 gNB/UE，自动审批
拒绝了该操作，需要用户明确批准停机范围后再执行；不能把代码检查记为 TUN 实测通过。
已备份 Helm revision 165 的原版 Chart、参数及清单，并准备模式切换与恢复参数，
保留原核心网镜像和已上线的 WebUI 镜像。

日志、TCP/UDP 校验结果、UE pcap 和原配置位于本机
`/tmp/framed-routes-live-20261014/`。该目录含部署配置，仅保存在本机，未提交原始配置到 Git。
