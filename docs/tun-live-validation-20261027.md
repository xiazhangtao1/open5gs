# 2026-10-27 TUN 模式现场回归

## 结论与范围

使用现场最新核心网镜像，将同一套六终端环境从 VPP/memif 临时切换为
Linux UDP/TUN，完成真实转发、专载控制、单会话释放、默认 QoS /
Session-AMBR 修改及只读网页并发回归。正式负载窗口内，六终端共
**18,000 次 ping，18,000 次回复**，合并 P99 **30.5 ms**，最大 **45.0 ms**。
本轮未发现 TUN 转发错误、网元重启或新增 OOM；相关协议断言通过。

这是当前 IPv4、OAI RFsim 和合计约 31.5 Mbps UDP 业务负载下的验证，
不是线速、物理无线、IPv6 或长期稳定性保证。测试客户端仍有既有 NAS
结构体对齐的 UBSan 告警，不能称为 sanitizer 全部通过。现场 OAI UE 的
NAS 在线修改能力限制仍然存在，受控协议对端的成功不代表该限制已解决。

## 环境与模式隔离

| 项目 | 配置 |
| --- | --- |
| 开始版本 | Helm revision 202，Git `1405606d1` |
| 核心网 runtime | `localhost:5000/xcn-runtime:rename-xcn-1026` |
| WebUI | `localhost:5000/xcn-webui:user-flow-1027` |
| 原用户面 | VPP 26.06 / memif，`disableCfsQuota: true` |
| 临时 TUN 部署 | Helm revision 203，5GC Pod 10/10，无 VPP 容器 |
| TUN N3 | `ens5f0` 临时增加 `10.2.0.230/32`，UPF UDP 2152 |
| TUN N6 | `ogstun`，`10.45.0.1/16`，UP / LOWER_UP，MTU 1500 |
| UPF CPU | CPUSet `44-46,116-117`，配额 `500000/100000` µs |
| 诊断 CPU | 36、37；测试和离线解码限制 CPU、内存 |
| 原业务终端 | IMSI `460110000000100` 至 `460110000000105`，均为 PSI 1、DNN `cmnet` |
| 原业务 IP | `10.45.0.3`、`10.45.0.101` 至 `10.45.0.105` |

切换前保存完整 Helm values、manifest、镜像、RAN 副本数、接口／路由／
iptables 及流量配置。仅改变 `networking.upf.mode=tun` 和
`networking.upf.n3.hostInterface=ens5f0`，保留原 NF 镜像。切换时暂停原六个
测试 UE 和 gNB，避免残留无线、会话及 N3 状态干扰对照，然后逐个重新接入。

业务使用原专用 `xcn-six-ue-traffic-server`，客户端为 iperf 3.9，UDP 载荷
1200 字节，绑定各 UE 业务 IP；路由核对实际经过 `oaitun_ue1`。每个终端
分别有一条上行和一条下行流量：

| UE 后缀 | 上行目标 Mbps | 下行目标 Mbps |
| --- | ---: | ---: |
| 100 | 0.5 | 1 |
| 101 | 1 | 2 |
| 102 | 1.5 | 3 |
| 103 | 2 | 4 |
| 104 | 2.5 | 5 |
| 105 | 3 | 6 |

客户端重连后，旧 iperf 服务端连接曾导致 `server is busy`。仅检查客户端
进程不足以证明负载存在；已重启原专用流量容器清理旧连接，并用 UPF 实际
速率确认十二路均恢复。此前没有实际灌包的首轮十分钟 ping 和网页测试仅
算轻载，不计入下列正式负载结论。

## 持续转发与数据正确性

正式负载窗口为约 **602.3 秒**。每个 UE 每 0.2 秒发送一个 ping，各
3000 次；并行每约 20 秒检查六个用户的 UPF 上下行速率，每一路必须达到
配置目标的 70% 以上。29 个资源／速率采样全部满足条件，无观测错误。
UPF 统计包含协议开销，与 iperf 应用载荷速率口径不同。

| UE 后缀 | 回复 / 发送 | P50 ms | P99 ms | 最大 ms |
| --- | ---: | ---: | ---: | ---: |
| 100 | 3000 / 3000 | 13.0 | 30.2 | 34.7 |
| 101 | 3000 / 3000 | 13.8 | 31.2 | 40.6 |
| 102 | 3000 / 3000 | 13.3 | 31.0 | 40.6 |
| 103 | 3000 / 3000 | 13.1 | 30.2 | 37.3 |
| 104 | 3000 / 3000 | 13.1 | 30.5 | 39.1 |
| 105 | 3000 / 3000 | 13.2 | 30.2 | 45.0 |

正式负载下额外验证：

- 六 UE 各发送 32、512、1200、1472 字节 ICMP payload 的 DF ping，每种
  20 次，合计 **480/480**。1472 字节 payload 对应 1500 字节 IPv4 报文。
- UE105 与 DN 双向 UDP 回显，64、512、1200、1400 字节各 100 次，
  合计 **800 次逐字节一致**。
- UE105 与 DN 双向 TCP，各传输 64 KiB 和 1 MiB；四次内容、长度及
  SHA-256 校验均一致。双向端点显式绑定业务地址。
- 收集到的六路下行 iperf 接收日志各有 68 个十秒区间，合计
  **1,487,570 个 datagram，报告丢失 0**。这些约 680 秒的客户端区间包含
  正式 ping 窗口前的准备时间，不能与 602.3 秒窗口完全等同；也不能据此
  宣称上行接收端逐包零丢失。

## 控制协议与规则核验

使用已有 `tests/vonr/dedicated-bearer-test.c`，所有本地 NF 启动均关闭，
测试 NGAP/NAS/GTP-U 对端连接现场真实 AMF、SMF、PCF、UPF。三组串行执行，
同时保留原六 UE 灌包；没有在原六个用户上触发释放或改签约。

| 回归 | 正式负载结果 | 核验内容 |
| --- | --- | --- |
| 专载 | 退出 0，55.7 秒 | 100 次单会话及 100 轮双会话，共 300 个专载生命周期；建立、修改、删除、优先级／ARP、非法参数、QFI 和 GTP-U 隔离 |
| 第一阶段会话控制 | 退出 0，37.4 秒 | 单 PDU 会话释放，NAS／N2 两种完成顺序，缺少／陈旧版本、PSI 重用、丢完成回复后的清理及其他会话继续转发 |
| 第二阶段默认承载控制 | 退出 0，117.0 秒 | 60 轮交替默认 QoS／Session-AMBR 更新；两种完成顺序、专载共存、并发冲突、对端拒绝、NAS 重传、丢回复进入 unknown、显式释放和 PSI 重用 |

测试订户为 `460110000090270`、`460110000090271`、`460110000090272`。
插入前确认不存在；正常结束后删除，最终实际 `xcn` 数据库匹配数量为 0。
最终 UPF 仅保留原六用户、六会话。

PFCP 在 loopback 上抓取 300 秒，共 **4548 个报文，抓包内核丢弃 0**。
用现有 trace-decoder 镜像内 Wireshark 离线独立解析，容器无网络、只读
挂载抓包。捕获到的会话响应如下，均为 **Cause=1 / Request accepted**：

| PFCP 响应 | 数量 |
| --- | ---: |
| Session Establishment Response | 12 |
| Session Modification Response | 2189 |
| Session Deletion Response | 13 |
| 合计 | 2214 |

捕获的 QER 包含 QFI、上下行 Gate、UL/DL MBR；MBR 编码值包括 10000、
20000 和 1000000 kbit/s，覆盖测试的 10/20 Mbps 及原 1 Gbps 配置。
抓包窗口没有覆盖三组测试的所有起始报文，表中数量不能用作全部生命周期
数量；PFCP 接受也不单独证明 NAS／NGAP 事务已完成，仍以各协议断言和
实际转发共同判断。

当前 OAI UE 未实现 NAS PDU Session Modification Command 的完成处理，
gNB 的无 NAS N2-only 修改也有限制，详见
[此前现场兼容性记录](vpp-cfs-latency-validation.md#第二阶段现场兼容性检查)。
本轮没有修改 OAI，不能把受控对端的通过结果当作六个 OAI UE 在线修改
默认承载已全部验收。

## 资源、CFS 与网页负载

29 个采样内，AMF、SMF、PCF、UPF 的叶子 cgroup 新增 CFS 节流和 OOM
均为 **0**。SMF 在正式窗口前已有一次约 51 ms 的节流，正式窗口没有继续
增长。TUN 的事件驱动 UPF 保留 5 CPU 配额，本轮未观察到此前 VPP 配额下
的数百毫秒停顿；这不等于已证明所有内核、负载和配额组合均无问题。
`vpp.cpu.disableCfsQuota` 只作用于 VPP；TUN 没有 VPP 容器，本轮并未
取消 TUN UPF 的 CPU 配额。

| 网元 | 窗口内 RSS MiB 范围 | 最后约三分钟 RSS MiB | FD 范围 | 线程数 |
| --- | ---: | ---: | ---: | ---: |
| AMF | 206.91–260.01 | 260.01 | 37–38 | 2 |
| SMF | 238.18–244.38 | 243.79 | 31 | 2 |
| PCF | 32.55–37.32 | 37.32 | 26–27 | 2 |
| UPF | 139.26–141.32 | 141.32 | 11 | 4 |

控制压力阶段内存有所上升，随后采样趋于稳定；短窗口只能说明未观察到
持续增长，不能排除长周期泄漏。回切前 TUN Pod 运行约四小时，零重启、
六路上下行实际速率正常；该间隔没有持续 ping 记录，不计作四小时零丢包。

50 个模拟浏览者持续约 120.15 秒读取现有监控缓存，**3204 请求、0 错误**，
P95 **24.03 ms**，P99 **59.16 ms**。UPF 速率、AMF／SMF 信令源采样从约
0.4996 次/秒变为 0.4994 次/秒；AMF UE、SMF PDU 保持约 0.0999 次/秒。
没有随浏览人数增加核心网查询频率。结束时监控实际内存约 **103.43 MiB /
512 MiB**，没有内存暂停、回收或采集错误。这里只压测只读 API 请求，
没有启动 50 个浏览器渲染进程。

## 告警与不计入通过的运行

第一阶段和第二阶段客户端保留 ASan 地址检查，关闭泄漏检测后退出 0，
各有三处 `lib/nas/5gs/ies.c` 紧凑 NAS 结构体对齐 UBSan 告警；没有报告
`ERROR: AddressSanitizer`。生产 NF 是原发布镜像，本轮没有使用带 sanitizer
的 NF 替代它们，不能将客户端检查扩大为生产网元内存安全结论。

此前一次开启泄漏检测的客户端在协议打印 SUCCESS 后，退出阶段反复出现
`AddressSanitizer:DEADLYSIGNAL` 并挂起，已结束该临时测试容器，不算一次
干净的 ASan／泄漏检查。相关告警与此前
[第二阶段 sanitizer 记录](webui-core-control.md) 一并保留，尚未修复。

准备阶段还纠正了旧测试配置使用 `open5gs` 数据库、临时 MSIN 的 BCD
编码，以及两个客户端同时占用 AF 监听地址的问题；这些失败运行未计为
协议通过。只清理本轮插入的临时记录，未覆盖原业务签约。正式三组均使用
现场 `xcn` 数据库并串行执行。

## 现场恢复与证据

**08:22:22 恢复完成，Helm revision 204，VPP/memif。** 用户提供的 Helm
values 及包含默认值的完整有效配置，分别与 revision 202 比较，均一致；
11 个核心网容器的实际镜像摘要也全部一致。Core、MongoDB、monitor、WebUI
Pod UID 未变，原六个 UE 的业务 IP、PSI、DNN 保留。

原十二路灌包经专用服务器清除旧连接后恢复，连续三次检查全部上下行实际
速率达到各自目标的 70% 以上；恢复后一分钟复测也持续检查实际速率。
六 UE 各发送 300 次 ping，共 **1800/1800**，合并 P99 **29.8 ms**，最大
**39.8 ms**，无观测错误。恢复后的短窗口与 TUN 十分钟窗口长度、CPU 分配
及并发控制压力不同，不能用两者的 P99 小差异判断哪种模式更快。

VPP 实际叶子配额恢复为 `-1/100000`，复测期间新增节流为 0；CPU Manager
重新分配的 VPP CPUSet 为 `44-45,116`，与诊断 CPU 36、37 分离。N3、N6
memif 均 connected，分别有 2 条 RX/TX 队列；VPP 外侧地址仍为
`10.2.0.231`、`10.2.0.232`。仅删除测试新增的主机 `10.2.0.230/32`，避免
本地路由截获 VPP N3 报文。`ogstun` 回到原来的 NO-CARRIER / operstate
DOWN；IPv4、IPv6 中与 UE 网段、ogstun 相关的原有规则逐条核对一致，未
清空路由或防火墙。

5GC 11/11、gNB 及六 UE 全部就绪且零重启，网页入口 HTTP 200。
临时回显服务器／进程及协议客户端均已退出，只保留原流量服务器和本地
镜像仓库；临时订户及其会话已清理。

原始配置、结果 JSON、客户端日志及抓包保存在主机私有目录
`/tmp/xcn-tun-validation-1027`，目录权限 0700；完整配置、鉴权密钥和信令
抓包不提交到 Git。主要正式证据文件为：

- `loaded-soak.summary.json`、`loaded-soak.resources.json`、`loaded-soak-ue*.log`
- `loaded-protocol.results.json`、`loaded-protocol-{priority,control,native}.log`
- `loaded-icmp-sizes.json`、`loaded-echo-{uplink,downlink}.log`
- `loaded-readers.json`、`loaded-pfcp.pcap`、`loaded-pfcp.tsv`
- `loaded-pfcp.summary.json`、`resources.tun.final.json`、`pdu.tun.final.json`
- `mode-switch.restore.log`、`check-restored.log`、`restored-vpp.summary.json`
- `restored-vpp.resources.json`、`vpp.restored.{before,after}.txt`

本轮应用和转发代码无需修改，仓库变更为本验证记录。
