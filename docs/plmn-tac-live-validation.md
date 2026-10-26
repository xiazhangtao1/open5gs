# 三个 PLMN 与 TAC 0/1 上线实测

验证日期：2026-10-14。节点：`xnet-server`，主机地址 `10.2.0.119`。

## 核心网升级

将 `xcn/xcn` 从 Helm revision 164 升级到 165，默认配置来自提交
`a576e6b88`，包含移动 `46000`、联通 `46001`、电信 `46011`，
每个 PLMN 的 TAC 均为 `[0, 1]`，切片均为 SST `1`、SD `010101`。
同步配置 NRF serving，以及 AMF GUAMI、TAI、PLMN support。

升级前保存 Helm 参数、完整清单、历史版本和现有 gNB/UE 配置。
对比升级前清单和使用原参数渲染的候选清单，确认仅修改 `xcn-config`
中的 `nrf.yaml`、`amf.yaml.tmpl`，以及 5GC Deployment 的配置校验和。
保留原镜像、VPP/memif、SR-IOV、CPU、N3/N6 地址、用户意图功能及地址池参数。

```bash
helm upgrade xcn /path/to/xcn/helm/xcn -n xcn \
  --reuse-values --atomic --wait --timeout 10m
kubectl rollout restart deployment/xcn-core -n xcn
```

5GC 随配置校验和变更自动重建；另重启 NRF/SCP，使其加载新的服务 PLMN。
实际读取新 AMF 的 `/tmp/amf.yaml`，确认三个 PLMN 和全部六组 TAI 生效。
运行镜像保持为
`localhost:5000/xcn-runtime:epco-intent-notify-56e2b8d04-1013`。

## OAI 基站和终端实测

使用现有 OAI gNB 和 RFsim nrUE，临时切换 gNB 的 PLMN/TAC 和 UE IMSI。
保持原基站、UE 镜像和无线参数；每次停止旧实例后重新启动，确保实际加载配置。
移动、联通使用由现有电信测试用户复制的临时订阅，清除复制的静态地址，
由当前 SMF 动态池分配地址。

| PLMN | TAC | UE IMSI | UE IPv4 | 注册、鉴权、活动 PDU 会话 | 隧道 ping |
| --- | --- | --- | --- | --- | --- |
| `46000` | 1 | `460000000099901` | `10.45.1.2` | 通过 | 5/5 |
| `46001` | 1 | `460010000099901` | `10.45.1.3` | 通过 | 5/5 |
| `46011` | 1 | `460110000000100` | `10.45.0.3` | 通过 | 恢复后 5/5，持续复验 100/100 |

通过 AMF `/ue-info` 核对 `cm_state=connected`、`security.valid=1` 和实际
`location.nr_tai`；通过 SMF `/pdu-info` 核对 DNN `cmnet`、PSI `2`、
分配的 IPv4 和 `pdu_state=active`。数据测试绑定 UE 的 `oaitun_ue1`，
目标为 `10.2.0.119`，经过真实 gNB、UPF、VPP/memif 用户面。

电信首轮 5 包收到 4 包，没有将该轮记为零丢包通过。
恢复原配置后的 5 包复验全部回复，最终持续测试如下：

```bash
kubectl exec -n default deploy/nrue-oai-nr-ue -- \
  ping -I oaitun_ue1 -c 100 -i 0.2 -W 2 10.2.0.119
```

最终为 `100 packets transmitted, 100 received, 0% packet loss`。

当前 OAI gNB 对 TAC 0 的配置检查报错：

```text
tracking_area_code: 0 invalid value, authorized range: 1 65533
```

三个 TAC 0 的 OAI 场景均在基站启动阶段退出，尚未向 AMF 发起 NG Setup，
因此不能据此判断核心网拒绝 TAC 0，也不能将它们记为 OAI 接入通过。

## 真实核心网受控协议测试

复用已构建测试镜像中的 NGAP/NAS/GTP-U 辅助函数，在临时容器中编译接入冒烟
入口，连接本次升级后的实际 AMF、MongoDB、SMF、UPF 和 VPP。
关闭测试配置中全部内置核心网子进程；没有用隔离模拟核心替代现网核心。
临时受控 gNB 的 N3 地址为 `10.2.0.233/32`。

每组执行 NG Setup、身份识别、鉴权、NAS 安全模式、Registration Complete、
DNN `internet`/PSI `5` 的 PDU 会话建立、真实 GTP-U/ICMP 回包校验和会话释放。
除最多三次的邻居预热探测外，每组独立校验 10 个回包的 TEID、QFI、
内层 IPv4 源/目的地址、ICMP 协议号和 Echo Reply 类型，目标同为 `10.2.0.119`。

| PLMN | TAC | 实际 UE IMSI | UE IPv4 | 运行中状态核对 | GTP-U/ICMP |
| --- | --- | --- | --- | --- | --- |
| `46011` | 0 | `460110000908900` | `10.45.1.10` | 通过 | 10/10 |
| `46011` | 1 | `460110000908910` | `10.45.1.11` | 通过 | 10/10 |
| `46000` | 0 | `460000000908900` | `10.45.1.12` | 通过 | 10/10 |
| `46000` | 1 | `460000000908910` | `10.45.1.13` | 通过 | 10/10 |
| `46001` | 0 | `460010000908900` | `10.45.1.14` | 通过 | 10/10 |
| `46001` | 1 | `460010000908910` | `10.45.1.15` | 通过 | 10/10 |

全部六组退出码为 0。运行期间另采集 AMF/SMF 的实际状态，逐组核对 SUPI、
PLMN、TAC、有效安全上下文、活动 PDU 会话及分配地址。

临时测试工具移除了把合法 MNC `00` 误判为空值的两处检查，保留 MCC 检查；
这些改动仅位于临时容器，没有修改核心网二进制或仓库 C 代码。
首轮状态采集按字符串直接拼接 IMSI，未考虑工具 MSIN 参数的 BCD 字节顺序，
导致状态匹配未命中。修正采集脚本后完整重跑六组，以上表格取自重跑结果。

## 恢复与验证边界

核心网保留 revision 165 和新的三个 PLMN/TAC 配置。
恢复原 gNB ConfigMap、UE Secret 和副本数，原电信 UE 恢复为 `10.45.0.3`。
删除临时订阅，释放测试会话，临时用户均完成标准 NAS 关机注销。
AMF 在注销后仍保留空闲缓存，针对该缓存单独计划重启 AMF 一次；
UPF、VPP 保持运行。恢复原 gNB/UE 后，确认 AMF/SMF 仅剩原用户和一个
活动 PDU 会话，并重新执行上述 100 包测试。移除临时 N3 地址和测试容器。

OAI 测试覆盖 RFsim 终端的实际接入；受控测试覆盖六组 PLMN/TAC 在真实核心网
上的注册、鉴权、会话及数据路径。TAC 0 尚未与用户的另一台基站或商用终端
实际联调，不能把受控协议结果等同于所有终端的互通保证。

原始配置备份、测试脚本、逐组状态和日志保存在本机私有目录
`/tmp/xcn-live-plmn-miiqs2jv/`，鉴权资料和大日志没有提交到 Git。
