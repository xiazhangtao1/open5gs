# 2026-10-25 现场 VPP 丢包与尾延迟定位

## 结论与边界

同一 VPP 进程、相同 CPUSet 和相同六终端流量的正反对照，将本轮主要性能
问题定位到 **VPP 容器的 CFS 带宽节流**。暂停后的突发下行使 OAI gNB 的
GTP-U UDP 接收队列溢出。扩大基站接收缓冲区可以消除本轮丢包，但保留
数百毫秒时延；取消 VPP 自身时间配额，同时保留实际 CPUSet 约束后，两者
均明显改善。旧版核心网也复现，不能归因于第二阶段业务代码。

现场内核为 `5.15.148-rt74-rc202408`、PREEMPT_RT、cgroup v1，启动参数包含
隔离 CPU 和 `nohz_full`。已证明节流与问题的因果关系；**没有定位到具体内核
缺陷，也没有证明所有 RT / nohz 平台都会出现**。配额从 3 核临时提高到 4 核
仍发生节流，不以平均 CPU 占用推断调度延迟。修复后的少量孤立长样本仍需
如实记录，短时结果不代表长期或满速保证。

## 流量、测量与单变量对照

原六个 SUPI 的尾号为 100–105；上行分别为 0.5/1/1.5/2/2.5/3 Mbps，下行
为 1/2/3/4/5/6 Mbps，UDP 长度 1200 字节。使用已隔离到 `/tmp` 的 iperf3
3.9；此前 3.16 的高 CPU 现象及替换结果见核心网控制文档。此次所有对照
使用相同客户端、服务端和速率，基站与终端不在对照之间重启。

六终端各发送 100 次 ping，间隔 0.2 秒，目标 `10.2.0.119`，绑定
`oaitun_ue1`。P99 按实际收到的全部回复合并计算，丢包单独统计。

| 条件 | 回复 / 600 | 合并 P99 / 最大 RTT（ms） | 约 30 秒内 VPP 新增节流次数 | gNB 套接字新增丢弃 |
| --- | ---: | ---: | ---: | ---: |
| 原时间配额，默认 gNB 接收缓冲区 | 529 | 292 / 394 | — | 持续增加 |
| 仅取消 VPP 自身配额 | 600 | 30.5 / 38.9 | 0 | 停止增加 |
| 恢复原配额复现 | 504 | 270 / 420 | 148 | 5100 |
| 仅将 VPP 配额临时提高到 4 核 | 560 | 152 / 327 | 108 | 持续增加 |
| 原配额，gNB 接收缓冲区提高到 8 MiB（内核报告值） | 600 | 325 / 426 | 146 | 0 |
| 恢复 gNB 默认缓冲区，仅取消 VPP 自身配额 | 600 | 28.9 / 56.7 | 0 | 0 |

原 gNB `10.2.0.119:2152` 的 `SO_RCVBUF` 为 212992 字节。表中缓冲实验
临时对现有套接字调用 `SO_RCVBUFFORCE`，请求 4 MiB，Linux 报告 8 MiB；
实验结束已恢复 212992 字节，未将大缓冲作为最终修复。

抓包在主机 `ens5f0` 限时进行，诊断任务固定 CPU 36、37；捕获头部后按
ICMP 载荷中的发送时间戳关联 NAT 前后四个观测点：

1. A：gNB 发往 UPF 的 N3 GTP-U 内层 Echo Request。
2. B：核心网经 N6 / NAT 发往主机的 Echo Request。
3. C：主机返回核心网的 Echo Reply。
4. D：核心网返回 gNB 的 N3 内层 Echo Reply。

原配额复现时，A→B 的 P99 为 280.915 ms，主机 B→C 的 P99 为 0.035 ms。
取消配额的重复轮次中，600 组 A/B/C/D 完整，A→D 的 P99 为 0.2201 ms，
最大为 40.7052 ms；终端完整 RTT 的最大值为 56.7 ms。抓包程序报告内核
丢包为 0。A→D 包含 VPP、UPF、物理 / 主机网络和主机回复，不将其称为
UPF 单独处理时间。UPF worker、memif 收发统计未见新增转发丢弃；
UPF、gNB 及 5GC 父 cgroup 未见新增 CFS 节流。没有据此排除所有其他潜在故障。

## 持久化配置

Helm `vpp.cpu.disableCfsQuota` 默认关闭，现场经上述对照后显式开启。
VPP 启动入口先等待真实内核 cpuset 与分配的 CPU 清单完全一致，再解析
自身 cgroup 的叶子路径，写入自身 `cpu.cfs_quota_us=-1`（v2 则保留周期并
写入 `cpu.max=max ...`），并回读确认。ISOCPU 提示不能代替实际 cpuset。
节点范围的 cpuset、错误 CPU 清单、写失败或回读失败均阻止 VPP 启动。

保留三个逻辑 CPU 的分配、`SCHED_OTHER`、Kubernetes requests/limits 和父组
总配额；未增加容器权限或挂载。重新创建 Pod 时自动应用。资源调整后应检查
节流计数；运行时或上层控制器重新施加时间配额仍可能影响结果。

```bash
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  cat /run/vpp/affinity-plan.json
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- sh -ec '
  cat /sys/fs/cgroup/cpuset/cpuset.cpus
  cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us
  cat /sys/fs/cgroup/cpu/cpu.stat
'
kubectl exec deploy/gnb-oai-gnb -c gnb -- sh -ec '
  ss -uanmp | sed -n "/10.2.0.119:2152/,+1p"
'
```

以上为当前 cgroup v1 现场命令。需要对 `cpu.stat` 和套接字 `d` 计数取前后
差值，不能将历史累计值视作新故障。恢复正常 Kubernetes 时间配额可设置
`vpp.cpu.disableCfsQuota=false` 并完成 Helm rollout；现场已证明原配额会
复现性能问题，故当前保持开启。

## 持久化部署后的最终复测

revision 196 的新 Pod 和重新接入的六终端保留原双向十二路流量。一次长测
因外层诊断程序默认 80 秒超时提前结束，未记为通过；停止该轮专用 ping，
修正诊断超时后重新完成约 215 秒抓包和每 UE 1000 次探测：

- **6000/6000** 回复，合并 RTT 中位数 **13.3 ms**、P99 **30.2 ms**、
  最大 **101 ms**。四个观测点 A/B/C/D 均匹配 6000 次；A→D 的 P99 为
  **0.2232 ms**、最大 **81.6438 ms**。抓包报告内核丢包为 0。
- 同期 VPP `nr_throttled` / `throttled_time` 均无新增，UPF worker 报告
  drops=0。gNB 的默认 212992 字节接收队列仍有 **29** 次新增丢弃，
  计数从 13 到 42。UE101 / UE102 的下行接收统计分别丢 9 / 20 个 UDP
  包，与队列增量一致；其余四 UE 为 0。临近窗口的每 UE 最后 24 个
  10 秒接收区间合计 29/525001，约 **0.0055%**；上行服务端同规模区间
  0/262502。接收区间与抓包边界不完全相同，不混用其分母。
- 残余长样本后的 N3 下行突发最高 **187 包/ms**，例如
  `00:42:45.512 UTC`；少数 A→B / C→D 停顿仍发生在核心网路径内。
  这些结果只证明主要问题明显改善，**未消除全部 UDP 丢包或尾延迟**。
  已确认残余丢包位置为基站接收队列；残余停顿的具体线程 / 内核机制
  尚未确认，不能进一步武断归因于 UPF、VPP 或无线侧。
- 在完整重跑期间叠加 50 个浏览者读取共享缓存 60 秒，共 **1500 次请求、
  0 错误**，Web P95 **17.13 ms**、P99 **24.94 ms**。网页响应统计与
  用户面 RTT 分开记录。另一次相同浏览压测也是 1500 次、0 错误。
- Helm / CPUSet / cgroup 测试 **31 项通过**，包括真实 cpuset 未收敛、
  CPU 数相同但清单错误、cgroup v1/v2 叶子定位、写入失败和回读失败。
  Helm lint 和现场参数渲染通过；v1 实际启动写入及重建 Pod 已验证，
  v2 的验证为单元测试。Playwright 现场检查六终端、DNN、IP、速率、
  默认 QoS 参数与管理入口，控制台无脚本错误。

进一步定向内核调度采样未执行成功：初次 tracefs 挂载被环境拒绝；需要
扩大临时诊断容器内核权限的重试被自动审批拒绝，未绕过审批。没有启用
全局跟踪，也不把未采集到的调度证据当作结论。六终端、灌包服务和最新
核心网版本继续运行，诊断容器已退出。

## 第二阶段现场兼容性检查

真实 UE105 修改 5QI / ARP / Session-AMBR 时，Wireshark 独立确认 PFCP
Session Modification Response 的 Cause=1，以及 NGAP
PDUSessionResourceModifyResponse。终端日志记录收到 36 字节的
`FGS_PDU_SESSION_MODIFICATION_COMMAND`，随后打印 `unknown message type 203`。
当前 OAI UE 的 `nr_nas_msg.c` 收包分支没有该消息的处理，未回送完成消息。
核心网重传后正确进入 `unknown`，阶段为 PFCP / NGAP confirmed、NAS unknown，
原确认配置与目标配置分开保留；不能宣称这条现场 NAS 修改流程完成。

恢复仅针对 UE105。无线断开后调用单用户 OAM 释放返回 500，未记为释放成功；
随后重连并核对新会话实例、IP `10.45.0.105`、5QI=9、ARP=8、上下行
1 Gbps 原参数。该 OAM 失败单独保留，未采用批量释放或清理其他五个终端。
只重建 UE105 两个 iperf 服务端会话及客户端；其余五个终端流量继续运行。
OAI UE 的 NAS 支持与 gNB 的 Session-AMBR 执行能力仍是现场限制，不能用
隔离协议模拟终端的成功结果替代。

另测 ARP-only 的 N2-only 修改：当前 OAI gNB 将 N2 Transfer 的解码放在
`nAS_PDU != NULL` 分支内，无 NAS 时打印 `received pdu session modify with
void content`，随后回复修改 0 个 bearer。核心网要求目标 QFI 的确认，正确
进入 unknown，未把该空回复算作成功。该轮也仅重连 UE105，并再次核对新
实例和原参数。这里没有修改 OAI 代码；其 NGAP 解码和 UE NAS 能力需要另外
完成兼容性工作，才能在当前六终端演示环境验收全部第二阶段在线修改。
