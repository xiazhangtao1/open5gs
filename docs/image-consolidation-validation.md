# 监控镜像合并与保留调试能力的镜像瘦身

本轮将 monitor 和 decoder 合并为同一发布镜像，runtime、WebUI、VPP、MongoDB
各自独立。当前 memif 部署的标准发布物为五种镜像，运行进程及容器继续独立。

## 构建行为

- `monitor/Dockerfile` 编译原生 trace reader 和 TShark 4.6.9，在统一 Python
  3.12 镜像中提供四种角色。Helm 显式启动 decoder，agent 命令保持原路径。
- `monitor/Dockerfile.decoder` 兼容旧构建入口，仅从统一镜像派生旧 decoder
  启动方式，不属于标准五镜像发布物；可用 `MONITOR_IMAGE` 指定统一镜像。
- runtime 保持 Ubuntu 22.04 和 `debugoptimized` 编译；仅复制 Meson 安装
  暂存产物，保留全部网元、xcnctl、共享库、freeDiameter 插件、配置及证书。
  Meson 的配置安装脚本在 `DESTDIR` 下会跳过配置，Dockerfile 因此显式复制
  生成的 xcn、TLS、hnet 和 freeDiameter 配置。完整调试符号不剥离。
- runtime 保留 tcpdump、iperf、iperf3、ping、ip、ss、bridge、tc、iptables、
  ip6tables、nc、taskset、top、ps、watch、openssl、Python 及 binutils；
  编译器、头文件和构建工具留在构建阶段。保留 setup、等待脚本和数据库工具。
- WebUI 保持 Node 19.9.0 / Debian 11，最终改用同版本 slim，复制应用、
  `.next`、静态图表和生产依赖；保留 curl、wget、file、binutils、procps、
  taskset、Python、openssl 及压缩工具。构建缓存留在构建阶段，现场 `.env`
  不进入镜像。等待程序保持原 2.9.0，并校验原文件 SHA-256。
  Debian 11 安全更新包已迁移到[官方归档源](https://archive.debian.org/debian-security/dists/bullseye-security/Release)，
  Dockerfile 只替换该源地址，继续校验 APT 签名；本轮保持既有运行环境，
  不包含 Node/操作系统的大版本升级。
- 构建上下文保留已检出的第三方源码，排除嵌套 Git 元数据；记录源版本和
  基础镜像摘要。VPP 和 MongoDB 沿用现网镜像，不重新打包。

## Helm 兼容性

`monitor.image` 是统一镜像。空的 `collectorImage`、`trace.agentImage`、
`trace.decoderImage` 字段继承它；已有显式 repository、tag、pullPolicy
仍按原角色生效。旧镜像路径的 decoder 同样使用兼容的绝对启动路径。
独立更新 collector/decoder 时不修改 core、5gc、WebUI、MongoDB 模板。

旧 values 中显式填写的 agent/decoder 镜像必须清空或更新到统一版本，
仅更新 Chart 默认值不会覆盖它们。本轮现场将四个角色统一到同一镜像，
保留原同 Pod 双容器布局、独立资源限制、CPU 绑定及权限。

## 验证记录

### 镜像体积

以下为本机 Docker 报告的解压后逻辑体积，单位 MiB；镜像之间的共享层不
在此表中去重。新发布标签为 `slim-1027`，仓库前缀为 `localhost:5000`。

| 镜像 | 优化前 | 优化后 | 变化 |
| --- | ---: | ---: | --- |
| runtime | 1455.61 | 256.64 | 减少 82.37% |
| WebUI | 1498.56 | 430.51 | 减少 71.27% |
| monitor | 135.89 | 294.19 | 含原 decoder，统一发布 |
| 独立 decoder | 294.31 | — | 不再单独发布 |
| VPP | 286.46 | 286.46 | 原镜像摘要不变 |
| MongoDB | 902.57 | 902.57 | 原镜像摘要不变 |

对标准发布的全部镜像执行 `docker save` 后统一 `gzip -1`：原六种镜像
1,737,294,726 字节（1656.81 MiB），新五种镜像 805,972,168 字节
（768.63 MiB），离线包减少 **53.61%**。这里包含未变更的 VPP 和 MongoDB，
并由 Docker 导出流程处理共享层。

统一镜像不合并进程。两个 agent、collector 和 decoder 仍各自启动，
agent/collector 不会因为镜像含 TShark 而自动启动解码进程。

构建使用空闲 CPU 36、37，最多两编译任务，Docker 构建限制 1.5 CPU /
4 GiB，使用主机网络。本轮网络故障通过仅作用于本次构建的代理绕过、
官方基础镜像层续传及 SHA-256 校验处理，没有重启生产 Docker 或更改
主机全局代理。基础镜像记录如下，Node 两项为上游 linux/amd64 manifest，
Python 项为本机既有基础镜像的 image ID：

| 基础镜像 | 摘要 |
| --- | --- |
| Ubuntu 22.04 | `ubuntu@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982` |
| Python 3.12 slim | `sha256:a33c10a97f11005523814c6eee013986119fe41f3122f444518117536675c8e1` |
| Node 19.9.0 bullseye | `sha256:46c72d31352c28fe0397b5b2772761a867f8b9ad11184768c9c027fab41baa44` |
| Node 19.9.0 bullseye slim | `sha256:63b5ca0f90dbaf64465a83d46276e133eab30410f642808d158bf98928811999` |

### 隔离与浏览器验证

- Helm 回归 35 项通过，新增真实渲染覆盖两种 decoder 布局、五镜像去重、
  角色权限与内存限制、旧覆盖配置、关闭信令，以及单独升级不重建核心网。
- 统一监控镜像内运行 56 项测试，55 项通过，CLI 等价测试因未指定
  `XCNCTL` 跳过；真实
  NGAP/NAS 夹具经 TShark 4.6.9 解码。另验证原生 reader 加载和四角色
  实际启动，monitor/decoder 非 root，两个 agent 保持原有权限。
- 新 runtime 构建产物的十组核心网单元测试全部通过。17 个网元和 57 个
  ELF 依赖闭包核验通过；网元及 libogs 保留 `.debug_info` 和 `.symtab`，
  18 份配置及 TLS/hnet/freeDiameter 配置完整。实际 tcpdump 抓取四个
  ICMP 包，iperf、iperf3 的 TCP/UDP 收发均通过。
- 独立网络命名空间、临时 MongoDB 和 TUN 内运行新镜像安装后的 11 个
  网元：专载优先级回归 54.36 秒、会话控制回归 36.09 秒、默认承载修改
  回归 115.51 秒，三阶段均返回 0，全部网元正常退出。覆盖 300 次专载
  生命周期和 60 次默认 QoS/AMBR 交替修改，测试不连接现场数据库或 UE。
- 最终 WebUI 镜像内的控制接口/权限、监控 JWT/缓存、走势、framed-route
  四组回归全部通过；运维命令、Node 19.9.0、`.next/BUILD_ID` 和等待程序
  摘要核验通过，镜像内没有现场 `.env`。
- Playwright 使用新 WebUI、独立账号数据库及现场只读监控缓存，47 项
  检查通过，覆盖真实登录、六用户/六会话、24 小时分钟走势、表单读取、
  搜索与键盘选择、深浅主题、390 像素窄屏，以及 NGAP 协议树展开/收起和
  HEX 显示。没有浏览器运行错误，没有提交核心网写请求。临时数据库和
  预览容器已删除。

本轮隔离协议测试使用发布构建；既有 sanitizer/OAI 兼容性记录仍见
[控制功能验证](webui-core-control.md#验证)。

### 现场对照与上线

优化前现场为 Helm revision 204。六终端保持十二路原速率 UDP 流量，
十分钟共发送并收到 18,000 个 ping，合并 P99 29.7 ms、最大 38.8 ms。
29 个持续采样核实真实上下行速率、网元资源、OOM 和 CFS 计数，
无观测错误；50 只读客户端完成 3204 个请求，零错误，P95 37.26 ms、
P99 134.98 ms，核心网采样频率未随浏览人数增加。

2026-10-27 16:46 升级到 revision 205，16:49 完成原六终端和十二路流量
恢复。IMSI 后缀 100–105，PSI=1、DNN=cmnet，IP 保持 `.3`、`.101`–`.105`；
上行目标分别为 0.5/1/1.5/2/2.5/3 Mbps，下行为 1/2/3/4/5/6 Mbps，实际
速率连续三次达到目标的至少 70% 后才进入十分钟观测。

现场核实五 Pod、十七个常规容器、包括 init 在内五种镜像。WebUI 和监控
密钥保持不变，MongoDB Pod 未重建；新 Pod 无容器重启。VPP/memif 配置、
资源和亲和性策略保持原值，CPU Manager 重建后分配的 VPP CPU 为
`48-49,120`，自身 CFS quota 为 `-1`。监控 CPU 36、37 及其 SMT 兄弟均不在
UPF/VPP 允许 CPU 内。现场新产生的 NGAP 信令返回 ready，协议树和完整
HEX 可读，`redacted=false`。

升级后 50 个只读客户端在 120.14 秒完成 3204 次请求，零错误；P95
49.31 ms、P99 68.08 ms。UPF/事件采样约 0.49 次/秒，UE/PDU 约
0.10 次/秒，没有随客户端数量增加；全部来源无错误，监控使用内存
47,886,336 字节（45.67 MiB），内存保护未暂停或淘汰批次。

两轮均为六 UE、十二路 UDP 流量，约 602 秒、29 次资源/实际速率采样。
UPF 字节口径与 iperf 应用载荷口径不同，以下速率统一采用 UPF 采样均值。

| 观测项 | 升级前 | 升级后 |
| --- | ---: | ---: |
| ping 收到/发送 | 18000/18000 | 18000/18000 |
| 合并 P99 | 29.7 ms | 29.7 ms |
| 最大 RTT | 38.8 ms | 40.4 ms |
| 上行速率 | 10.756 Mbps | 10.764 Mbps |
| 下行速率 | 21.511 Mbps | 21.511 Mbps |
| iperf 上行接收端丢失/总包数 | 0/656253 | 0/659893 |
| iperf 下行接收端丢失/总包数 | 0/1312500 | 0/1312499 |
| AMF/SMF/PCF/UPF/VPP 新增 OOM | 0 | 0 |
| 上述网元新增 CFS 节流 | 0 | 0 |

UDP 接收端从现有服务端和 UE 日志核对，过滤对应时间窗内十秒报告；上行
两轮分别为 360/362 段、下行各 360 段，边界区间数量略有不同，均为零丢包。
不能将这些应用载荷包数直接当作 UPF 字节统计的分母。

升级后的 RSS 范围及文件描述符数如下，均无异常增长或进程退出：

| 进程 | RSS 范围（MiB） | FD 数 |
| --- | ---: | ---: |
| AMF | 112.72–112.72 | 37 |
| SMF | 218.80–219.06 | 30 |
| PCF | 21.99–21.99 | 23 |
| UPF | 294.33–294.98 | 30 |
| VPP | 1593.61–1593.61 | 53 |

本轮当前演示负载下未发现可重复的吞吐、丢包或 P99 退化，保留 revision
205；最终复核所有容器就绪且无新增重启，原六终端和十二路灌包继续运行。
十分钟结果验证本轮发布，不代表满速吞吐或长期稳定性保证。

原始现场配置、镜像清单、回滚 values、离线包和测试日志保存在主机权限
0700 的 `/tmp/xcn-image-slim-1027`，不提交密钥或真实信令内容。旧镜像标签
保留；出现可重复的丢包增加超过 0.1 个百分点，或 P99 同时增加超过
20% 和 5 ms，以及 OOM/崩溃/功能失败时，使用保存配置回滚并恢复六用户流量。
