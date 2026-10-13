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
    notify_uri: http://10.2.0.102:8000/api/v1/intent/notify
    face_recognition:
      five_qi: 2
      priority_level: 40
      maxbr_dl: "500 Mbps"
      maxbr_ul: "500 Mbps"
      gbr_dl: "5 Mbps"
      gbr_ul: "5 Mbps"
    object_recognition:
      five_qi: 3
      priority_level: 30
      maxbr_dl: "500 Mbps"
      maxbr_ul: "500 Mbps"
      gbr_dl: "5 Mbps"
      gbr_ul: "5 Mbps"
```

MCC/MNC 是应用运营商标识，必须显式填写；MNC 字符串保留两位/三位区别。容器固定 `0xFF10`，部署前检查编号冲突。Helm 对应 `networking.smf.userIntent.enabled/mcc/mnc/notifyUri`，默认 `enabled: false`。双栈会话当前选择已分配 IPv4；纯 IPv6 会话尚未提供可确认的业务 IPv6 主机地址，通知会记录失败，不能将 IPv6 前缀当作 UE 主机地址。

Helm 的初始 QoS 配置位于 `helm/xcn/values.yaml`：

```yaml
networking:
  smf:
    userIntent:
      faceRecognition:       # 人脸识别，aiModel=2
        fiveQi: 2
        priorityLevel: 40
        maxbrDl: "500 Mbps"
        maxbrUl: "500 Mbps"
        gbrDl: "5 Mbps"
        gbrUl: "5 Mbps"
      objectRecognition:     # 物体识别，aiModel=3
        fiveQi: 3
        priorityLevel: 30
        maxbrDl: "500 Mbps"
        maxbrUl: "500 Mbps"
        gbrDl: "5 Mbps"
        gbrUl: "5 Mbps"
```

创建专载和模型互切时应用目标模型的全部六项配置；同模型的新事务只在当前已生效优先级上减 1，最低 1，保留当前带宽。On+LLM 保留全部已有参数。旧 SMF 配置省略这些配置组或个别字段时采用以上默认值。5QI 配置名为 `fiveQi`（SMF YAML 为 `five_qi`），整数范围为 1～255，默认人脸为 2、物体为 3；优先级范围为 1～127；带宽必须为带单位的正数字符串，支持 `bps/Kbps/Mbps/Gbps/Tbps`，每项不超过 4 Tbps，且同方向 GBR 不超过 MBR。启用功能后，非法 QoS 配置会导致 SMF 启动失败并记录错误。

修改 values 后需 Helm 升级并重建 SMF 所在 Pod，使 SMF 重新读取配置；不支持热加载，也不主动重设存量专载。后续创建或模型切换使用新值，同模型请求继续基于存量状态递减。

## 消息及策略

示例完整 IE：`7b 00 0a 80 ff 10 06 64 f0 00 01 02 02`。容器内容必须为 PLMN 三字节加 `commStatus/aiModel/gpuLevel` 三字节。按容器 ID 遍历，校验长度、重复 ID、PLMN、枚举；有目标容器时 INFO 十六进制日志包括 ID、长度、PLMN 和业务字节。截断日志只打印实际可用数据。

| 输入 | 本功能专载操作 |
| --- | --- |
| On + LLM | 保留当前资源与全 IP 过滤规则，无资源时不创建 |
| On + 人脸，无专载 | 使用 faceRecognition 配置创建（默认 5QI 2、priorityLevel 40） |
| On + 物体，无专载 | 使用 objectRecognition 配置创建（默认 5QI 3、priorityLevel 30） |
| On + 同模型且实际 5QI 匹配配置 | priorityLevel 递减，最低 1；到 1 后允许 no-op |
| On + 模型切换 | 同一 appSessionId/QFI 修改，5QI、优先级及带宽重置为目标模型配置 |
| Off | 删除本功能专载，保留默认流；本来无专载则 no-op |

模型枚举与 5QI 分离：UE 仍上报 aiModel=2/3，不需要随配置改变枚举。两类模型可以配置相同 5QI；核心网保留最近成功生效的模型关联，切换模型时重置目标配置，同模型再递减。LLM 不清除该关联。如果切换后的全部 QoS 已与当前值相同，则走 NAS-only 成功流程，避免空策略变更。

GPU 仅通知，不参与 QoS。ARP 8、NOT_PREEMPT/PREEMPTABLE；MBR/GBR 使用对应模型配置，默认双向 MBR 500 Mbps、GBR 5 Mbps。过滤规则等效于 `permit out ip from 0.0.0.0/0 to assigned` 和 `permit in ip from assigned to 0.0.0.0/0`；后者按现有 PCC/IPFW 约定归一化存储为反向描述加 UPLINK 方向。

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

JSON 包含会话 IPv4、SUPI、三个业务整数、`qosResult`、业务三字节小写十六进制 `rawPco`、实际发起时 Unix 毫秒时间。只有 SUCCESS + On + 已有本功能专载时包含 `fiveQi`，值为实际生效的 5QI（包括配置的非 2/3 值）。图片不经过本接口。

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

自定义初始 QoS 测试：在隔离配置中将 `smf.user_intent.face_recognition` 设置为 `five_qi: 1`、优先级 55、MBR 下行/上行 120/80 Mbps、GBR 下行/上行 12/8 Mbps；`object_recognition` 设置为 `five_qi: 4`、优先级 25、MBR 240/160 Mbps、GBR 24/16 Mbps，再执行：

```bash
XCN_INTENT_TEST=1 XCN_INTENT_PROFILE_TEST=1 \
  build/tests/vonr/vonr -e info -c /tmp/intent-profiles.yaml dedicated-bearer-test
```

此场景验证 NAS/N2/PFCP 闭环、创建与模型互切后的六项 QoS、同模型递减时带宽保留、LLM 保留、appSessionId/QFI 不变、重传和删除后的默认流用户面 ping。共 7 个新意图事务，使用独立通知日志，不能套用默认场景的 51/52 条通知校验。

相同 5QI 场景：将上述物体配置的 `five_qi` 改为 1，加环境变量 `XCN_INTENT_SHARED_5QI_TEST=1` 运行，验证切换时仍重置物体的优先级和带宽。完全相同配置场景：将物体的全部配置复制为人脸配置，使用 `XCN_INTENT_TEST=1 XCN_INTENT_EQUAL_PROFILE_TEST=1` 运行同一测试，验证切换时仅发 NAS Command，接着同模型递减；该场景有 6 次通知。

2026-10-12 可配置 5QI 实测：全量编译、unit、Helm 默认/自定义及省略新字段渲染通过；5QI 1/4、两模型同为 1、两组完整配置相同三组隔离集成测试通过，通知分别为 7/7/6 条，无重复。原默认配置意图生命周期的 52 条通知及原 dedicated-bearer 单会话/并发回归通过。5QI 为 0、256、小数、超大整数和数组时，独立 SMF 启动均受控拒绝。新模块 ASan/UBSan 单测通过，沿用下文的已知 talloc 基线分配抑制。

2026-10-12 初始 QoS 配置实测：全量编译、unit、Helm lint 及默认/自定义渲染通过；上述自定义场景中 PCF 已生效策略和实际 N2 下发带宽逐项一致，7 次通知无重复。未增加新配置的旧 YAML 通过原有意图生命周期及 52 条通知校验，原 dedicated-bearer 单会话和并发回归通过。非法优先级、错误配置类型、带宽超限和 GBR 大于 MBR 的独立 SMF 启动均受控拒绝，无断言退出。通知使用隔离接收端。

本次隔离 Docker 实测：全量构建和 unit 通过；意图成功/拒绝生命周期及默认用户面 ping 通过；T3591 重发通过；通知 400/500/200/超时/连接失败五组全部通过，每组原 QoS/NAS 流程正常且无通知重试；原 dedicated-bearer 单会话及并发双会话回归通过。

新模块 ASan/UBSan 单测通过。LeakSanitizer 初始报告 96 字节 `talloc_enable_null_tracking` 全局分配，已在原有 qos-priority 单测复现；复测仅按该函数名抑制这一基线分配，未关闭其他泄漏检测。本结果不等于真实 UE/gNB 端到端验收。
