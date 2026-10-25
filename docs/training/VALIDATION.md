# 培训文档验证记录

内容快照：2026-10-25。仓库实现基线：`4ccc714f5f7b39e685c39a9637e2497705ab2d19`。本次仅新增培训资料和生成/检查脚本，没有修改业务代码、Chart、VF、路由、NAT 或已部署 Release。

## 内容依据

- 服务器只读查询：CPU/NUMA/兄弟线程、内核隔离参数、网卡型号/驱动/链路、VF 数量、节点扩展资源、CPUSet 分配、Pod、镜像与工具版本。
- 运行态与仓库交叉核对：核心网 memif 模式、N3/UPF 与 VPP 接口地址、N3 VRF/代理 ARP、N6 地址与 NAT、VF 请求和 VPP CPU。当前活跃 PF 链路为 1Gbps；未将其表述为 10Gbps 实测链路。
- 自研基站和模拟 UE：以用户提供的三个函数与补充说明解释配置意图。缺少的分流调用链、下行会话匹配、全局出口初始化、失败回滚及多会话 LAN 设计均注明边界。
- Kubernetes、Docker、Helm、Harbor、SR-IOV 插件、Linux、iperf3、Kuboard、k9s 的官方参考链接列在讲义附录 D；命令结合本机版本核对。
- 已移除 VPP 抓包与 trace 教程，保留接口计数、FIB、邻居、NAT 状态等只读排障方法。

## 自动检查与实际运行

| 检查 | 方法 | 结果 |
| --- | --- | --- |
| 正文结构 | Markdown 解析 | 六章、四附录，15 张图片引用有效 |
| Shell 命令块 | 对 23 个 bash 块执行 `bash -n`，不执行其中的变更命令 | 通过 |
| Chart | `helm lint ./helm/xcn` | 通过；仅有 icon 推荐信息 |
| TUN 配置 | 离线渲染，检查无 VPP 容器与对应 VF 请求 | 通过，23 个对象 |
| memif NAT 配置 | 离线渲染，检查双 memif、2 个 VF、NAT 启用命令 | 通过，26 个对象 |
| memif 路由配置 | 离线渲染，检查双 memif、2 个 VF、无 NAT 启用命令 | 通过，26 个对象 |
| iperf2 2.1.5 | TCP/UDP 正向、反向，以及 TCP `--full-duplex` | 5 组通过 |
| iperf3 3.9 | TCP/UDP 正向、反向，以及 TCP `--bidir` | 5 组通过 |
| 原生图表 | Archify 3.2.0 showcase validate/deliver/check/browser-check | 15 张全部通过 |
| 打印衍生图 | Chromium 导出 SVG/PNG，检查节点文字边界 | 15 张无节点文字越界 |
| 离线图册 | Playwright 检查图片、本地链接及核心网图跳转 | 15 张图片、63 个链接正常，无页面脚本错误 |
| Word 目录与排版 | LibreOffice 实际打开 DOCX、更新目录，再导出 PDF | 33 页，目录与页码已生成 |
| Word 内容完整性 | 逐段比对 Markdown 正文/表格/代码与 DOCX XML | 通过，详情见 `validation/document.json` |
| 图片独立性 | 检查 DOCX 内部 media | 15 张图片均已嵌入 |
| 页面与视觉检查 | 全页缩略图，重点放大目录、拓扑、IP 表、命令和附录；PDF 文本边界检查 | 无页外文字、无替代字符；连续命令块不跨页 |

iperf 全部只在 `127.0.0.1` 使用临时端口，客户端每例 2 秒，`-b 1M`。双向为每个方向的目标速率。验证的是工具与方向参数能工作，不是 PF、VF、基站、UPF 或无线吞吐。服务端日志中的终止信息来自测试完成后关闭本次启动的进程。

## 可复核文件

- `validation/commands.json`：正文、Helm 和 10 组 iperf 结果。
- `validation/iperf/*.txt`：实际客户端/服务端命令与输出。
- `validation/diagrams.json`：15 张原生图的门禁状态及输入/HTML SHA-256。
- `validation/*.finalize.json`：对应的完整图表检查回执。
- `assets/print-export-check.json`：打印衍生图文字边界检查。
- `validation/document.json`：Word/PDF 一致性、图数、页数和页边界检查。
- `validation/index-browser.json`：离线图册链接与跳转检查；`validation/browser-cjk.json`：加载 Noto CJK 后的原生图浏览器复查。中文字体只加入临时 Fontconfig 环境，未修改主机字体配置。

`verify_document.py` 不替代人工阅读图意；本次同时检查了图上的方向、关键地址与正文报文表。原生 HTML 的源码来源仍绑定上述仓库基线；打印衍生图与 Word 的排版调整不改变数据路径语义。

## 未执行的操作

未安装或卸载业务 Release，未切换核心网 TUN/memif/NAT，未改 Linux/VPP 路由、VF 绑定、CPU 亲和性或交换机。未对当前网络进行高带宽灌包，也未声称完成商用终端或自研系统的端到端验收。

本轮未在服务器发现 Kuboard 实例，故讲义提供通用操作步骤，没有编造本机登录入口和截图。静态路由及 NAT 示例要求独立实验条件；它们的 `bash -n` 通过只证明 Shell 语法正确，不代表已经在真实拓扑执行。
