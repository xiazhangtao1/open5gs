# xcn 名称调整与迁移

当前项目统一使用 `xcn`。这次调整覆盖代码、注释、构建目标、配置、服务、容器、Helm、监控、WebUI、数据库工具和文档，协议处理和数据面算法保持原有行为。

## 名称与路径

| 范围 | 原名称 | 当前名称 |
| --- | --- | --- |
| Meson 项目 | `open5gs` | `xcn` |
| 网元程序 | `open5gs-{amf,smf,upf,...}d` | `xcn-{amf,smf,upf,...}d` |
| 源码配置目录 | `configs/open5gs/` | `configs/xcn/` |
| 配置安装目录 | `/etc/open5gs/` | `/etc/xcn/` |
| 日志目录 | `/var/log/open5gs/` | `/var/log/xcn/` |
| systemd 服务及服务用户 | `open5gs-*` / `open5gs` | `xcn-*` / `xcn` |
| MongoDB 默认数据库 | `open5gs` | `xcn` |
| UPF 统计 socket | `/run/open5gs/upf-stats.sock` | `/run/xcn/upf-stats.sock` |
| 数据库工具 | `open5gs-dbctl` / `Open5GS.py:Open5GS` | `xcn-dbctl` / `xcn.py:xcn` |
| 环境变量前缀、版本宏 | `OPEN5GS_*` | `XCN_*` |
| WebUI 容器目录 | `/usr/src/open5gs/webui` | `/usr/src/xcn/webui` |

`ogs_*` 内部 API、`libogs*` 库名、`ogstun` 接口名和协议字段不含旧品牌名称，因此保留。网络地址、路由、字节序、队列归属及线程模型没有调整。

## 已部署环境

部署前先重新构建全部网元和 WebUI 镜像，并让监控镜像使用新的二进制文件与 socket 路径。Helm 中的数据库连接、初始化脚本、程序入口、挂载路径和监控配置已同步更新。自定义 values、脚本和 systemd overrides 也需要使用新名称；已有旧镜像不能与新模板混用。

数据库名称变化不会自动复制已有订阅者、账户或会话记录。维护窗口内先停止核心网和 WebUI 的写入，再备份原数据库，恢复到新库。例如，在可访问 MongoDB 且已安装 MongoDB Database Tools 的 Ubuntu 环境执行：

```bash
mongodump --uri='mongodb://127.0.0.1:27017/open5gs' --archive=xcn-before-rename.archive.gz --gzip
mongorestore --uri='mongodb://127.0.0.1:27017' --archive=xcn-before-rename.archive.gz --gzip \
  --nsInclude='open5gs.*' --nsFrom='open5gs.*' --nsTo='xcn.*'
```

恢复前确认目标 `xcn` 数据库为空，并按部署环境补充认证参数。验证新库的订阅者和账户记录后再启动新版本；原数据库及归档保留用于回退。非 Helm 部署的所有数据库消费者均需将连接切换到同一新库。

独立安装 WebUI 时，在当前源码目录执行 `sudo -E bash docs/assets/webui/install`；源码目录可通过 `XCN_SOURCE_DIR` 指定。此安装器从本地源码构建，不下载上游发行包。`docker/package/Dockerfile` 的构建上下文应为本地 xcn `.deb` 包所在目录。

## 保留项与检查边界

原作者版权署名、上游历史链接、第三方仓库 URL、Prometheus 依赖的 `open5gs` 分支和 OSS-Fuzz 注册项目名称均保留真实值。上游贡献协议、历史 PDF、截图、抓包等二进制资料保留原始内容，相关文件路径已改为 `xcn`。这些材料用于出处和历史参考，不代表当前运行时名称。

Git 远端地址、当前分支、当前工作区根目录以及未纳入版本管理的镜像归档不属于此次源码改名范围。仓库文档中的本机绝对路径已改为 `/path/to/xcn` 示例，实际命令需使用当前检出目录。

## 本次验证

- 在已有构建依赖的隔离容器中，以 `upf_memif=true` 完成全部网元、共享库和测试程序的编译及暂存安装。
- Meson `unit` 套件 10 组全部通过；涵盖核心库、加密、协议、UE 事件、快照以及 xcnctl。
- 17 个安装后的 `xcn-*d` 程序版本输出正常；NRF 实际启动并通过 HTTP/2 请求返回 `200` 和 `server: xcn v2.7.7`，随后正常退出。
- Helm lint、默认及 hostNetwork 渲染、31 项 Chart 回归测试通过，渲染产物不含旧运行时名称。
- WebUI 生产构建通过；候选镜像实际返回 HTTP 200 和 `xcn - Login` 页面。监控回归测试运行 56 项，通过并按原条件跳过 3 项。
- Python 文件语法、Shell 安装脚本、17 个 systemd 服务模板及 Debian 生成文件清单检查通过。

隔离业务集成测试另外通过 11 组，覆盖 SCTP、5G 注册、切片、4G 附着、VoLTE、CSFB、310014、切换、非 3GPP、上下文转移及错误分支。VoNR 套件在 AF 场景达到 150 秒时限；将 AF 场景单独运行 180 秒，改名前原提交与改名后版本均超时，属于本次对照中共有的限制，未计为通过。

上述自动化检查使用独立容器网络、临时 MongoDB 和测试 TUN。当前环境升级保留原 Helm 参数与登录密钥，只更新镜像及统计 socket 路径；数据库通过归档复制，原库保留用于回退。

## 当前环境升级结果（2026-10-26 / 2026-10-27 复核）

- `xcn` namespace 的 Helm release 已升级到 revision **200**，状态为 `deployed`。运行时、WebUI、监控和解码器镜像使用 `rename-xcn-1026` 标签。
- 原网络、端口、Service、资源、安全设置和登录密钥保留。UPF CPU 仍为 `44-46,116-117`，VPP CPU 仍为 `48-49,120`；N3/N6 两个 memif 接口均为 `admin-up connected`，远端接口标识已改为 xcn。
- 停止写入后将 `open5gs` 数据库归档复制到 `xcn`，复制时所有集合数量及哈希一致。保留 1,201 条订阅者、1 个账户和 10 条审计记录；原数据库未删除。
- 核心网、WebUI、监控及解码器均就绪，复核时新容器已连续运行约 5 小时、无重启。监控数据源无采集错误且均未过期，信令解码器就绪。
- 6 个真实 UE 均为 `connected`，恢复 6 个 `cmnet` PDU 会话，UE 地址与升级前一致：`10.45.0.3` 及 `10.45.0.101` 至 `10.45.0.105`。
- 每个 UE 通过 `oaitun_ue1` 向 `10.45.0.1` 复测 10 个 ping，合计 **60 发、60 收、零丢包**。此前首轮主 UE 的 5 个探测包有首包丢失（4/5），其余 UE 首轮为 5/5；后续复测未重现，不将首轮记录计为零丢包。
- WebUI 实际返回 HTTP 200，页面标题为 `xcn - Login`。测试结束后已清理隔离验证容器；gNB 中断重启遗留的失败 Pod 也已清理，运行模板恢复至升级前状态。

最终数据库归档位于本机 `/tmp/xcn-validation/mongodb-before-upgrade-final.archive.gz`，权限为 `600`；升级前 Helm 参数、清单和测试日志也保存在该目录。归档包含敏感数据，不纳入 Git。需要长期保留时应另行保存至合适的备份位置。
