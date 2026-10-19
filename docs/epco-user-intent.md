# ePCO UE 用户意图：核心网实现与联调

实现范围为 AMF、SMF、PCF 及配置和测试。UE/AP/modem 与算力服务器由其他团队实现；`tests/helpers/intent-notify-server.py` 仅为隔离测试接收端。功能默认关闭，不改变未携带目标容器请求的正常入口。尚未进行真实 UE/gNB 联调，不能据软件模拟测试宣称独立 DRB 已建立。

2026-10-13 算力服务已迁移至 `10.2.0.102:8000`，实际核心网与新服务器的验证及算力端修复复测结果见[迁移联调记录](epco-user-intent-server-migration-test.md)。

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
      flow_descriptions:
        - "permit out ip from 0.0.0.0/0 to assigned"
        - "permit in ip from assigned to 0.0.0.0/0"
      five_qi: 2
      priority_level: 40
      maxbr_dl: "500 Mbps"
      maxbr_ul: "500 Mbps"
      gbr_dl: "5 Mbps"
      gbr_ul: "5 Mbps"
    object_recognition:
      flow_descriptions:
        - "permit out ip from 0.0.0.0/0 to assigned"
        - "permit in ip from assigned to 0.0.0.0/0"
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
        flowDescriptions:
          - "permit out ip from 0.0.0.0/0 to assigned"
          - "permit in ip from assigned to 0.0.0.0/0"
        fiveQi: 2
        priorityLevel: 40
        maxbrDl: "500 Mbps"
        maxbrUl: "500 Mbps"
        gbrDl: "5 Mbps"
        gbrUl: "5 Mbps"
      objectRecognition:     # 物体识别，aiModel=3
        flowDescriptions:
          - "permit out ip from 0.0.0.0/0 to assigned"
          - "permit in ip from assigned to 0.0.0.0/0"
        fiveQi: 3
        priorityLevel: 30
        maxbrDl: "500 Mbps"
        maxbrUl: "500 Mbps"
        gbrDl: "5 Mbps"
        gbrUl: "5 Mbps"
```

创建专载和模型互切时应用目标模型的过滤规则及全部六项 QoS 配置；同模型的新事务只在当前已生效优先级上减 1，最低 1，保留当前带宽和过滤规则。On+LLM 保留全部已有参数。旧 SMF 配置省略这些配置组或个别字段时采用以上默认值。5QI 配置名为 `fiveQi`（SMF YAML 为 `five_qi`），整数范围为 1～255，默认人脸为 2、物体为 3；优先级范围为 1～127；带宽必须为带单位的正数字符串，支持 `bps/Kbps/Mbps/Gbps/Tbps`，每项不超过 4 Tbps，且同方向 GBR 不超过 MBR。启用功能后，非法 QoS 配置会导致 SMF 启动失败并记录错误。

`flowDescriptions`（SMF YAML 为 `flow_descriptions`）支持每个模型独立配置 1～15 条过滤规则。省略该字段沿用以上两条全 IP 默认规则；显式配置空列表、非字符串列表、超过 15 条或格式非法时拒绝启动。创建时使用目标模型规则；模型切换时替换同一专载的规则，即使两个模型的全部 QoS 参数相同，也不能跳过规则变更。同模型递减优先级及 On+LLM 均保留已有规则。仅操作本功能专载，不修改默认 QoS Flow 或其他业务资源。

支持的规则形式为 `permit in/out ip/tcp/udp/协议号 from 地址 [端口] to 地址 [端口]`；协议号范围 1～255，地址支持数字 IPv4/IPv6、CIDR、`any` 和 UE 地址占位符 `assigned`。`assigned` 应放在 `out` 的目标端或 `in` 的源端；端口支持 `any`、0～65535 或单个递增范围，不支持域名、逗号列表和额外选项。每条规则最长 511 字节。示例只匹配算力端 TCP 8000：

```yaml
flowDescriptions:
  - "permit out tcp from 10.2.0.102/32 8000 to assigned"
  - "permit in tcp from assigned to 10.2.0.102/32 8000"
```

修改 values 后需 Helm 升级并重建 SMF 所在 Pod，使 SMF 重新读取配置；不支持热加载，也不主动重设存量专载。后续创建或模型切换使用新值，同模型请求继续基于存量状态递减。

## 消息及策略

示例完整 IE：`7b 00 0a 80 ff 10 06 64 f0 00 01 02 02`。容器内容必须为 PLMN 三字节加 `commStatus/aiModel/gpuLevel` 三字节。按容器 ID 遍历，校验长度、重复 ID、PLMN、枚举；有目标容器时 INFO 十六进制日志包括 ID、长度、PLMN 和业务字节。截断日志只打印实际可用数据。

| 输入 | 本功能专载操作 |
| --- | --- |
| On + LLM | 保留当前资源与过滤规则，无资源时不创建 |
| On + 人脸，无专载 | 使用 faceRecognition 配置创建（默认 5QI 2、priorityLevel 40） |
| On + 物体，无专载 | 使用 objectRecognition 配置创建（默认 5QI 3、priorityLevel 30） |
| On + 同模型且实际 5QI 匹配配置 | priorityLevel 递减，最低 1；到 1 后允许 no-op |
| On + 模型切换 | 同一 appSessionId/QFI 修改，5QI、优先级、带宽及过滤规则重置为目标模型配置 |
| Off | 删除本功能专载，保留默认流；本来无专载则 no-op |

模型枚举与 5QI 分离：UE 仍上报 aiModel=2/3，不需要随配置改变枚举。两类模型可以配置相同 5QI；核心网保留最近成功生效的模型关联，切换模型时重置目标配置，同模型再递减。LLM 不清除该关联。如果切换后的全部 QoS 和过滤规则均与当前值相同，则走 NAS-only 成功流程，避免空策略变更。

GPU 仅通知，不参与 QoS。ARP 8、NOT_PREEMPT/PREEMPTABLE；MBR/GBR 使用对应模型配置，默认双向 MBR 500 Mbps、GBR 5 Mbps。过滤规则默认等效于 `permit out ip from 0.0.0.0/0 to assigned` 和 `permit in ip from assigned to 0.0.0.0/0`；后者按现有 PCC/IPFW 约定归一化存储为反向描述加 UPLINK 方向。

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
- 未被意图入口接管、且不含 Requested QoS Rules / Requested QoS Flow Descriptions 的修改请求，当前返回 Modification Reject，不提供通用的标准 ePCO-only 成功处理。2026-10-19 修复使此类拒绝保留原 PDU 会话及资源；含标准 QoS IE 的既有解析/执行异常路径未被全局改写。

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

独立过滤规则场景：两模型均设置为 5QI 2、priorityLevel 40、双向 MBR 500 Mbps、GBR 5 Mbps；人脸的两条规则仅匹配远端 `10.45.0.1/32`，物体仅匹配 `10.45.0.2/32`，分别使用 out/in 双向形式，再执行：

```bash
XCN_INTENT_TEST=1 XCN_INTENT_FLOW_TEST=1 \
  build/tests/vonr/vonr -e info -c /tmp/intent-flow-flows.yaml dedicated-bearer-test
```

此场景覆盖 QoS 完全相同但规则不同的模型切换，检查 PCF 已提交规则、appSessionId/QFI 不变、重传去重、同模型及 LLM 保留过滤规则、切回人脸后的专载用户面 ping 和删除后的默认流 ping，共 6 次通知。

2026-10-16 过滤规则配置实测：全量编译、完整 unit、Helm lint、默认及独立自定义规则渲染通过。上述过滤规则场景、完全相同配置场景及自定义 5QI/带宽场景通过，通知分别为 6/6/7 条；原意图生命周期 52 条通知及原 dedicated-bearer 单会话/并发回归通过。空列表、错误类型、16 条超限、非法 IP 和端口的独立 SMF 启动均按标准初始化错误退出，无断言崩溃。新增解析及匹配单测经 ASan/UBSan 检查通过，仅沿用已知 talloc 基线分配抑制。上述通知使用隔离测试接收端。

同日已将实现提交 `3bdb90f20` 部署到当前 K8s，镜像 `localhost:5000/xcn-runtime:epco-intent-flows-3bdb90f20-1016`；保留原有网络与 QoS 配置，两模型显式设置上述默认双向规则。核心网 9/9、NRF/SCP 2/2 就绪；实际 K8s 上的软件 NAS 客户端回归通过，SMF 已记录配置规则安装。测试 UE 重建会话后地址为 `10.45.0.3`，经 `oaitun_ue1` 到 `10.2.0.119` 的稳态 ping 8/8 成功；这项检查不代表真实 UE 已验证自定义过滤规则或独立 DRB。测试 NAS 会话及 MongoDB 临时订户已清理。

**本次到实际算力服务的通知联调未通过网络可达性检查。** 测试产生的 52 次通知均按配置超时、不重试，未影响 NAS/资源回归结果。主机路由直连 `ens5f0`，但 `10.2.0.102` 的 ARP 邻居状态为 `FAILED`，ping 0/3；绕过代理直访大屏返回 `No route to host`。抓包仅有到 `10.2.0.102:8000` 的 SYN，无 TCP 握手返回，无法检验服务端处理或 204 响应，不能据此认定算力服务程序有缺陷。现场证据保存在 `/tmp/intent-flow-live.pcap`、`/tmp/intent-flow-live-e2e.log`、`/tmp/intent-flow-live-smf.log`；网络恢复后需重新验证实际通知链路。

用户随后确认算力服务器为临时停机，要求暂停算力侧联调。2026-10-16 后续验证全部在隔离核心网内进行，通知目标仅为 `http://127.0.0.1:8000/api/v1/intent/notify`，未再访问实际算力服务器。复用上述独立过滤规则用例，每组执行 6 个新意图事务：

| 本地接收端场景 | NAS/资源用例 | 接收端请求数 | 核心网通知失败回调数 |
| --- | --- | --- | --- |
| 204 | 通过 | 6 | 0 |
| 400 | 通过 | 6 | 6 |
| 500 | 通过 | 6 | 6 |
| 200（不满足成功契约） | 通过 | 6 | 6 |
| 延迟 1.2 秒 | 通过 | 6 | 6 |
| 端口未监听、连接拒绝 | 通过 | 0 | 6 |

共 36 个事务全部通过；模型切换、同模型递减、LLM 保留、同一 appSessionId/QFI、重传去重、删除后的默认流 ping 和会话释放均正常，通知失败未改变已提交策略及后续事务结果。各有接收端的场景均严格收到 6 次请求，无额外重试；超时回调实测为 1.000～1.001 秒。会话释放后 PCF 查询为 404，隔离 MongoDB 临时订户数为 0。证据与补测脚本保存在 `/tmp/intent-core-isolation-evidence/`。

本轮仅修正集成测试释放会话后读取 PSI 的悬空引用，并使所有模型测试都等待最后一次通知超时回调。核心网实现及部署参数继续采用前述已部署版本；既有混合标准 QoS 请求、纯 IPv6 通知和真实 RAN 映射边界仍按本文记录，不将本轮通过结果扩展为这些未完成项的验收。

2026-10-12 可配置 5QI 实测：全量编译、unit、Helm 默认/自定义及省略新字段渲染通过；5QI 1/4、两模型同为 1、两组完整配置相同三组隔离集成测试通过，通知分别为 7/7/6 条，无重复。原默认配置意图生命周期的 52 条通知及原 dedicated-bearer 单会话/并发回归通过。5QI 为 0、256、小数、超大整数和数组时，独立 SMF 启动均受控拒绝。新模块 ASan/UBSan 单测通过，沿用下文的已知 talloc 基线分配抑制。

2026-10-12 初始 QoS 配置实测：全量编译、unit、Helm lint 及默认/自定义渲染通过；上述自定义场景中 PCF 已生效策略和实际 N2 下发带宽逐项一致，7 次通知无重复。未增加新配置的旧 YAML 通过原有意图生命周期及 52 条通知校验，原 dedicated-bearer 单会话和并发回归通过。非法优先级、错误配置类型、带宽超限和 GBR 大于 MBR 的独立 SMF 启动均受控拒绝，无断言退出。通知使用隔离接收端。

本次隔离 Docker 实测：全量构建和 unit 通过；意图成功/拒绝生命周期及默认用户面 ping 通过；T3591 重发通过；通知 400/500/200/超时/连接失败五组全部通过，每组原 QoS/NAS 流程正常且无通知重试；原 dedicated-bearer 单会话及并发双会话回归通过。

新模块 ASan/UBSan 单测通过。LeakSanitizer 初始报告 96 字节 `talloc_enable_null_tracking` 全局分配，已在原有 qos-priority 单测复现；复测仅按该函数名抑制这一基线分配，未关闭其他泄漏检测。本结果不等于真实 UE/gNB 端到端验收。

2026-10-19 另一套环境反馈 `Invalid modification request [modify:0] → State machine exception → Removed Session → Not found`。`modify:0` 指本次待修改 QoS Flow 列表为空，不是会话 ID、PTI 或已有专载总数。按当前代码，意图入口仅在功能关闭、没有 ePCO IE、或合法 ePCO 没有目标容器时返回原流程；PLMN、容器格式或 PTI 校验失败会在意图入口拒绝，不会回落到这个数量检查。因此另一套环境还需核验实际生效的 `user_intent.enabled`、新请求的完整 ePCO 及运行镜像，不能仅凭配置文件编辑或 AMF 的 `[PSI:PTI]` 日志认定具体旁路原因。

隔离环境已复现上述错误链。最小修复在运行任何标准 QoS 解析器之前，识别未携带两类标准 QoS IE 且未被意图接管的请求，沿用原来的 NAS Reject/HTTP 错误响应，返回“本次请求已处理”，避免状态机异常删除正常会话；不将这类请求伪装成意图成功。新诊断日志包含 SUPI、PSI、实际 SMF PTI、`intent-enabled`、ePCO presence、长度和完整 presence mask，并明确记录 `keep PDU session`。

回归使用隔离 NF、MongoDB、TUN 和本地通知接收端，未连接实际算力服务器：功能开启时测试无 ePCO/非 FF10 容器，分别在无专载和已有专载时拒绝并保留原会话、PCF 策略、appSessionId/QFI 和用户面 ping；同一会话随后完成合法 FF10 创建/删除及不携带 FF10 的标准 QoS Flow Descriptions 修改。功能关闭时测试无 ePCO、非 FF10 和 FF10 三类请求，全部受控拒绝，默认流 ping 正常，无意图资源或通知。新用例通过，功能开启/关闭分别产生 2/0 条通知；正常意图 52 条通知、自定义过滤规则 6 条通知及原 dedicated-bearer 单会话/并发回归、完整 unit 均通过。临时订户清理后为 0。

复现新用例：使用默认人脸 QoS 的隔离配置及本地通知接收端，执行 `XCN_INTENT_TEST=1 XCN_INTENT_BYPASS_TEST=1 build/tests/vonr/vonr -e info -c /tmp/intent-bypass-enabled.yaml dedicated-bearer-test`。使用功能关闭的隔离配置时再加 `XCN_INTENT_DISABLED_TEST=1`，配置文件换为 `/tmp/intent-bypass-disabled.yaml`。修复前失败证据为 `/tmp/intent-bypass-before.log`，修复后脚本、日志及结果保存在 `/tmp/intent-bypass-evidence/`。另一套服务器需更新包含本修复的 SMF 镜像后才能获得保护和新增诊断，本次未直接修改那套环境。

2026-10-19 后续终端诊断确认 PSI=5、PTI=6 的 Modification Request 收到 `INVALID_MANDATORY_IE`（cause 96）。日志显示的 `64 F0 00 01 03 03` 为合法 PLMN 460/00、On、物体、GPU高，长度为 6；但仅打印容器不能证明公共头、完整配置项或期望 PLMN 校验通过。PTI=6 合法；意图入口拒绝非法 PTI 的 cause 为 81。通用旁路拒绝也可能返回 96，因此应关联同次请求的 SUPI/PSI/PTI 和阶段日志，不能只凭 cause 指定根因。该片段的 FF10 行时间为 15:36:41.919，终端修改请求却在 15:37:51.49，二者是否为同次或粘贴有误仍待确认。AMF 定时清理的是旧 NG 上下文 RAN/AMF ID 25/7，新上下文为 29/8，不能把旧上下文清理当作新请求被拒原因。

诊断增强保留原有校验及 NAS cause，仅补充：请求 SUPI/PSI/PTI、ePCO 内容长度与公共头、具体拒绝原因、PLMN 接收/期望编码，以及成功校验的业务值。`Intent validated` 只表示输入校验通过，资源完成仍看后续策略、NAS/N2/PFCP 和最终结果。例如：

```text
Intent ePCO rejected: PLMN mismatch [received:0x64f000 expected:0x64f011]
Intent ePCO rejected: invalid header [received:0x00 expected:0x80]
[imsi-...:5] Intent request rejected [PTI:6 cause:96]
```

PLMN 期望值来自实际 SMF `user_intent.mcc/mnc`，不是从 SUPI 或驻留 PLMN 推导。应用协议约定为 460/00 时使用字符串 `mcc: "460"`、`mnc: "00"`；若双方改为 460/11，则线上前缀应为 `64 F0 11`。只有一个目标容器时，合法完整 ePCO IE 示例为 `7B 00 0A 80 FF 10 06 64 F0 00 01 03 03`；多个容器时应按真实总长度构造，保留其他配置项。

新增隔离集成用例 `XCN_INTENT_TEST=1 XCN_INTENT_VALIDATION_TEST=1`（默认模型 QoS、应用 PLMN 460/00）覆盖 PLMN 不匹配、公共头错误、内容长度 5、重复容器、非法业务枚举和尾部截断六种 cause 96，以及 PTI=0/255 两种 cause 81。每次拒绝后检查 PCF 会话仍存在、没有专载、默认流 ping 正常；随后复用 PTI=6 和上述合法六字节成功创建物体专载 5QI3/30，再切换为同一 appSessionId/QFI 的人脸专载 5QI2/40，最终 Off 删除。仅三个有效意图产生 3 条通知，八个无效请求不通知。完整 unit、该用例、旁路保护的启用/停用用例、52 条正常意图通知回归及 ASan/UBSan 单测均通过，沿用已知 talloc 基线分配抑制。通知仅指向本地接收端，另一套部署未直接更新；证据和脚本位于 `/tmp/intent-parse-evidence/`。
