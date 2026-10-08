# N2 DRB 前传实现与验证

## 支持范围

当前实现覆盖单 UPF、intra5GS N2 切换，支持直连和间接 DRB 级上下行前传。
每个 PDU 会话最多 32 个 DRB，每个 DRB 的 UL/DL 方向分别使用独立 TEID。
会话级下行前传保持兼容。多 UPF、UPF 重选、跨 SMF 和漫游间接前传不在本次验收范围。
数据面实际验收使用 UDP N3/TUN N6；memif/VPP 和 session workers 未在本次环境验收。

## 协商与规则

- 源 gNB 在 HandoverRequiredTransfer 中宣告 directForwardingPathAvailability。
  直连可用时，SMF 将目标 ACK 中的前传端点写入 HandoverCommand，不创建 UPF 前传规则。
- 未宣告直连时，单 UPF 会话已有 N3 端点即可分配间接前传资源。
  源 gNB 使用 Command 返回的 UPF 端点，UPF 再转发到目标 ACK 中的端点。
  该决策表示资源支持，不代替实际 IP 路由、防火墙和基站能力检查。
- 每个 DRB/方向建立一个 Access→Access PDR/FAR；F-TEID Choose 不共享 Choose ID。
  每条 PFCP 消息最多 16 对规则，分批创建成功后才返回 HandoverCommand。
  会话规则容量为 96；普通会话修改排除临时前传规则。
- 正常 N3 下行端点与前传端点分别存储，防止切换后业务走错隧道。
- DRB PDCP 负载、长/短序号扩展、可选字段和 End Marker 保留，仅替换 GTP TEID。
- 取消、切换完成和失败回滚删除临时规则；事务 generation 防止旧响应修改新状态。
  删除失败或超时后保留本地记录，阻止再次切换复用未确认回收的资源；会话释放可清理。
  不自动宣称支持任意连续超短间隔切换。
- PFCP 事务资源不足时返回错误并允许对端重试，避免定时器池耗尽触发断言。

## 基站配合

直连要求源站实际拥有到目标站前传端点的通路，并宣告直连可用。
间接方式不要求直连字段，源站必须理解 Command 的 DRB 列表及上下行端点。
目标站需要按支持的方向在 ACK 中提供 DRB 前传隧道，接收 PDCP 数据及 End Marker。
基站间的 PDCP 序号与无线侧重排序由基站处理，核心网不解析或改写 PDCP 负载。

## 可重复的隔离验证

先配置独立网络命名空间、临时 MongoDB、ogstun（10.45.0.1/16 和
2001:db8:cafe::1/48），使用构建生成的 sample.yaml。禁止将完整测试套件直接指向业务数据库：
原有测试帮助函数会覆盖相同 IMSI 的订阅。

```bash
meson setup build --prefix=/usr -Dupf_memif=true
ninja -C build
meson test -C build --no-rebuild --suite unit --print-errorlogs
build/tests/handover/handover -c /tmp/forwarding-sample.yaml
XCN_FORWARDING_STRESS_SECONDS=1800 \
  build/tests/handover/handover -c /tmp/forwarding-sample.yaml 5gc-n2-test
```

N2 测试包含直连/间接、仅 DL、仅 UL、双向、多会话、32 DRB 边界和取消切换。
每个隧道发送 64 个包，交替携带 PSC，逐字节比较扩展头、PDCP 负载和目标 TEID。
压力测试每轮重新注册并完整释放 PDU 会话和 UE，至少 1000 次切换且持续指定时间。

另一个隔离命名空间中仅启动 UPF，执行标准 PFCP 故障测试：

```bash
build/src/upf/open5gs-upfd -c /tmp/forwarding-sample.yaml
XCN_FORWARDING_ISOLATED=1 python3 tests/handover/pfcp-forwarding-fault-test.py
```

故障测试覆盖重复 PDR ID、缺失 Apply Action、无效 CH F-TEID、部分创建后的删除、
重复删除和资源耗尽后的 PFCP 重试；各步验证原有 Access→Core 业务转发。
`XCN_FORWARDING_ISOLATED` 只是明确的执行前提标记，不会自动建立网络隔离。

## 内存检查的限制

```bash
meson setup build-san --prefix=/usr -Dbuildtype=debug \
  -Db_sanitize=address,undefined -Dupf_memif=true
ninja -C build-san
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  XCN_FORWARDING_HEADER_ONLY=1 build-san/tests/unit/unit gtp-message-test
ASAN_OPTIONS=detect_leaks=0:detect_odr_violation=0:use_sigaltstack=0 \
  build-san/tests/handover/handover -c /tmp/forwarding-san.yaml 5gc-n2-test
```

新增 GTP 头部测试通过严格 ASan/UBSan 检查。完整 N2 流程在上述选项下运行并正常退出，
未报告 ASan 内存错误。关闭泄漏检查是因为公共初始化存在常驻 talloc 分配；关闭 ODR
检查是因为 freeDiameter 插件有意导出同名全局变量；关闭 alternate signal stack 是为规避
本环境检测器在线程取消退出时的内部检查失败。
完整 UBSan 套件仍报告既有 NAS 对齐、零长度 memcpy 及 freeDiameter 告警，不能称为全库
UBSan 无告警，也不能据此宣称不存在任何泄漏。

## 当前部署的 N3 地址

当前 OAI gNB 使用主机 10.2.0.119:2152，UPF 使用专用 10.2.0.226:2152。
新增 networking.upf.n3.hostInterface 可在 hostNetwork/UDP 模式启动时幂等配置专用 /32 地址，
地址会保留以便重启。接口必须已启用，地址应由部署者确认可用。
这解决现有地址缺失导致的 UPF CrashLoop；不需要修改 OAI 基站的监听地址。

外部受控 gNB 复验使用 XCN_FORWARDING_MATRIX_ONLY=1 和独立
XCN_FORWARDING_TEST_MSIN，仅运行八项完整释放会话的前传矩阵；测试配置关闭所有本地 NF，
指向实际 AMF 和 MongoDB，并使用实际 PLMN/切片。执行前必须确认测试 IMSI 不存在。
切片 SD 要写成字符串（例如 `"010101"`），避免 YAML 工具将其解释成八进制再改写。
可用 XCN_FORWARDING_GNB1_ADDR / XCN_FORWARDING_GNB2_ADDR 指定测试基站 IPv4 地址。
本次临时在主机 lo 上配置 192.0.2.2/32、192.0.2.3/32；测试结束后已删除。
使用默认 127/8 地址连接实际 UPF 会受到 Kubernetes 的 localnet 防火墙限制，
表现为 UPF sendto 返回 EPERM；应使用独立测试地址，不需要放宽防火墙。

```bash
XCN_FORWARDING_MATRIX_ONLY=1 XCN_FORWARDING_TEST_MSIN=0000000904 \
  XCN_FORWARDING_GNB1_ADDR=192.0.2.2 XCN_FORWARDING_GNB2_ADDR=192.0.2.3 \
  build/tests/handover/handover -c /tmp/forwarding-live.yaml 5gc-n2-test
```

## 本次验收记录

2026-10-08，Ubuntu 主机上的独立 Docker 网络命名空间，以及当前 Kubernetes xcn。
普通构建使用 GCC 11，启用 memif 编译支持，但实际数据面使用 UDP/TUN。

| 验证项目 | 实际结果 |
|---|---|
| 完整普通构建 | 3967 个构建目标完成 |
| core / crypt / unit | 3 个测试套件通过 |
| EPC X2、EPC S1、5GC Xn、5GC N2 回归 | 全部通过 |
| N2 前传和取消矩阵 | 17 项通过，包含 32 DRB 双向和取消后普通业务 |
| UPF PFCP 故障注入 | 100 轮回滚，300 次拒绝，601 个 GTP-U 包校验通过；资源不足后重试恢复 |
| 检测器 | 新增头部测试严格 ASan/UBSan 通过；完整 N2 ASan 结果及既有 UBSan 告警见上文 |
| 部署后受控 gNB 复验 | 八项通过，每项两个 PDU 会话、往返切换；包含 32 DRB 双向，共校验 35,840 个前传 G-PDU 和 560 个 End Marker |

部署镜像为 `localhost:5000/xcn-runtime:forwarding-drbuldl-1008`，registry digest：
`sha256:4c196832cff8c9ba58a23068c61a2c1dad1f3c61680a50dcf440b6490dbb346c`。
Helm xcn revision 150；5gc 8/8、core 2/2、webui 1/1，更新后的 Pod 未重启。
实际受控基站订阅 IMSI 460110000009040 在测试前确认不存在，测试后查询数量为 0。
抓包同时记录了源站→目标站，以及源站→10.2.0.226 UPF→目标站两种路径。

日志和抓包保存在本机 `/tmp/xcn-forwarding-results/`，未加入 Git。
32 DRB 用受控 NGAP/GTP-U 基站验证核心网的边界处理；无线侧 PDCP 重排序和真实
OAI 双站切换未作为本次测试结果，不应将受控基站测试称为完整无线互操作认证。

OAI gNB/UE 重连后，IMSI 460110000000100 注册完成，UE 获得 10.45.0.3。
UE 经 oaitun_ue1 到网关 10.45.0.1 的 ping 为 20/20，到 8.8.8.8 为 10/10，均无丢包。
OAI 使用现有镜像，未修改其源码；只进行了实例重启。

最终压力复验持续 1800.9 秒，完成 2458 次切换（1229 轮，每轮间接后直连），
校验 1,258,496 个前传 G-PDU 和 19,664 个 End Marker；测试失败数为 0，退出码为 0。
这是功能与生命周期耐久验证，不是线速吞吐或无限并发验证。
前一轮发现测试断言错误地要求不同 IP 上的 TEID 数值也必须不同，修正为比较完整端点，
随后重新运行上述完整 30 分钟验收；最终结果不使用前一轮的失败记录作为通过依据。
