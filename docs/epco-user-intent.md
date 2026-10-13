# ePCO UE 用户意图：核心网实现与联调

实现范围为 AMF、SMF、PCF 及配置和测试。UE/AP/modem 与算力服务器由其他团队实现；`tests/helpers/intent-notify-server.py` 仅为隔离测试接收端。功能默认关闭，不改变未携带目标容器请求的正常入口。尚未进行真实 UE/gNB 联调，不能据软件模拟测试宣称独立 DRB 已建立。

## 配置

SMF YAML：

```yaml
smf:
  user_intent:
    enabled: true
    notify_uri: http://10.2.5.125:8000/api/v1/intent/notify
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

按终端侧最新确认，当前使用功能测试格式：容器内容只有三个业务字节，不含 MCC/MNC，不需要配置应用运营商标识。旧 SMF YAML 中的 `user_intent.mcc/mnc` 会告警并忽略；旧 Helm values 中的同名配置不再渲染。容器固定 `0xFF10`，部署前检查编号冲突。Helm 对应 `networking.smf.userIntent.enabled/notifyUri`，默认 `enabled: false`。双栈会话当前选择已分配 IPv4；纯 IPv6 会话尚未提供可确认的业务 IPv6 主机地址，通知会记录失败，不能将 IPv6 前缀当作 UE 主机地址。

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

示例完整 IE：`7b 00 07 80 ff 10 03 01 02 02`，表示 On、人脸识别、GPU 中档。ePCO 内容长 7 字节，完整 IE 长 10 字节；`0xFF10` 容器长度固定为 3，内容从偏移 0 起依次为 `commStatus/aiModel/gpuLevel`。本格式是双方确认的功能测试约定，不是带运营商标识的标准运营商自定义容器格式；不自动兼容旧 6 字节载荷，旧格式会受控拒绝，避免错位解析。

按容器 ID 遍历，校验长度、重复 ID、枚举；有目标容器时 INFO 十六进制日志包括 ID、长度和三个业务字节，例如 `ff 10 03 01 02 02`。截断日志只打印实际可用数据。前期联调记录中的 6 字节 PLMN 前缀报文属于历史格式，不再作为当前终端报文依据。

日志中的 `length` 是目标容器声明的内容长度；`available` 是从目标容器内容起到 ePCO 缓冲区末尾的剩余字节数，会包含后续容器。例如目标容器后还有 `00 0d 00`（DNS 请求）时，日志为 `length:3 available:6`，但本容器仍只解析三个业务字节，十六进制输出为 `ff 10 03 01 02 02`，不含后面的 DNS 项。只有目标容器时则为 `length:3 available:3`。

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

2026-10-12 三字节功能测试格式实测：全量编译、unit 和 Helm lint/渲染通过；解析覆盖全部 18 种合法字段组合，截断、非法长度/枚举、重复容器和旧六字节格式均受控拒绝。无 MCC/MNC 配置的隔离 NAS/N2/PFCP 集成测试通过：默认策略生命周期（含 Command 重发、资源拒绝）通知 52 条，自定义 5QI 1/4 场景通知 7 条，字段与顺序正确，无重复；创建及删除后的 GTP-U ping 和原 dedicated-bearer 单会话/并发回归通过。ASan/UBSan 单测通过，仅抑制下述已知 talloc 基线分配。此处使用软件模拟 UE，不代替终端团队实际三字节上报验收。

2026-10-12 部署及 2026-10-13 收尾核验：代码提交 `de664c580`，K8s Helm release `xcn` revision 162，镜像 `localhost:5000/xcn-runtime:epco-intent-3byte-de664c580-1012`。运行中的 SMF 二进制与已测试构建的 SHA-256 一致，实际配置启用功能且无 MCC/MNC。软件 UE 向实际 K8s 核心网发送三字节 NAS 请求，SMF 日志确认 `length:3`；默认策略完整生命周期通过。与已部署算力服务的 h2c 抓包中共有 52 条通知，全部收到 204 且响应体为空，业务字段、顺序和重传去重校验通过；其中 50 条 QoS SUCCESS、2 条预期拒绝场景 FAILURE，HTTP 成功不替代 QoS 结果。观测通知响应耗时约 0.82～2.49 ms（含网络）。

实际 K8s 联调使用软件模拟 UE，显式跳过该测试会话的 GTP-U ping；真实 OAI UE 重新注册后另行验证到 `10.2.5.125` 的用户面，8/8 ping 成功。此连通性结果不证明真实终端已发送三字节意图，也不证明独立 DRB 已建立。最终核心网 9/9 就绪，测试订阅者已删除，大屏已用 Off 通知恢复默认承载，隔离测试容器已停止。原始证据归档：`/tmp/epco-intent-three-byte-evidence-20261013.tar.gz`。

2026-10-12 可配置 5QI 实测：全量编译、unit、Helm 默认/自定义及省略新字段渲染通过；5QI 1/4、两模型同为 1、两组完整配置相同三组隔离集成测试通过，通知分别为 7/7/6 条，无重复。原默认配置意图生命周期的 52 条通知及原 dedicated-bearer 单会话/并发回归通过。5QI 为 0、256、小数、超大整数和数组时，独立 SMF 启动均受控拒绝。新模块 ASan/UBSan 单测通过，沿用下文的已知 talloc 基线分配抑制。

2026-10-12 初始 QoS 配置实测：全量编译、unit、Helm lint 及默认/自定义渲染通过；上述自定义场景中 PCF 已生效策略和实际 N2 下发带宽逐项一致，7 次通知无重复。未增加新配置的旧 YAML 通过原有意图生命周期及 52 条通知校验，原 dedicated-bearer 单会话和并发回归通过。非法优先级、错误配置类型、带宽超限和 GBR 大于 MBR 的独立 SMF 启动均受控拒绝，无断言退出。通知使用隔离接收端。

本次隔离 Docker 实测：全量构建和 unit 通过；意图成功/拒绝生命周期及默认用户面 ping 通过；T3591 重发通过；通知 400/500/200/超时/连接失败五组全部通过，每组原 QoS/NAS 流程正常且无通知重试；原 dedicated-bearer 单会话及并发双会话回归通过。

新模块 ASan/UBSan 单测通过。LeakSanitizer 初始报告 96 字节 `talloc_enable_null_tracking` 全局分配，已在原有 qos-priority 单测复现；复测仅按该函数名抑制这一基线分配，未关闭其他泄漏检测。本结果不等于真实 UE/gNB 端到端验收。
