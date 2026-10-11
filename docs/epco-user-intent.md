# ePCO UE 用户意图：核心网实现与联调

实现范围为 AMF、SMF、PCF 及配置和测试。UE/AP/modem 与算力服务器由其他团队实现；`tests/helpers/intent-notify-server.py` 仅为隔离测试接收端。功能默认关闭，不改变未携带目标容器请求的正常入口。尚未进行真实 UE/gNB 联调，不能据软件模拟测试宣称独立 DRB 已建立。

## 配置

SMF YAML：

```yaml
smf:
  user_intent:
    enabled: true
    mcc: "460"
    mnc: "00"
    notify_uri: http://10.2.5.125:8000/api/v1/intent/notify
```

MCC/MNC 是应用运营商标识，必须显式填写；MNC 字符串保留两位/三位区别。容器固定 `0xFF10`，部署前检查编号冲突。Helm 对应 `networking.smf.userIntent.enabled/mcc/mnc/notifyUri`，默认 `enabled: false`。双栈会话当前选择已分配 IPv4；纯 IPv6 会话尚未提供可确认的业务 IPv6 主机地址，通知会记录失败，不能将 IPv6 前缀当作 UE 主机地址。

## 消息及策略

示例完整 IE：`7b 00 0a 80 ff 10 06 64 f0 00 01 02 02`。容器内容必须为 PLMN 三字节加 `commStatus/aiModel/gpuLevel` 三字节。按容器 ID 遍历，校验长度、重复 ID、PLMN、枚举；有目标容器时 INFO 十六进制日志包括 ID、长度、PLMN 和业务字节。截断日志只打印实际可用数据。

| 输入 | 本功能专载操作 |
| --- | --- |
| On + LLM | 保留当前资源与全 IP 过滤规则，无资源时不创建 |
| On + 人脸，无专载 | 创建 5QI 2、priorityLevel 40 |
| On + 物体，无专载 | 创建 5QI 3、priorityLevel 30 |
| On + 同一实际 5QI | priorityLevel 递减，最低 1；到 1 后允许 no-op |
| On + 模型切换 | 同一 appSessionId/QFI 修改，优先级重置 40/30 |
| Off | 删除本功能专载，保留默认流；本来无专载则 no-op |

GPU 仅通知，不参与 QoS。ARP 8、NOT_PREEMPT/PREEMPTABLE；双向 MBR 500 Mbps、GBR 5 Mbps。过滤规则等效于 `permit out ip from 0.0.0.0/0 to assigned` 和 `permit in ip from assigned to 0.0.0.0/0`；后者按现有 PCC/IPFW 约定归一化存储为反向描述加 UPLINK 方向。

标准 ePCO 配置项逐项复用现有响应构造，包括 DNS；先检查 PAP/CHAP/IPCP 长度，再调用旧入口，输出受缓冲区容量限制。不回显私有意图容器。

## 事务及资源闭环

SMF 会话持有独立意图队列、PTI/请求副本、资源身份、事务 generation 和最终结果。相同活跃事务重传只重发缓存响应或关联新的 SBI stream，不重复资源操作或通知。Complete 结束的 PTI 可被新事务复用；相同三个业务字节不会抑制新事务。UE 必须按需求串行发起意图事务；队列有上限，超出上限按 NAS 资源不足拒绝。

PCF 的内部 `POST/PATCH /xcn-dedicated-bearer/v1/intent-operations` 复用 dedicated-bearer 的会话、授权、QoS 和 appSessionId 机制。POST 准备策略并返回 delta，避免另起 PTI=0 的网络修改。SMF 通过当前 UE 事务执行 PFCP、NAS、N2，最终再 PATCH commit/abort/uncertain。此入口是 SMF/PCF 内部事务适配，不是供 UE 调用的新接口。公共 dedicated-bearer PATCH/DELETE 拒绝直接修改意图拥有的资源。

流程先发送必要的 NAS Command，再等待 NAS Complete、N2 成功和最终 PFCP 确认，随后通知；不等待通知完成来驱动 NAS。no-op 也发送 N1-only Command 并等待 Complete。AMF 两个 N1-only 下发入口已经支持修改消息，不再把修改重发误认为建立拒绝并删除会话。

Command 等待使用 8 秒间隔、最多 4 次相同 NAS 重发；不重复执行资源操作。准备/最终确认等待有独立 30 秒边界。PFCP 和 HTTP 异步回调用 session ID 与 generation 排除迟到回调；会话释放清理缓存、定时器和状态。

### 当前边界与未完成事项

- **同一请求同时包含 Requested QoS Rules / Requested QoS Flow Descriptions 时，当前整包拒绝并通知 FAILURE，尚未实现标准 QoS 变更与意图策略的合并执行。** 不会先执行部分意图后静默丢弃标准字段。该限制与原需求的完整混合请求处理仍有差距，不能标记该验收项已完成。
- PFCP/N2/NAS 执行结果不确定时，不猜测资源已删除或回写 SUCCESS。保留关联和待确认状态，PCF 查询显示 `UNCERTAIN`，本会话后续意图返回 FAILURE；其他策略队列可以继续。当前没有跨 UE/RAN/UPF 的补偿恢复流程，通过正常会话释放收尾。
- NAS 层没有新增请求序号。完成后相同 PTI/相同内容按新事务处理；调用方必须遵守 NAS 生命周期，不能将已经完成的旧业务请求重新提交并期待长期去重。
- 纯 IPv6 通知地址选择、真实终端定时器差异、真实 RAN QFI/DRB 映射仍需对接验证。

## 通知

使用现有异步 SBI HTTP/2 客户端，直连 h2c prior knowledge，显式绕过代理；单请求 deadline 为 1 秒，无 TLS、无鉴权、无重试。仅 204 视为成功；400/500/200/其他状态、超时、连接失败只记通知失败，不改变已经产生的 QoS 结果。

JSON 包含会话 IPv4、SUPI、三个业务整数、`qosResult`、业务三字节小写十六进制 `rawPco`、实际发起时 Unix 毫秒时间。只有 SUCCESS + On + 已有本功能专载时包含 `fiveQi`。图片不经过本接口。

## 验证与复现

在具备本项目依赖的构建环境中：

```bash
ninja -C build
build/tests/unit/unit -q
python3 -m pip install h2
python3 tests/helpers/intent-notify-server.py --output /tmp/intent-notifications.jsonl
```

集成测试需使用隔离的 MongoDB、TUN 和 NF 配置；以 `build/configs/vonr.yaml` 为基础，启用上述 SMF 配置并将通知 URL 指向测试接收端。不要把下面的测试配置指向生产核心网。示例使用已准备的 `/tmp/intent-vonr.yaml`：

```bash
XCN_INTENT_TEST=1 XCN_INTENT_TIMER_TEST=1 XCN_INTENT_FAILURE_TEST=1 \
  build/tests/vonr/vonr -e info -c /tmp/intent-vonr.yaml dedicated-bearer-test
python3 tests/helpers/verify-intent-notifications.py \
  /tmp/intent-notifications.jsonl --resource-failure
```

不设 `XCN_INTENT_FAILURE_TEST` 时不添加最后一次资源 Command Reject，通知总数 51；设置时 52。`XCN_INTENT_TIMER_TEST` 会暂缓 Complete 以验证 Command 重发。测试检查优先级 40→1、模型互切 QFI 不变、LLM 保留、PTI 复用、重传、非法枚举、UE 拒绝、删除后的默认流 GTP-U ping，以及通知完整序列与次数。

通知 fixture 可用 `--status 400/500/200`、`--delay 1.2` 模拟失败；关闭接收端验证连接失败。原有 dedicated-bearer 回归直接运行上述测试二进制、不设置 `XCN_INTENT_TEST`。

本次隔离 Docker 实测：全量构建和 unit 通过；意图成功/拒绝生命周期及默认用户面 ping 通过；T3591 重发通过；通知 400/500/200/超时/连接失败五组全部通过，每组原 QoS/NAS 流程正常且无通知重试；原 dedicated-bearer 单会话及并发双会话回归通过。

新模块 ASan/UBSan 单测通过。LeakSanitizer 初始报告 96 字节 `talloc_enable_null_tracking` 全局分配，已在原有 qos-priority 单测复现；复测仅按该函数名抑制这一基线分配，未关闭其他泄漏检测。本结果不等于真实 UE/gNB 端到端验收。
