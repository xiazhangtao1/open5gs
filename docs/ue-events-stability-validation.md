# UE 事件历史与核心网稳定性验证

本轮验证日期为 2026-10-21，业务代码基线为 `3cfac8f99`。新增的是可重复执行的测试和 SMF 内存配置修正，没有修改注册、会话或切换信令的处理路径。

## 测试入口

```bash
ninja -C build tests/unit/ue-events-soak-test tests/registration/registration
meson test -C build --no-rebuild --print-errorlogs ue-events-soak
meson test -C build --no-rebuild --repeat 10 --print-errorlogs \
  amf-location smf-events ue-events xcnctl-events xcnctl-inspect
```

`ue-events-soak` 默认模拟 4,096 个历史用户，写入 6,520,000 条记录，覆盖：

- 默认 65,536 条及最大 262,144 条容量，多次淘汰后保留数恰好等于容量。
- 并发写入和 JSON 查询，游标顺序及每次最多 1,000 条的限制。
- 匿名接入事件与 SUPI 绑定，以及单个 SUPI 的 PSI 2、PSI 5 共享 512 条历史上限。
- glibc 支持 `mallinfo2()` 时，检查充分写入后的活跃堆占用保持在允许的查询波动内；ASan 构建依靠其独立的泄漏和越界检测。

长期协议测试需要独立网络、独立 MongoDB 和可用的 TUN/SCTP。测试程序会启动自己的 NF，并修改测试用户数据，不能与现网共用数据库、地址或端口。

使用构建生成的 `build/configs/sample.yaml` 副本，设置 `global.max.ue: 1024`，确保 AMF/SMF 的事件记录开启且容量为 `65536`、单用户为 `512`，并为该测试网络建立 `ogstun`、`10.45.0.1/16` 和 `2001:db8:cafe::1/48`。在已经隔离的测试环境内运行：

```bash
python3 tests/registration/ue-lifecycle-soak.py \
  --binary build/tests/registration/registration \
  --config /tmp/xcn-soak-sample.yaml \
  --output /tmp/xcn-soak-30m \
  --seconds 1800
```

默认按 16、64、128、256、512、1,024 UE 递增，再重复 1,024 UE。`--ue-count 64` 可固定用户数；实际人数受测试配置上限约束。每轮执行注册、认证、安全模式、PDU Session 建立、GTP-U 业务探测、PDU Session 释放和注销，同一组 NF 在循环间保持运行。

脚本检查 NF PID 和启动标识不变、事件有序且采集无缺口、会话完成事件发生于 N2/N4 成功之后且不重复，并检查每轮清理后的 RAN、AMF Session、SMF UE、Bearer、PFCP Session 计数归零。AMF 保留的空闲上下文单独分页核对身份唯一和数量上限，不把 CM-IDLE 误判为 AMF 上下文已删除。

输出目录必须不存在；测试会生成 `summary.json` 或 `failure.json`，以及资源采样和协议日志。只有 `summary.json` 的 `status` 为 `passed` 才表示通过。日志记录的是测试证据，不属于核心网事件历史存储。

## 提前触及资源池页面

普通启动的 RSS 明显低于已分配堆空间，长时间信令循环会逐渐触及固定池的更多页面。可增加一轮 glibc 分配填充测试，尽早验证内存限额：

```bash
MALLOC_PERTURB_=90 python3 tests/registration/ue-lifecycle-soak.py \
  --binary build/tests/registration/registration \
  --config /tmp/xcn-soak-sample.yaml \
  --output /tmp/xcn-soak-warm \
  --seconds 300 --ue-count 1024
```

该测试需要给隔离环境预留至少约 8 GiB 普通内存；本轮隔离容器限额为 12 GiB。测试时不启用宿主网络，不对现网批量注入用户。

## 本轮结果与资源修正

- 5 组事件、位置和 CLI 回归各重复 10 次，50/50 通过。
- 注册、会话、N2 切换成功/失败/取消、Xn Path Switch 场景重复 5 轮通过。
- 默认及最大历史容量原生压力测试通过，ASan/UBSan 另检查 CLI、事件存储、AMF/SMF 元数据、URI 所有权及完成事件关联。
- 满池专项连续 3 轮，3,072 个 UE 生命周期通过。
- 全池触页专项运行 332 秒、9 轮、9,216 个 UE 生命周期通过。末段清理时 RSS 约为 AMF 1,686 MiB、SMF 1,649 MiB、UPF 1,415 MiB；AMF/UPF 后几轮基本不变，SMF 末三轮波动约 2 MiB。
- 连续协议测试运行满 1,800 秒，56 轮、53,200 个 UE 生命周期通过。注册完成、会话建立完成、会话删除计数均为 53,200；采集 AMF 2,504,496 条、SMF 2,340,800 条事件，采集无缺口。所有 NF PID/启动标识保持不变；每轮清理时会话计数归零，AMF 保留上下文维持 1,024 上限。AMF/SMF 历史分别淘汰 2,438,960 / 2,275,264 条，未持续累积。
- 普通启动长测末段 RSS 为 AMF 823 MiB、SMF 583 MiB、UPF 436 MiB，尚在逐步触及已分配池页面。全池触页测试的较高且稳定的 RSS 更适合评估内存限额。FD/线程数未出现随循环持续增加。
- 最终修改的完整注册套件及 N2/Xn 回归通过；额外校验了 81 条不重复的会话完成事件，均发生于 N2/N4 成功之后。
- 当前部署的 UE/gNB 详情、速率 watch、附带位置的速率 watch、事件 watch 均已复测。事件 watch 的 75 次输出共包含 55 条不同事件，没有重复输出同一事件。验收期间容器重启计数不变、内存 `failcnt=0`、`oom_kill=0`。
- 核心网调整资源后，gNB 自动恢复 NG 连接，测试 UE 需重启后重新注册。重连后的第一次探测为 99/100，丢失序号 1，具体环节尚未定位；保留该结果后，在接入稳定阶段复测为 100/100。不能将本轮报告解读为所有情况下网络首包零丢失。
- `show rate --detail` 当前返回明确的参数错误（退出码 2），该选项用于 UE/gNB 详情。有效的速率循环查询为 `xcnctl show rate --level bearer --active-only --watch`，需要位置时再加 `--with-location`。

原配置 SMF `request=256Mi`、`limit=512Mi` 无法覆盖 1,024 UE 配置下固定 PFCP/SBI 资源池逐步驻留后的需求。实际分配器统计中，SMF 活跃堆约 1.6 GiB，UPF 约 1.4 GiB，AMF 约 1.6 GiB；这与刚启动时看到的低 RSS 不同。

因此将 SMF 默认值及当前 Helm release 调整为 `request=1Gi`、`limit=4Gi`。AMF 和 UPF 原本的 `limit=4Gi` 保持不变。渲染对比确认仅 SMF 的两项内存值变化；当前 release 为 revision 178。修改容量、会话/QoS 数、诊断容量或数据面实现后，需要重新测量，不能把本轮数值当作所有规模下的绝对最低内存。

## 满池行为及验证边界

UE 注销后，当前 AMF 可以保留空闲 UE 上下文。配置中的 `global.max.ue` 限制这些上下文的总数，不能仅按当前连接数理解容量。

首轮压测用全新的未知 GUTI 重复接入，在保留上下文占满 1,024 个槽位后触发过载释放。原临时测试驱动仍按身份流程继续，随后断言失败；NF 本身没有崩溃或 OOM。正式循环保留 UE 上一轮收到的 GUTI，并在池满时额外用未知 GUTI 发起请求，断言收到 `control_processing_overload` 的 UEContextReleaseCommand、完成释放，再验证后续已知 UE 能继续接入。

事件历史的有界测试不等于无限 UE 容量，也不等于已完成全部 3GPP 协议一致性或全部故障注入验证。已覆盖的负面场景包括过载释放、认证/会话拒绝、切换失败和取消；多节点故障、长时间掉电、持续数据库故障等仍需专门测试。

独立全进程 LeakSanitizer 退出检查另外观察到每进程 96 字节的 talloc 跟踪残留及 SMF 的少量 freeDiameter 退出残留；这项检查不记为零泄漏通过。它们与容量受限的事件历史不同；上述模块级 ASan/UBSan 检查通过，长期资源占用以实际采样结果判断。

本机详细证据保存在 `/tmp/xcn-reliability-20261021/`。测试脚本和结论随仓库提交，临时日志位于 `/tmp`，需长期保留时应另行归档。
