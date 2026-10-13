# 算力服务迁移联调记录

日期：2026-10-13。

新服务器通知链路、正常网络下的大屏显示和真实 UE 用户面访问均通过测试。
仍有两项算力端问题：OpenAPI 定义与实际接口不一致；Chart.js CDN 不可达时会阻断大屏轮询。
本次未修改算力服务或终端实现。

## 地址与核心网变更

| 项目 | 当前值 |
| --- | --- |
| 通知接口 | `http://10.2.0.102:8000/api/v1/intent/notify` |
| 大屏 | `http://10.2.0.102:8000/monitor/index.html` |
| 监控数据 | `http://10.2.0.102:8000/api/monitor` |
| 核心网代码提交 | `56e2b8d04` |
| Helm release | `xcn`，namespace `xcn`，revision 164 |
| Runtime 镜像 | `localhost:5000/xcn-runtime:epco-intent-notify-56e2b8d04-1013` |
| 应用 PLMN | MCC `460`、两位 MNC `00` |

更新了 Helm 默认值、默认 SMF YAML、SMF 内置默认通知地址及实现文档。
实际部署显式设置 `networking.smf.userIntent.notifyUri` 为新地址，复用原网络和模型 QoS 配置。
运行中的 SMF 与测试构建 SHA-256 一致：
`28565fdcdcc3ce770e88ac8b925dc2ca83ab327d0f457019ab9247c5e0a9183a`。

ePCO 继续使用六字节内容：三字节 PLMN 加三个业务字段。
例如 `7b 00 0a 80 ff 10 06 64 f0 00 01 02 02` 表示 On、人脸识别、GPU 中档。
通知仍使用 h2c prior knowledge，无 TLS、无鉴权，单次超时一秒、不重试。

## 测试结果

| 检查 | 结果 |
| --- | --- |
| 核心网编译、unit、Helm lint/默认配置渲染 | 通过，默认通知地址为新服务器 |
| 完整通知契约矩阵 | 34/34 通过：19 次 204、15 次 400；h2c、空成功响应体及错误 JSON 符合约定 |
| 非法 `ueIp` 专项 | 15/15 返回 400，覆盖非法 IPv4/IPv6、空值及错误类型；大屏业务状态未改变 |
| 正常浏览器显示 | 8/8 场景通过；记录 29 次正常轮询，无脚本异常或失败请求 |
| LLM 无专载 | 不携带 `fiveQi` 时，接口及页面显示默认承载 |
| LLM 保留专载 | 携带 `fiveQi=2/3` 时，显示对应已有专载 |
| 自定义 5QI | 人脸 5QI1、物体 5QI4、LLM 保留 5QI4，3/3 接口及页面检查通过 |
| 实际核心网 NAS/QoS 链路 | 软件 UE 发送六字节 ePCO，完整策略生命周期通过；SMF 日志确认 `length:6` |
| 实际核心网算力通知 | 抓包解析 52 条通知，全部返回 204 且响应体为空，字段、顺序和去重正确 |
| 新服务器通知响应耗时 | 实际核心网抓包约 3.09～6.57 ms，包含网络往返 |
| 真实 UE 用户面 | 强制绑定 `oaitun_ue1`，从 UE `10.45.0.3` 访问新服务器 `/api/monitor`，HTTP 200 |
| 真实 UE ping | 重新注册后首轮 6/8，稳定复测 8/8，目标 `10.2.0.102` |

52 条实际核心网通知包含 50 条 QoS SUCCESS 和 2 条预期拒绝用例的 FAILURE。
HTTP 204 表示通知被接收，不将这两条 QoS 失败改写为成功。
NAS 联调使用软件模拟 UE；真实 OAI UE 测试单独验证用户面访问，未测试图片推理或独立 DRB。

## 算力团队待处理问题

### OpenAPI 契约不完整

访问 `http://10.2.0.102:8000/openapi.json`，通知 POST 的 `responses` 只声明 200，
且没有 `requestBody` schema。接口文字描述声明 204/400/500，实测成功返回 204、校验失败返回 400。
建议补齐 JSON 字段定义及 204/400/500 响应定义，便于接口文档和生成客户端保持一致。

### CDN 故障导致大屏停止轮询

页面引用 `https://cdn.jsdelivr.net/npm/chart.js`。
`app.js` 在 DOMContentLoaded 中先调用 `initCharts()`，然后才发起和注册监控轮询。

本次仅在测试浏览器中阻断该 CDN 请求，未改服务文件。6.5 秒观测结果为：

```text
ReferenceError: Chart is not defined
/api/monitor 轮询次数：0
页面：未识别 / 未配置
```

正常网络下的浏览器用例全部通过；上述结果是 CDN 不可达的故障注入结果。
该问题此前已记录，迁移后的页面脚本仍可复现。建议将 Chart.js 随服务部署，
并让图表初始化失败时仍能启动监控轮询。

## 收尾及证据

核心网 9/9 就绪，测试订阅者 `imsi-460110000009036` 已删除。
直接通知和 NAS 联调测试均已发送 Off 通知收尾，大屏恢复默认承载。
隔离构建/测试容器已停止，K8s 核心网和真实 UE 保持运行。

原始证据归档：`/tmp/epco-intent-server-migration-evidence-20261013.tar.gz`。
内容包括接口矩阵、浏览器逐次快照、CDN 故障注入、六字节 NAS 联调日志、h2c 抓包及解析结果、
实际 SMF/Helm 配置、真实 UE HTTP 与 ping 记录、测试清理记录。
