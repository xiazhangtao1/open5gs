# 可选 5QI 调度优先级实现与验证

验证日期：2026-10-08。核心网已部署本次实现；下文分别记录 TUN 初验和
随后完成的 VPP/memif 验收，并区分通过项与环境限制。

## API 行为

专用承载 POST/PATCH 的 `qos.priorityLevel` 是可选 JSON 整数，范围为
`1..127`，数值越小优先级越高。它独立于准入/抢占参数
`qos.arp.priorityLevel`（`1..15`）。例如：

```json
"qos": {
  "5qi": 2,
  "priorityLevel": 20,
  "arp": { "priorityLevel": 8 },
  "maxbrDl": "10 Mbps", "maxbrUl": "10 Mbps",
  "gbrDl": "5 Mbps", "gbrUl": "5 Mbps"
}
```

完整 curl 示例见 [Helm README](../helm/xcn/README.md)。

- 创建时省略新参数：不覆盖 5QI 默认调度优先级。
- PATCH 仍要求完整的 `flowDescriptions` 和 `qos`；仅改变新参数也触发
  同一 QoS Flow 的 N2 修改，保持 `appSessionId`、QFI 和既有 precedence。
- PATCH 省略新参数：取消已配置的覆盖值。
- 查询仅在显式配置时返回 `pccRules[].qos.priorityLevel`。
- `0`、`128`、负数、小数、`null`、布尔、字符串、对象、数组、超大数
  均返回 HTTP 400；失败的修改保留原规则。

传递路径为 PCF API → PCC 内部 QoS → SBI `QosData.priorityLevel` → SMF
QoS Flow → NGAP `NonDynamic5QIDescriptor.priorityLevelQos`。省略配置时不编码
该 NGAP 可选字段，RAN 使用标准/预配置的 5QI 默认值。ARP 始终独立编码。
H/V-SMF 间的 `QosFlowProfile.nonDynamic5Qi.priorityLevel` 也保留此值，并在
入站检查范围。使用已有 ASN.1/OpenAPI 模型及其析构路径管理分配的对象。

## 构建与自动验证

测试使用带完整依赖的 Docker 构建环境。生产进程与测试程序均由修改后的
源码构建；没有用 HTTP mock 代替核心网。

| 验证 | 结果与范围 |
| --- | --- |
| 完整 Ninja 构建 | 通过，包含依赖、所有核心 NF 和测试程序 |
| Meson `unit` suite | core、crypt、unit 三套均通过 |
| SBI QoSData 序列化/反序列化 | 覆盖未配置、1、20、127，确认 ARP 8 不受影响 |
| NGAP 编码后独立解码 | 同样覆盖 0/1/20/127；常规与 HR V-SMF 修改传输路径均通过 |
| ASan/UBSan | `sbi-message-test`、`qos-priority-test` 通过，无检测器错误 |
| 既有 VoNR 回归 | `qos-flow-test`、`session-test`、`simple-test`、`video-test` 通过 |
| 隔离真实核心压力测试 | 100 次顺序生命周期、100 轮双会话并发生命周期通过，共 300 次承载周期 |
| 实际部署最终复验 | 同样的 300 次承载周期全部通过；并发测试的断言计数只在主线程更新 |

新增 `tests/vonr/dedicated-bearer-test.c` 使用可控 NGAP/NAS 对端连接真实
AMF/SMF/PCF/UPF。每个周期执行创建、仅修改调度优先级、取消覆盖、删除，
并应答 NGAP 和 NAS。测试独立解码下发的调度优先级、ARP、5QI，查询规则
及 precedence，检查删除后只剩默认流，并发送 GTP-U ICMP 探测、等待回包。
还覆盖 access release/service request 后带覆盖值和无覆盖值的恢复。

并发测试在同一 UE 的两个 PDU 会话上同时发送 HTTP 请求，使用不同的
优先级验证隔离，并按实际收到的 PDU session ID 处理 N2 通知。
非法值测试检查创建无残留，修改后原配置仍可查询。

单元检查复现命令（已完成 Meson 配置及依赖安装时）：

```bash
ninja -C build
meson test -C build --suite unit --print-errorlogs
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  build-san/tests/unit/unit sbi-message-test qos-priority-test
```

ASan 检查关闭了泄漏检测；此结果不等于完整核心网的泄漏检测结论。
`build-san` 使用 `-Db_sanitize=address,undefined`。

隔离运行需要独立 MongoDB、测试配置中的地址和 UPF TUN，避免测试辅助函数
替换已有订阅。新增测试在插入前检查 IMSI 不存在，结束时删除自身订阅。
在匹配 `configs/vonr.yaml` 的隔离环境可执行：

```bash
XCN_QOS_CYCLES=100 build/tests/vonr/vonr \
  -q -c build/configs/vonr.yaml dedicated-bearer-test
build/tests/vonr/vonr -c build/configs/vonr.yaml \
  qos-flow-test session-test simple-test video-test
```

实际部署复验通过 NodePort `10.2.0.119:30777`，使用临时 IMSI
`460110000009050`、PDU 会话 5/6 和临时 GTP-U 地址 `192.0.2.5`。
测试配置禁用本地核心 NF，连接部署的 AMF 和 MongoDB，配置 PLMN 460/11
及 SD `010101`。运行命令如下，配置文件和测试二进制为本次验证产物：

```bash
XCN_QOS_PCF_URL=http://10.2.0.119:30777 \
XCN_QOS_TEST_MSIN=0000000905 XCN_QOS_CYCLES=100 \
XCN_FORWARDING_GNB1_ADDR=192.0.2.5 \
  build/tests/vonr/vonr -q \
  -c /tmp/xcn-qos-results/qos-live.yaml dedicated-bearer-test
```

## 部署、环境与证据

`xcn` Helm release 升级为 revision 151，使用镜像：

```text
localhost:5000/xcn-runtime:qos-priority-1008
sha256:fd318a23c4d3b5cacbfce5a683e21e621c157c5e43768dd39bc8649e3161b37e
```

实际压力测试后临时订阅查询计数为 0，SMF 临时 UE/会话已释放。
重启辅助 OAI gNB/UE 后恢复了原 UE `460110000000100`，分配地址
`10.45.0.3`；通过 `oaitun_ue1` 分别 ping `10.45.0.1` 和 `8.8.8.8`，
各 3/3 成功、0% 丢包。测试不以 OAI 的调度实现作为核心网字段正确性的依据。
随后持续外网 ping 240 次，240/240 成功，0% 丢包。

压力结束及 OAI 重连期间采集了 600 秒资源数据，每 30 秒采样一次，共 21 个
样本。各容器均无新增重启，主要 NF 的文件描述符数量全程不变：

| NF | RSS 范围（KiB） | 文件描述符 |
| --- | --- | --- |
| AMF | 128436–128700 | 36 |
| SMF | 164796–165056 | 29 |
| PCF | 32352–32352 | 25 |
| UPF | 133088–133200 | 11 |

PCF RSS 全程不变；SMF 在 OAI 重新建立会话后保持 165056 KiB。
UPF RSS 小幅增加 112 KiB，因此不据此声称整个核心网内存完全不增长。
这次短期观察没有发现 PCF/SMF 持续资源增长，不是长期泄漏排除证明。
临时验证容器及主机 `192.0.2.5/32` 地址均已清理，保留部署的新核心网镜像。

运行日志、原始 N2 抓包及资源采样保存在本机
`/tmp/xcn-qos-results/`，未作为大文件提交：

- `unit-final.log`、`unit-hr.log`、`sanitizer.log`；
- `vonr-selected.log`、`lifecycle-stress.log`、`live-final.log`；
- `qos-live.pcap`；
- `resource-observation.json`、`resource-observation.log`；
- `oai-ping-observation.log`、`deployed-{amf,smf,pcf,upf}.log`；
- `af-baseline.log`、`deploy.log`、`runtime-push.log`。

## VPP/memif 实际部署验收

随后将当前 Kubernetes `xcn` 切换为真实双 VF 的 VPP/memif 模式。
最终 Helm revision 为 154，核心网仍使用前文的正式
`qos-priority-1008` 镜像及 digest；临时诊断镜像已撤销，没有保留诊断代码。

```text
gNB / 受控 NGAP、GTP-U 对端
  → SR-IOV/DPDK dpdk-n3（10.2.0.231/20，FIB 10）
  → N3 memif2/0 → UPF（N3 10.2.0.230）
  → N6 memif1/0 → VPP NAT44 → dpdk-n6（10.2.0.232/20）
  → 10.2.7.254 → 外网
```

VPP 为 26.06，实际使用两张 iAVF VF。UPF 分配 5 个 CPU，自动解析为
2 个 Session Worker；VPP 分配 3 个 CPU，实际启动主线程和 2 个 Worker。
两条 IP 模式 memif 都连接成功，每条有 2 个 RX/TX 队列，ring size 8192。
验证确认两个 UPF Worker、两个 memif 队列均有实际数据，未以开启配置代替运行。

本次排查并修正了两个部署配置问题：

1. 原 N3/N6 地址收到其他 MAC 的 ARP 应答。经过 ARP 探测后改用上述
   `10.2.0.230/231/232`；受控 gNB 临时使用 `10.2.0.233`。
2. 已有订阅配置了静态 UE 地址 `10.45.0.2`、`10.45.0.3`，原动态池包含
   这些地址。SMF 日志曾同时向 OAI 和测试 UE 分配 `.3`，测试会话释放后影响
   OAI 下行地址查询。新增可选 `networking.smf.ipv4PoolRanges`，最终配置为
   `10.45.1.2-10.45.255.254`，避开静态订阅。空列表保持原动态池行为，
   此参数同时适用于 TUN 和 memif；没有修改用户订阅。

最终部署关键参数如下；其余参数保留本次部署原有值：

```yaml
networking:
  smf:
    ipv4PoolRanges: ["10.45.1.2-10.45.255.254"]
  upf:
    mode: memif
    n3:
      address: 10.2.0.230
      hostInterface: ""
resources:
  fivegc:
    upf:
      requests: {cpu: 5}
      limits: {cpu: 5}
vpp:
  resources:
    requests: {cpu: 3}
    limits: {cpu: 3}
  n3:
    interfaceAddress: 10.2.0.231/20
    defaultGateway: 10.2.7.254
  n6:
    externalAddress: 10.2.0.232/20
    defaultGateway: 10.2.7.254
    nat44: {enabled: true}
```

最终受控对端测试使用正式核心网镜像，配置 `XCN_QOS_CYCLES=100` 和
`XCN_QOS_PING_IP=8.8.8.8`。100 次顺序生命周期、100 轮双会话并发生命周期
全部通过，合计 300 次承载周期。覆盖优先级边界、非法值、仅修改优先级、取消
覆盖、恢复 access、查询、删除及清理。每次数据探测校验 GTP-U TEID、QFI、
ICMP Echo Reply 和内层源/目的地址，经过真实 N3/N6 memif、DPDK N6 和 NAT。
测试 UE 使用 `10.45.1.6/7/8`，OAI 保持静态 `.3`，同时运行期间没有再冲突。

测试仅在每个会话首次探测时允许最多 3 次、各 1 秒的重试，以处理 ARP 首包
丢弃；后续探测保持单次发送、15 秒超时。外网目标可通过 `XCN_QOS_PING_IP`
配置，默认仍为 `10.45.0.1`。复验命令：

```bash
XCN_QOS_PCF_URL=http://10.2.0.119:30777 \
XCN_QOS_TEST_MSIN=0000000905 XCN_QOS_CYCLES=100 \
XCN_QOS_PING_IP=8.8.8.8 XCN_FORWARDING_GNB1_ADDR=10.2.0.233 \
  build/tests/vonr/vonr -q \
  -c /tmp/xcn-qos-memif-results/qos-live.yaml dedicated-bearer-test
```

配置修正前还完成了 1000 次顺序和 1000 轮双会话并发，共 3000 次承载周期。
这些协议测试通过且没有核心网崩溃，但当时共享地址池冲突尚未排除，因此不把
这轮结果记为整个 OAI 共存环境的完整验收。最终验收使用修正配置及加强回包
断言后的 `accepted-final-stress.log`。

Helm lint、9 项 CPU 分配/入口回归通过；默认渲染确认未配置 ranges 时不生成
`range`，实际 SMF 配置确认应用了上述范围。临时测试订阅最终计数为 0，SMF
会话数恢复为 1，仅保留原 OAI 会话。

最终部署连续观察 600 秒，每 30 秒采样一次，共 21 个样本；期间所有容器
保持就绪、没有新增重启。资源数据如下（观察开始包含最终压力测试）：

| NF | RSS 范围（KiB） | 文件描述符范围 |
| --- | --- | --- |
| AMF | 82192–99336 | 36–37 |
| SMF | 147596–157148 | 29–29 |
| PCF | 23012–27520 | 25–25 |
| UPF | 179116–196088 | 30–30 |
| VPP | 1518772–1533396 | 53–53 |

压力完成后，AMF/SMF/PCF RSS 分别保持在 99336/157148/27520 KiB。
UPF/VPP 在持续数据流量下仍有小幅 RSS 增长，未将此结果解释为全库内存完全
不增长或长期泄漏排除证明。两个 UPF Worker 的 drops、queue-full、push-fail
均为 0，N3/N6 两组队列的 RX/refill 错误、dispatch-drop 和 TX drop 均为 0。
当前 Pod 的核心 NF/VPP 日志没有 FATAL 或断言。

OAI UE 同时持续外网 ping 600 次，599 次成功；逐条核对日志确认只缺失首次
探测，后续序号 2–600 全部成功。该首包结果单独记录，没有记作 0% 丢包。
同时到核心网网关 `10.45.0.1` 的持续 ping 为 480/480 成功、0% 丢包。
临时验证容器、gNB 地址 `10.2.0.233` 及其 VPP 静态邻居项均已清理。
当前保留上述 memif 部署与互不重叠的地址池配置。

本机证据目录为 `/tmp/xcn-qos-memif-results/`：`accepted-values.yaml`、
`accepted-pods.json`、`accepted-final-stress.log`、`memif-final.txt`、
`vpp-threads-final.txt`、`final-observation.json`、`oai-accepted-ping.log`、
`oai-accepted-gateway.log` 和 N3/N6 抓包。初始失败及诊断日志保留，未记为通过。

自动审批拒绝了具有主机 PID、读写主机 `/proc` 及解除安全限制的调试命令，
没有执行该命令。随后采用临时 UPF 收包日志定位；诊断实例已替换回正式镜像。

## 已知限制

完整 VoNR suite 中的 `af-test` 等待超时。修改前的
`xcn-forwarding-validation:1008` 镜像在独立环境运行同一测试也等待超时
（120 秒超时退出 124），因此此项未记为通过。其余上述四套回归均完成。

当前在线部署未运行 BSF，SCP 在会话建立时记录无法发现
`nbsf-management`，PCF 记录 BSF 注册返回 504 后按既有回退继续建立策略。
此次测试验证专用承载策略通知成功及会话/数据传输完成，未修改 BSF 配置。

恢复 OAI UE 后，UPF 还记录了 IPv4 会话上的 IPv6 背景报文丢弃；该会话未
分配 IPv6 地址，现有接收路径拒绝此类报文。IPv4 网关及外网连通性验证通过。
没有将这些环境日志计为“无错误日志”，也没有扩大本次修改去放宽地址检查。

HR 路径目前验证了字段转换和 NGAP 编码，未部署实际 H/V-SMF 漫游网络。
本次验证证明核心网传递及生命周期在上述测试范围内正确，不证明任何具体
gNB 调度器对覆盖值的执行效果，也不替代长期负载验证。
