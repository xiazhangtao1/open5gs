# ePCO 用户意图与真实算力服务器联调记录

日期：2026-10-12。目标：升级 K8s 核心网，与已部署的
`http://10.2.5.125:8000/api/v1/intent/notify` 实测。算力服务与终端源码未修改。

## 最终部署

| 项目 | 实测值 |
| --- | --- |
| 核心网代码 | `1ee28ec67`，含意图实现 `82b3abfa6` 及本次 PCF 修复 |
| Runtime 镜像 | `localhost:5000/xcn-runtime:epco-intent-1ee28ec67-1012` |
| Registry digest | `sha256:50d020e0d01e827b2f293b96cb43da286bf245612c5ff442bb257817aa8c03f0` |
| Helm | namespace `xcn`，release `xcn`，revision 159 |
| 核心网 Pod | `xcn-5gc-799bf596f5-dzjrn`，9/9 Ready，复测后所有容器重启次数为 0 |
| 配置 | `userIntent.enabled=true`，应用所属 MCC/MNC=`460/00`；驻留网络=`460/11` |
| 网络 | 保留 hostNetwork、现有 VPP/memif、N3/N6 及地址池配置 |

按用户授权替换旧部署。最终镜像包含当前项目安装的二进制和库，VPP 镜像保持现有版本。

## 实测结果

| 检查 | 结果及证据范围 |
| --- | --- |
| K8s 核心网完整意图生命周期 | 通过：控制测试端模拟 UE/gNB，真实 AMF/SMF/PCF/UPF 执行 NAS/N2/PFCP |
| 策略 | 创建、同模型递减 40→1、下限 no-op、模型切换保持 QFI、LLM 保留、Off 删除均通过 |
| 事务 | 活跃事务重传不重复操作或通知；完成后的 PTI 复用、相同载荷新事务递减均通过 |
| 拒绝路径 | 非法业务枚举及受控 UE 拒绝；有效失败意图通知 FAILURE，省略 fiveQi |
| 实际通知抓包 | 最终部署 52 条：50 SUCCESS、2 个预期 FAILURE；全部 HTTP/2 204、空响应体 |
| 通知内容 | 逐条精确核对顺序、次数、业务字节、rawPco、fiveQi 条件、时间戳和会话 IP；无重复 |
| 通知时延 | 抓包中请求 END_STREAM 至响应 END_STREAM 为 0.777～1.196 ms |
| 通知地址 | JSON ueIp=`10.45.1.2`；HTTP 请求直达指定服务器，h2c prior knowledge、无鉴权 |
| 隔离完整用户面回归 | 通过：专载及删除后的默认流 GTP-U ping、PCF 释放后存活检查；通知仍指向真实服务器 |
| 构建及回归 | 全量增量构建、unit、原 dedicated-bearer 单会话/并发场景通过 |
| 真实 UE 默认用户面 | UE `10.45.0.3` 经 `oaitun_ue1` 到算力服务器，稳定复测 20/20，RTT 4.135～199.280 ms |

K8s 测试显式设置 `XCN_INTENT_CONTROL_PLANE_ONLY=1`，跳过模拟 gNB 的 GTP-U ping；
其 N3 返回路由与实际 VPP 部署不同。该选项默认关闭，隔离回归保留原用户面断言。
真实 OAI UE 的默认用户面另行测试，不能据此宣称真实 UE 已实现 0xFF10 上报或真实 RAN 已建立新专载/独立 DRB。

最终升级后的 UE 初次探测为 8/10，随后稳定复测为 20/20。同期主机直连算力服务器
20/20、RTT 0.065～0.190 ms。用户面有明显时延波动，当前证据无法定位到无线、调度或转发中的具体环节；
不将其归因于算力接口。日志还记录了 BSF 注册 HTTP 504，PCF 按现有代码回退生成 SM 策略，未阻断本次会话与意图流程。

## 已修复的核心网问题

原版本在释放仍关联意图应用资源的会话时，遍历全部应用发送 AF termination callback。
意图资源通过内部 SMF/PCF 事务创建，没有 `naf.client`，触发
`pcf_sbi_send_policyauthorization_terminate_notify: Assertion 'client' failed`，导致 PCF 重启。
本次 K8s 联调捕获到该崩溃。

`src/pcf/npcf-handler.c` 在会话删除通知循环中跳过 `intent_owned` 应用，由既有会话释放逻辑统一清理。
普通 AF 的终止回调保留。回归增加会话释放后查询 PCF 返回 404 的检查，覆盖拒绝创建后留下待确认应用的场景，
避免只看到 NAS 测试成功而漏掉 PCF 退出。修复后的隔离和 K8s 回归均通过，PCF 无重启。

## 算力接口检查与问题

新增 `tests/helpers/verify-live-intent-server.py`，实测 34 次请求：

- 18 组合法输入全部 204，包括三个模型/三个 GPU 档位、LLM 保留 fiveQi、QoS FAILURE、Off 和最小必填请求。
- 15 组负例中 14 组正确返回 400 和错误 JSON；覆盖字段缺失、越界枚举、字符串/布尔/浮点类型、空请求和损坏 JSON。
- 最后一次 Off 收尾返回 204。全部使用 HTTP/2，总请求时延 1.067～2.049 ms；该时延包含网络开销。

### S1：非法 ueIp 被接受

以下请求实测返回 **204**，应按非法字段返回 **400**：

```bash
curl --noproxy '*' --http2-prior-knowledge -sS -i \
  'http://10.2.5.125:8000/api/v1/intent/notify' \
  -H 'Content-Type: application/json; charset=utf-8' \
  --data-binary '{"ueIp":"invalid-ip","commStatus":1,"aiModel":2,"gpuLevel":2,"qosResult":"SUCCESS"}'
```

建议算力团队验证 IPv4/IPv6 地址语法，非法地址不得进入后台处理。核心网本次抓包发送的会话 IPv4 均合法。

### S2：LLM 无专载时展示“已开启专载”

先发送 Off 清除展示，再单条发送 `commStatus=1, aiModel=1, gpuLevel=2, qosResult=SUCCESS`，**省略 fiveQi**，
随后每秒读取 `/api/monitor`，连续 4 秒均显示 `comm_config="已开启专载"`。
按通知契约，此输入表示 LLM 无本功能专载；页面应如实显示默认流/无本功能专载。
这是已观察到的展示不一致，不能据此判断服务器实际创建了网络资源。

人脸、物体和 Off 在间隔发送后均能更新对应模型/通信状态。快速连续发送时 150 ms 快照可能仍为旧值，
不以该现象判定处理失败。监控返回 GPU `10%/20%/80%` 是服务器展示映射，不能当作 UE 原始利用率测量值。

### S3：OpenAPI 契约描述不完整

`/openapi.json` 中该端点实际只声明响应 200，且没有结构化 requestBody；文字描述写明 204/400/500。
建议同步声明请求模型和真实响应码，避免其它团队按生成客户端误判成功。

服务器 HTTP 接收、字段校验和监控可观察状态已经实测；尚未取得服务器后台应用日志，
未测试图片上传、模型加载/推理及后台任务异常，因此不宣称完整算力业务已验收。500 故障注入没有施加到真实服务器；
核心网通知失败不重试、不回滚的测试仍采用此前隔离 fixture 结果，见 [实现说明](epco-user-intent.md)。

## 复现与证据

接口脚本依赖 Ubuntu curl 的 HTTP/2 支持和 Python 3。它会发送实际测试通知，须选择约定的测试 UE 标识。
发现接口问题时退出 1，并保存逐条请求、响应、时延和可选监控快照：

```bash
python3 tests/helpers/verify-live-intent-server.py \
  --url http://10.2.5.125:8000/api/v1/intent/notify \
  --ue-ip 10.45.0.2 --supi imsi-999700000009051 \
  --monitor-url http://10.2.5.125:8000/api/monitor \
  --output /tmp/intent-server-results.json
```

在已经准备好隔离 NF 配置或本次归档的全部本地 NF 禁用配置下，可运行现有集成测试。
本次 K8s 客户端在 UPF 容器内使用 hostNetwork，PCF 监听 7785，测试 N3 绑定回环：

```bash
XCN_QOS_PCF_URL=http://127.0.0.1:7785 \
XCN_FORWARDING_GNB1_ADDR=127.0.0.1 XCN_QOS_TEST_MSIN=0000000911 \
XCN_INTENT_TEST=1 XCN_INTENT_FAILURE_TEST=1 XCN_INTENT_CONTROL_PLANE_ONLY=1 \
  /tmp/intent-vonr-client -e info -c /tmp/intent-k8s-test.yaml dedicated-bearer-test
```

测试订阅者与会话均已清理；首次中断测试留下的专用订阅者也已单独删除，真实 UE 订阅未删除。
测试地址已发 Off 通知收尾。临时抓包进程与隔离容器停止，最终 K8s 核心网、gNB、UE 保持运行。

本机原始证据归档：`/tmp/epco-intent-live-evidence-20261012.tar.gz`，包括最终抓包、解析后的通知 JSONL、
NAS/NF 日志、接口矩阵、间隔监控快照、UE/主机 ping、部署信息及测试配置。
抓包解析后使用 `tests/helpers/verify-intent-notifications.py --resource-failure` 核对，结果为
`PASS: 52 notifications, exact order/values, no duplicates`。

混合标准 QoS 请求、资源不确定后的补偿恢复、IPv6-only 通知和真实终端/RAN 对接等现有边界
仍以 [实现说明](epco-user-intent.md) 为准，本次联调未将它们标为完成。
