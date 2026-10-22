# XCN 核心网产品说明书

**基于 Open5GS 的 5G SA 核心网系统｜投标技术说明版**

| 文档项目 | 内容 |
| --- | --- |
| 文档编号 | XCN-PM-001 |
| 文档版本 | V1.0 |
| 编制日期 | 2026 年 10 月 22 日 |
| 产品名称 | XCN 核心网系统 |
| 产品形态 | 核心网软件及容器化部署包；配套服务器以供货清单为准 |
| 软件基线 | Open5GS 2.7.7，XCN Helm Chart 0.1.0，仓库提交 45b6cb86c |
| 投标人／制造商 | 【投标人填写法定名称】 |
| 项目名称／招标编号 | 【投标人填写】 |
| 编制／审核／批准 | 【责任人署名后签发】 |

> ⚠ **安全提示**：设备供电、接地和机架安装由合格人员实施；升级、回滚、恢复及卸载可能中断终端业务，操作前应备份并落实维护窗口。订阅鉴权密钥、管理口令、私钥及数据库备份应受控保存。

**阅读说明**：本说明书按前言与文档控制、安全须知、产品概述、安装与部署、故障排查、附录六部分编制。适用于当前 XCN 5G SA 交付形态。文中 IP 地址、PLMN、切片及命令参数为明确标识的配置示例，实施时以项目批准的网络规划和交付清单为准。

**投标使用说明**：标注“【投标人填写】”或“待项目确认”的项目，应在提交标书前补齐并审核。吞吐量、并发能力、时延、可用性、硬件环境指标及认证材料，应采用对应交付配置的有效报告；本说明书不以配置上限或历史功能测试替代这些证明材料。

## 目录

- 1. 前言与文档控制
- 2. ⚠ 安全须知
- 3. 产品概述
- 4. 安装、部署与操作指南
- 5. 故障排查指南
- 6. 附录

## 1. 前言与文档控制

### 1.1 文档用途

本说明书描述 XCN 核心网的系统组成、接口、部署条件、基本操作及维护方法，供采购评审、集成交付、开通验收和日常运维使用。面向网络规划人员、系统集成人员、核心网维护人员及具有 Linux、Kubernetes 基础的管理员。

XCN 通过标准化的 N2、N3 接口与 5G 基站连接，通过 N6 接口接入企业数据网或业务网络，提供用户注册与鉴权、会话管理、用户面转发、策略控制和订阅管理等功能。

### 1.2 版本记录

| 版本 | 修订日期 | 修订内容摘要 | 编制／审核 |
| --- | --- | --- | --- |
| V1.0 | 2026-10-22 | 首版；根据当前 XCN 源码、Chart、运行配置及已有验证记录编制六部分说明书 | 编制：【投标人填写】；审核：【投标人填写】 |

本文件的“V1.0”为说明书版本，与 Open5GS 软件版本、Chart 版本和项目发布版本分别管理。交付升级后，应同步核对本说明书中的镜像、接口、配置和命令。

### 1.3 适用范围与版本基线

| 项目 | 本说明书适用范围 |
| --- | --- |
| 核心网版本 | 当前分支 open5gs-basev277-develop，编制基线提交 45b6cb86c，Open5GS 基础版本 2.7.7 |
| 部署包 | helm/xcn，Chart 版本 0.1.0，appVersion 为 2.7.7 |
| 部署方式 | Kubernetes＋Helm；当前 Chart 为单实例 5GC 拓扑 |
| 用户面模式 | UDP/TUN 模式；可选 VPP/memif＋DPDK 双 VF 模式 |
| 加速组件 | 当前 VPP 镜像版本 26.06；运行镜像需包含匹配的 libmemif |
| 操作系统参考 | 核心网运行镜像基于 Ubuntu 22.04；已核对宿主环境为 Ubuntu 22.04.3 LTS、x86_64 |
| 平台参考 | 已核对环境为 Kubernetes v1.26.8、containerd 1.6.32；编制机 Helm v3.17.3。该记录用于复现，项目平台版本应单独选型、验证并冻结 |
| 基站适配 | 具备 5G SA N2/NGAP、N3/GTP-U 能力且配置匹配的 gNB；已有 OAI gNB／RFsim nrUE 联调记录 |
| 基站型号与版本 | OAI 示例不构成所有商用基站的互通保证；商用型号、软件版本、频段及测试结果填写于项目适配清单 |
| 服务器／网卡硬件 | 无固定专用硬件型号；以配套服务器、网卡、固件和供货清单为准。加速模式额外要求合格的 SR-IOV／VFIO／大页／CPU 分配条件 |

**范围边界**：仓库保留 Open5GS 的 EPC 相关源代码，但当前 helm/xcn 不部署 MME、HSS、SGW 等 EPC 网元；本说明书不据此承诺当前部署已提供 4G EPC／NSA 业务。IMS、语音业务平台、基站射频单元、SIM 制卡设备及外部计费系统不在默认部署包中。

### 1.4 术语与缩略语

| 缩略语 | 中文含义 | 在本系统中的用途 |
| --- | --- | --- |
| XCN／CN | 本产品名称／核心网 | 承担核心网控制与数据转发功能 |
| 5GC／SA | 5G 核心网／独立组网 | 当前部署的核心业务架构 |
| EPC／NSA | 演进分组核心网／非独立组网 | 与当前默认 5GC 部署范围区分 |
| UE／gNB | 用户终端／5G 基站 | 核心网的接入对象及接入网对端 |
| AMF | 接入与移动性管理功能 | 注册、接入控制、移动性、NAS 信令 |
| SMF／UPF | 会话管理功能／用户面功能 | PDU 会话控制／业务数据转发 |
| AUSF／UDM／UDR | 鉴权服务／统一数据管理／统一数据存储功能 | 用户认证与订阅数据服务 |
| NRF／SCP | 网元注册功能／服务通信代理 | 网元服务发现与 SBI 通信转发 |
| PCF／NSSF | 策略控制功能／网络切片选择功能 | 策略控制与切片选择 |
| SBI／SBA | 服务化接口／服务化架构 | 控制面网元之间的服务调用方式 |
| NAS／NGAP | 非接入层信令／NG 应用协议 | UE－核心网信令／gNB－AMF 信令 |
| SCTP／GTP-U／PFCP | 流控制传输协议／用户面隧道协议／分组转发控制协议 | 分别承载 N2、N3、N4 通信 |
| DN／DNN | 数据网络／数据网络名称 | 业务出口网络／终端申请的数据网标识 |
| PLMN／MCC／MNC | 公共陆地移动网／移动国家码／移动网络码 | 网络与运营商身份；MNC 位数及前导零必须保留 |
| TAC／TAI | 跟踪区码／跟踪区标识 | 终端移动性管理区域 |
| S-NSSAI／SST／SD | 单一网络切片选择辅助信息／切片服务类型／切片区分符 | 标识用户使用的网络切片 |
| SUPI／SUCI／IMSI | 永久用户标识／隐藏用户标识／国际移动用户识别码 | 用户身份及隐私保护相关标识 |
| PDU／PSI | 协议数据单元／PDU 会话标识 | 数据连接及其会话编号 |
| QoS／QFI／5QI | 服务质量／QoS 流标识／5G QoS 标识 | 标识与控制不同业务流；QFI 不等同于 5QI |
| AMBR／GBR／MBR | 聚合最大比特率／保证比特率／最大比特率 | 订阅及 QoS 策略参数，不等同于实测吞吐量 |
| PDR／FAR／QER／URR | 包检测／转发动作／QoS 执行／用量报告规则 | UPF 的检测、转发、QoS 与用量规则 |
| TEID／SEID | 隧道端点标识／PFCP 会话端点标识 | 定位 N3 隧道及 N4 会话 |
| NCI／NR CGI／PCI | NR 小区标识／NR 全球小区标识／物理小区标识 | NCI 为 36 位；NR CGI 含 PLMN；CLI 的 CELL-ID 不是 PCI |
| TUN／NAT／MTU | 虚拟三层接口／网络地址转换／最大传输单元 | 兼容用户面、地址转换及路径报文长度 |
| VPP／DPDK／memif | 向量分组处理／数据平面开发套件／共享内存接口 | 可选加速路径的报文处理和收发组件 |
| SR-IOV／VF／VFIO | 单根 I/O 虚拟化／虚拟功能／用户态设备访问框架 | 将合格的网卡虚拟功能分配给 VPP |
| Pod／PVC／Helm | 容器组／持久卷声明／Kubernetes 包管理工具 | 网元承载、数据库持久化及发布管理 |
| BBU／RRU／AAU | 基带处理单元／射频拉远单元／有源天线单元 | 属于无线接入网硬件，非本核心网软件组成 |
| VSWR／SINR／MIMO | 电压驻波比／信号与干扰噪声比／多输入多输出 | 属于无线侧测量或能力指标，按基站／终端规格确认 |

### 1.5 参考资料与依据

本说明书中的实现与默认值优先依据本仓库对应版本。规范编号用于说明相关技术领域，引用规范不代表通过该规范全部条款的一致性认证；项目合同引用时，应约定适用 Release、版本和测试范围。

| 编号 | 资料 | 使用范围 |
| --- | --- | --- |
| R01 | helm/xcn/Chart.yaml、values.yaml、templates/ | 版本、拓扑、资源、接口及默认配置 |
| R02 | helm/xcn/README.md；docker/runtime/setup.sh | 部署、TUN／memif、CLI 与路由行为 |
| R03 | docs/plmn-tac-live-validation.md | PLMN／TAC 功能验证范围 |
| R04 | docs/ue-ipv4-validation.md | IPv4 子网、动态池及双模式验证 |
| R05 | docs/webui-framed-routes.md；docs/framed-routes-live-validation.md | 订阅后路由及真实会话验证边界 |
| R06 | docs/ue-events-stability-validation.md | 事件历史、资源与稳定性专项记录 |
| R07 | docs/epco-user-intent.md | 可选用户意图扩展及当前限制 |
| R08 | 项目网络规划、地址与 VLAN 表、基站对接规范、供货 BOM、验收大纲 | 【投标人补齐实际文件编号和版本】 |

相关国际技术规范参考：

- [3GPP TS 23.501：5G 系统架构](https://portal.3gpp.org/desktopmodules/Specifications/SpecificationDetails.aspx?specificationId=3144)，用于理解网元与参考点。
- [3GPP 5GS 规范目录，含 TS 23.502 流程规范](https://portal.3gpp.org/Specifications.aspx?WiUid=740005&q=1)，用于对照注册、会话及移动性流程。
- [3GPP TS 38.413：NGAP](https://www.etsi.org/deliver/etsi_ts/138400_138499/138413/17.03.00_60/ts_138413v170300p.pdf)，用于 N2 信令对接。
- [3GPP TS 29.244：控制面与用户面接口](https://www.etsi.org/deliver/etsi_ts/129200_129299/129244/19.05.00_60/ts_129244v190500p.pdf)，用于 PFCP／N4 对接。
- [3GPP TS 29.281：用户面隧道协议](https://www.etsi.org/deliver/etsi_ts/129200_129299/129281/17.05.00_60/ts_129281v170500p.pdf)，用于 GTP-U 协议对接；项目对接需选择双方约定的版本。
- [3GPP TS 33.501：5G 安全架构与过程](https://portal.etsi.org/webapp/workprogram/Report_WorkItem.asp?WKI_ID=79149)，用于安全设计对照。

国家标准、行业标准、等保及项目专项要求：【投标人填写标准编号、版本、适用条款和对应证明材料】。尚未确认的标准或认证不得列为已满足。

## 2. ⚠ 安全须知

### 2.1 警示标识与操作责任

| 标识 | 含义 | 处理要求 |
| --- | --- | --- |
| ⚠ 危险 | 可能涉及触电、设备倾倒、激光或其他人身伤害 | 立即停止不符合条件的操作，由合格人员处理 |
| ⚠ 警告 | 可能导致业务中断、数据丢失或安全凭据泄露 | 备份、审核操作范围，按批准的维护流程执行 |
| 注意 | 可能导致配置错误、性能下降或诊断误判 | 核对前提条件、参数与验证结果 |

### 2.2 电气、接地与防雷安全

XCN 软件随服务器运行，电压、功耗、接地电阻和浪涌保护要求采用配套设备原厂说明及现场批准的电气设计。不得将未经确认的服务器指标写为 XCN 固有规格。

1. 上架前检查保护接地、配电容量、插座、PDU、UPS 和防雷设施；保护接地应先接后拆。
2. 拆机、移动及更换非热插拔部件前，正常停止业务和操作系统，再按原厂要求断开全部电源输入；双电源设备不得只断开一路即视为断电。
3. 电源接线、绝缘测试及电压测量由具备资质的人员实施。不得对运行中的电子设备或已连接设备擅自进行绝缘耐压测试。
4. 电源冗余、UPS 续航及雷击防护属于配套硬件和机房工程验收内容，应保留测试记录。

### 2.3 射频与光口安全

XCN 核心网软件不包含无线发射机、天线和射频功放，不定义天线安全距离或辐射功率限值。与 gNB、RRU、AAU 联合部署时，射频禁区、安全距离、功率设置及现场标识按对应无线设备文件和批准的现场规范执行。

采用光纤连接的服务器或交换机，应检查光模块兼容性及激光安全标识。不得直视已上电光口；未使用光口和光纤端面应加防尘保护。

### 2.4 机械、安装与环境安全

服务器上架应核对机柜深度、承重、导轨、固定螺钉及机柜防倾倒措施；搬运人数、吊装器具和个人防护用品按设备重量及现场要求配置。核心网服务器安装不包含塔上作业，涉及塔上无线设备时应另行执行登高作业规范。

保持冷／热通道及风道畅通，不遮挡散热口；避免凝露、进水、导电粉尘和易燃物。防护等级、温湿度、海拔和噪声限值按供货硬件规格执行。UPS 电池、含电池部件及其他化学品交由指定渠道回收，不擅自拆解或混入普通垃圾。

### 2.5 网络、数据与软件操作安全

| 事项 | 要求与当前实现注意点 |
| --- | --- |
| 管理访问 | WebUI、Kubernetes API、数据库及诊断接口应限制在授权管理网；按项目要求接入堡垒机、访问控制及审计设施 |
| 初始账号与凭据 | 首次启用后修改初始管理口令，替换示例 WebUI Secret、JWT Secret 及归属网示例私钥；禁止把鉴权密钥写入标书或公共日志 |
| 传输保护 | 当前 Chart 的 SBI 与 WebUI 使用 HTTP 示例配置，不能视为已启用 TLS／mTLS；需要时通过经验证的网元配置、反向代理或安全网关实现 |
| NAS 算法 | 当前 AMF 模板优先配置 NEA0，并包含 NIA0 选项；空算法不提供对应的加密或完整性保护。应按项目安全策略调整并联调，不能仅凭安全上下文有效就认定 NAS 已加密 |
| 数据库 | 当前默认 MongoDB 为集群内单实例；访问控制、鉴权、备份加密及异地留存需按项目方案落实，不能因使用 ClusterIP 即认为自动安全 |
| 特权权限 | UPF 使用特权容器并操作网络；VPP 模式需要设备、大页及 CPU 资源。hostNetwork 模式会影响宿主网络，须核对端口和防火墙 |
| 业务操作 | 修改地址池、切片、PLMN、用户面模式或重建 5GC Pod 可能影响全部会话；执行后应安排终端重新接入验证 |
| 证据留存 | 日志、抓包、Helm 参数和备份可能包含 SUPI、IP、密钥或 Secret；按权限保存，外发前脱敏 |
| 镜像与版本 | 交付采用经过验证的固定版本并记录镜像摘要；latest 标签仅表示标签名称，不能证明镜像内容固定 |

当前 Chart 未实现设计文档中的 gNB 证书准入、IPsec 自动配置或 RANNodeName HMAC 准入。需要此类能力时，应单独实施并验收，不能将方案设计写成默认已交付功能。

### 2.6 合规与认证说明

XCN 核心网软件本身无射频发射设备型号核准代码。配套基站、服务器、网卡、光模块和电源的 CE、FCC、SRRC 等材料，应逐项对应实际供货型号及适用范围，由投标人附有效证书或检测报告。当前仓库不提供可直接用于投标的 XCN 整机认证证明。

开源软件及第三方组件的许可义务按交付的软件清单、仓库 LICENSE／第三方声明和合同约定履行；正式交付时应附组件及许可清单。

## 3. 产品概述

### 3.1 产品定位

XCN 为基于 Open5GS 的软件化核心网系统，采用控制面与用户面分离的 5GC 架构，可部署在满足条件的通用服务器及 Kubernetes 平台。系统通过容器化交付核心网网元、Web 管理界面和订阅数据库，为 5G SA 数据业务提供接入、认证、会话、转发及运维能力。

无线制式、工作频段、载波带宽、MIMO 层数及空口发射功率由 gNB、终端及无线规划决定。核心网通过 N2／N3 接口与接入网配合，不以无线侧参数代替自身处理能力指标。

### 3.2 主要功能与交付边界

| 功能 | 当前实现 | 使用条件／边界 |
| --- | --- | --- |
| 用户接入与认证 | 5G SA 注册、身份处理、鉴权、NAS 安全、注销及连接状态管理 | SIM 与订阅数据一致；算法及策略按项目确认 |
| 移动性管理 | 跟踪区与位置报告、空闲／连接态管理、寻呼、N2 切换及 Xn Path Switch 相关处理 | 需基站、终端及传输网络共同支持并完成互通验收 |
| 会话管理 | PDU 会话建立、修改、释放，DNN／切片与订阅匹配 | 在线订阅变更不等于已建立会话立即更新 |
| 用户面转发 | GTP-U 封装／解封装、PFCP 规则控制、N6 连接 | 根据场景选择 UDP/TUN 或 VPP/memif |
| 策略与 QoS | 订阅 QoS、AMBR、QFI／5QI 及相关规则；存在专用 QoS 策略增强实现与测试 | 端到端效果依赖无线调度、传输及业务端；参数值不等同于保障实测值 |
| 网络身份与切片 | 当前模板配置多 PLMN、TAI 和 S-NSSAI，包含 NRF、NSSF 等配套功能 | 默认配置见 3.6；定制 PLMN 需同步核对各相关网元配置 |
| IPv4 地址管理 | 统一 UE 子网／网关、可选动态池范围及静态订阅地址 | 静态地址应避开网关、网络／广播地址及动态范围，避免重复 |
| 订阅管理 | WebUI 新增、查询、修改和删除订阅，管理切片及 Session 配置 | 账号权限受控；鉴权资料由授权人员录入 |
| IPv4 后路由 | 每个订阅 Session 最多 8 条 IPv4 CIDR，支持增删改 | 需重新建立会话；还需 UE、Linux／VPP、DN 路由及 NAT 配合；非页面自动开通 |
| 运维诊断 | UE／gNB 详情、分层速率、UE 控制面事件历史、日志及指标接口 | xcnctl 为当前运维工具；不承诺通用厂商 MML 命令兼容 |
| 可选业务扩展 | ePCO 用户意图与算力通知相关增强 | 默认关闭；私有扩展需专门联调，非通用 3GPP 标准业务承诺 |

IPv6 相关会话及 TUN 示例配置存在于当前模板中，但本说明书的通用部署与业务验收以 IPv4 为主。IPv6／双栈、纯 IPv6 业务通知及加速模式 IPv6 N6 应按项目分别验证，不能由配置存在推断端到端验收完成。

### 3.3 系统架构与工作原理

![XCN 核心网逻辑架构与部署关系](xcn-architecture.png)

图 1 为逻辑关系示意。UE 的 NAS 信令经 gNB 在 N1／N2 路径到达 AMF；业务数据经 gNB 的 N3 隧道到达 UPF，再通过 N6 进入数据网。SMF 经 N4 控制 UPF。控制面服务调用按当前配置使用 SCP，并通过 NRF 完成相应注册／发现；数据库连接不属于 SBI。

| 工作负载 | 组成 | 作用 |
| --- | --- | --- |
| xcn-core | NRF、SCP | 服务注册／发现及通信代理 |
| xcn-5gc | AMF、SMF、UPF、AUSF、UDM、UDR、NSSF、PCF | 接入、鉴权、会话、策略、切片与用户面；memif 模式增加 VPP 容器 |
| xcn-webui | WebUI 及初始化容器 | 订阅与管理账号操作 |
| xcn-mongodb | 单副本 StatefulSet＋PVC | 订阅、账号及相关持久数据 |

BBU－RRU／AAU 的前传连接、天线馈线、无线帧时钟不由 XCN 核心网连接或提供。核心网宿主机应保持系统时间同步，便于日志、证书及跨网元事件关联；gNB 的 GNSS／PTP／SyncE 等无线同步按基站方案建设。

### 3.4 主要网络接口

以下为当前 Chart 的默认端口；实际防火墙与路由以部署模式、渲染清单及现场规划为准。SCTP 与 TCP 为不同传输协议，不能用 TCP 放通结果代替 N2 验证。

| 接口／服务 | 通信双方 | 协议与默认端口 | 部署说明 |
| --- | --- | --- | --- |
| N1 | UE－AMF，经 gNB 转送 | NAS，不单独开放一个服务器端口 | 结合 NGAP／NAS 流程定位 |
| N2 | gNB－AMF | NGAP／SCTP 38412 | hostNetwork 时使用配置的节点地址；另有默认 NodePort 31412 |
| N3 | gNB－UPF | GTP-U／UDP 2152 | TUN 直达 UPF；memif 经 VPP 的专用 N3 地址；默认 NodePort 32152 不替代加速模式路由 |
| N4 | SMF－UPF | PFCP／UDP 8805 | 当前同 Pod，SMF 127.0.0.4、UPF 127.0.0.7，无需向外部开放 |
| N6 | UPF－DN | 按业务承载 IPv4 等 IP 流量 | 无单一固定业务端口；需双向路由或 NAT |
| NRF／SCP SBI | 核心网内部 | Service TCP 7777 | 容器实际监听分别 7777／7778 |
| 其余控制面 SBI | 核心网内部 | Service TCP 7777 | AMF／SMF／AUSF／UDM／UDR／NSSF／PCF 实际监听依次 7779～7785 |
| PCF 外部策略访问 | 授权应用－PCF | 默认 NodePort TCP 30777 | 按需限制来源；不要无条件向公网开放 |
| 运维指标／诊断 | 管理网－网元 | Service TCP 9090；AMF／SMF／UPF／PCF 容器为 9091～9094 | 含 UE／会话信息；PCF Service 为 NodePort 时指标也可能获得节点端口，应核对实际清单 |
| WebUI | 管理员－管理页面 | TCP 9999 | 默认 Service＋hostPort；项目需要 HTTPS 时另行配置并验证 |
| MongoDB | UDR／PCF／WebUI 等－数据库 | TCP 27017 | 默认仅集群 Service；不作为外部业务入口 |

启用 hostNetwork 后，0.0.0.0 监听可能在宿主可达接口上暴露服务，不能仅依赖 Service 类型判断暴露范围。非 hostNetwork 的 hostPort／NodePort 也需核对节点转发、防火墙及 CNI 对 SCTP 的支持。采用 NodePort 时，gNB 实际使用的 AMF 端口、N3 广告地址与到达端口应联合验证，不能只确认 Service 已创建。

### 3.5 两种用户面模式

| 项目 | UDP/TUN 模式 | VPP/memif 模式 |
| --- | --- | --- |
| 选择参数 | networking.upf.mode=tun | networking.upf.mode=memif |
| N3 路径 | Linux UDP socket | N3 VF → VPP／DPDK → N3 raw-IP memif → UPF |
| N6 路径 | UPF → ogstun → Linux 路由／NAT | UPF → N6 memif → VPP 路由／NAT44 → N6 VF |
| 主要前提 | /dev/net/tun、SCTP、NET_ADMIN／特权容器及可用路由 | 额外需要两个合格 VF、设备插件、VFIO／IOMMU 条件、大页、CPU 分配机制及可达的 N3／N6 地址 |
| 典型用途 | 兼容部署、功能联调及不具备 VF 的环境 | 已具备相应硬件与平台条件的加速部署 |
| 故障定位 | UDP 监听、ogstun、主机路由及 iptables | 额外检查 VF、memif、VPP FIB／邻居、NAT、队列与 Worker |

当前 Chart 默认 mode 为空，以兼容旧配置；其默认组合解析为 UDP/TUN。新项目应显式指定 tun 或 memif。切换模式会重建相关工作负载，原 PFCP／PDU 会话不会因此自动恢复，终端需按重新接入流程验证。

memif 模式仍由 Open5GS UPF 承担 PFCP 与 PDR／FAR／QER／URR 等处理；VPP 为收发、路由及 NAT 等提供加速路径。不能将本实现等同于替换为另一套 VPP 原生 UPF。

### 3.6 默认业务配置与可配置项

| 项目 | 当前模板／默认值 | 修改说明 |
| --- | --- | --- |
| PLMN | 46011、46000、46001 | 为模板中的网络标识，使用须符合项目授权；当前无通用 PLMN／TAC values 开关，需受控修改相关模板并验证 |
| TAC | 各上述 PLMN 配置 0、1 | OAI 当前实测使用 TAC 1；TAC 0 的 OAI 配置限制见 R03，不能宣称该组合已完成 OAI 互通 |
| 切片 | SST=1、SD=010101 | SD 按 6 位十六进制标识处理，gNB、订阅和网元配置应一致 |
| UE IPv4 网段／网关 | 10.45.0.0/16；10.45.0.1 | networking.ue.ipv4Subnet／ipv4Gateway 统一供 SMF、UPF/TUN、VPP 内侧配置使用 |
| 动态 IPv4 范围 | networking.smf.ipv4PoolRanges 默认空列表 | 空列表沿用默认分配行为；需要静态地址时应显式规划不重叠的动态范围 |
| DNN | 由订阅与对接配置约定；现有联调记录使用 cmnet、internet | DNN 是业务配置，不能由示例名称推断自动开通 |
| 会话 MTU | SMF 模板为 1400 | 结合 gNB、隧道及 DN 路径确认；不能仅据此认定所有大报文均可通过 |
| DNS | SMF 模板含公共 DNS 示例 | 正式部署按可达性、内网域名和项目要求修改 |
| UE 配置上限 | 当前网元模板 global.max.ue=1024 | 为资源池配置，非商用并发能力、吞吐量或许可证承诺；保留的空闲上下文也可能占用资源池 |
| 速率统计 | 默认启用；采样周期 1000 ms | 可按用户／会话／QFI／规则观察；不作为计费结算口径 |
| 事件历史 | AMF／SMF 分别默认容量 65536 条，单 UE 上限 512 条 | 有界内存；释放 UE 后可保留历史，淘汰或网元重启会丢失 |
| MongoDB PVC | 默认启用，1 GiB，StorageClass local-path | 按实际数据量、增长和备份要求扩容；本地卷不提供跨节点数据冗余 |

### 3.7 硬件组成、资源与环境

XCN 软件无固定机箱尺寸、重量、额定功耗、风扇形式或物理接口引脚。供货包含服务器时，以上指标以及供电、电气安全、防护等级由对应硬件资料给出，填写于 6.5。服务器管理口、业务网口、光口及告警能力亦随硬件型号确定。

当前默认资源配置的部分关键值如下，CPU 单位为逻辑核：

| 组件 | CPU request／limit | 普通内存 request／limit | 备注 |
| --- | --- | --- | --- |
| UPF | 4／4 | 4 GiB／4 GiB | 加速模式需配合 CPU 分配与队列规划 |
| SMF | 1／1 | 1 GiB／4 GiB | 长期 UE churn 会逐步触及固定资源池，不能按刚启动 RSS 缩减限额 |
| AMF | 0.5／1.5 | 1 GiB／4 GiB | 用户、信令与事件容量影响占用 |
| NRF＋SCP | 合计 0.4／1 | 合计 256 MiB／512 MiB | 独立 core Pod |
| AUSF／UDM／UDR／NSSF／PCF | 详见 values.yaml | 详见 values.yaml | 应连同平台开销统一核算 |
| MongoDB | 1.5／2.5 | 1 GiB／2 GiB | PVC 容量单独计算 |
| WebUI 主容器 | 0.2／0.5 | 128 MiB／256 MiB | 初始化容器另有请求 |
| VPP，可选 | 2／2 | 16 GiB／16 GiB | 另请求及限制 8 GiB 的 1 GiB 大页；大页需单独预留 |

以上为当前 Chart 的资源请求与限制，不是整机最低配置，也不是性能承诺。选型应覆盖 Kubernetes／CNI／存储／日志等额外开销，给资源池逐步驻留、业务增长与维护留出余量。大页预留不能同时作为可用普通内存重复计算。

加速模式默认 UPF 4 核、reservedCpus=3，因此解析为 1 个 Session Worker；两侧 memif 队列数与 Worker 数一致。默认 VPP 容器独立使用 2 核。调整核数、NUMA、超线程或队列配置后，应重新验证 CPU 亲和性、拥塞、丢包与实际性能，不能仅增加 Worker 数就认定性能按比例增长。

| 环境项目 | 要求 |
| --- | --- |
| 温度、湿度、海拔 | 满足服务器、交换机、光模块和 UPS 中最严格的适用条件；数值由实际型号资料确认 |
| 供电、接地、浪涌 | 按配套硬件铭牌、原厂安装说明及项目机房设计执行 |
| 网络 | 管理、N2、N3、N6 地址与路由明确；避免与 Pod／Service／UE 网段重叠；确认 VLAN、MTU 和双向可达 |
| 存储 | 镜像、数据库、日志、备份空间分别规划；PVC 应 Bound；定期检查容量与磁盘故障 |
| 时间同步 | 宿主机使用经批准的时间源并检查同步状态；不把普通系统时间同步视为无线侧精密时钟能力 |

### 3.8 性能指标与验收口径

| 指标 | 应约定的测量条件 | 当前说明 |
| --- | --- | --- |
| 并发注册 UE／活动 PDU 会话 | 模式、硬件、资源、订阅、每 UE 会话／QFI 数、持续时间 | 待项目测试确认；global.max.ue=1024 不作为该指标证明 |
| 上／下行吞吐量与 PPS | 包长、流数、方向、端口速率、协议、CPU／NUMA、丢包门限 | 待项目性能报告确认；速率配置值不等于实测能力 |
| 注册／会话建立速率 | 每秒请求数、鉴权方式、成功率及持续时间 | 待项目确认 |
| 核心网转发／业务时延 | 起止测点、负载、时钟／仪表、平均值及分位值 | 待项目确认；不直接采用无线 RTT 替代核心网时延 |
| 可靠性与恢复时间 | 观察周期、故障模型、数据恢复、终端重接入条件 | 当前单实例不承诺双机热备、会话无损切换或特定可用性等级 |

仓库已有注册、QoS、地址池、后路由、事件及长期生命周期测试记录，可作为功能验证参考。R06 记录了隔离环境 1800 秒、53200 个 UE 生命周期等测试，但不代表 53200 个并发用户，也不替代第三方报告、全协议一致性测试或本项目的现场验收。

## 4. 安装、部署与操作指南

### 4.1 安装前检查

| 检查项 | 检查内容 | 合格判据 |
| --- | --- | --- |
| 交付核验 | 软件包、Chart、镜像清单与摘要、授权／许可材料、配套硬件和线缆 | 与交付清单一致，无缺件和损坏 |
| 现场勘测 | 机架、电源、UPS、接地、散热、光／电链路及端口标签 | 满足硬件说明与现场设计 |
| 平台与权限 | Kubernetes、Helm、kubectl、容器运行时、管理权限、CNI／DNS | 节点 Ready，基础组件正常 |
| 存储 | 默认 local-path 或已批准的替代 StorageClass | 可创建所需 PVC，容量和备份位置满足要求 |
| 网络规划 | 节点、N2／N3／N6、UE、Pod／Service 地址及 VLAN／路由 | 无重叠和重复 IP，路由及防火墙经确认 |
| 无线对接 | PLMN、TAC、S-NSSAI、DNN、SIM 数据及 N2／N3 对端 | 核心网、gNB、UE 配置一致 |
| 加速条件，可选 | 两个 VF、资源名、驱动、设备插件、大页、CPUSet 分配机制 | 资源可分配且不占用管理网卡 |
| 工具仪表 | 网线／光纤测试工具、授权终端、万用表及防静电用品 | 满足现场作业范围；软件诊断使用 ip、ss、tcpdump 等 |

Ubuntu 参考检查命令如下，仅检查当前环境，不安装或升级平台：

```bash
uname -m
cat /etc/os-release
kubectl version --short
helm version --short
kubectl get nodes -o wide
kubectl get pods -A
kubectl get storageclass
ip -br addr
ip route
timedatectl status
test -c /dev/net/tun
```

上述 kubectl version --short 对应已核对的 1.26 环境；使用其他版本时先查看 kubectl version --help。检查 SCTP 时可执行 lsmod，未显示模块也可能因为内建于内核，应结合内核配置及实际 N2 连接判断。

### 4.2 硬件安装、线缆与上电

1. 按配套服务器说明安装导轨、服务器、电源及接地；核对序列号、槽位与 BOM。
2. 将管理口、N2／N3 接入网络及 N6 业务出口按批准的端口表连接；共享物理网口时，VLAN 和安全边界应明确。
3. 电源线、网线与光纤按现场规范分路、固定、留维护余量。弯曲半径和连接器处理以线缆厂商要求为准，不挤压光纤，不带尘插接。
4. 室内服务器通常不采用无线设备馈线接头工艺；涉及室外接线盒或光缆接头时，按对应产品的防水防尘工艺施工。
5. 在两端标注设备名、端口、链路用途与编号，制作机柜／交换机端口对照表。
6. 上电前检查接地、固定、异物、电源范围、插接及保护装置；电压、极性与绝缘检查按实际硬件和现场规程实施。
7. 依次确认机房配电与 UPS、交换网络、服务器管理控制器、服务器系统、平台组件可用；软件服务按 4.5 检查启动结果。

硬件自检、风扇和指示灯判读以原厂说明为准。BMC 正常、Linux 启动成功或 Kubernetes Ready 均不单独代表核心网业务开通完成。

### 4.3 软件包与镜像准备

在交付工作站进入仓库目录；下列命令中的路径为当前项目示例，可替换为交付目录：

```bash
cd /home/xiazhangtao/code/open5gs
helm lint helm/xcn
helm show chart helm/xcn
```

镜像采用“交付包导入”或“从源码构建”之一，完成后核对所有可能调度节点的镜像可用性。当前工作区镜像归档示例为 images/xcn-runtime.tar、images/xcn-webui.tar、images/mongo.tar、images/vpp.tar；归档是否属于本次交付版本，以实际摘要清单为准。

若 Kubernetes 使用 containerd，可在对应节点使用以下命令导入交付镜像；TUN 不需要启动 VPP，但当前源码构建运行镜像时仍引用 VPP 阶段的 libmemif：

```bash
sudo ctr -n k8s.io images import images/xcn-runtime.tar
sudo ctr -n k8s.io images import images/xcn-webui.tar
sudo ctr -n k8s.io images import images/mongo.tar
# 仅加速部署按需导入；确认归档版本与交付清单一致。
sudo ctr -n k8s.io images import images/vpp.tar
sudo ctr -n k8s.io images list
```

containerd 与 Docker 的镜像存储通常分离，docker load 不代表 kubelet 已能使用镜像。多节点部署需同步导入或使用可访问的镜像仓库。

具备构建依赖和匹配 VPP 基础镜像时，可选择源码构建：

```bash
docker build -f docker/runtime/Dockerfile -t xcn-runtime:xcn-20261022 .
docker build -f docker/webui/Dockerfile -t xcn-webui:xcn-20261022 .
```

上述日期标签为构建示例，不代表仓库已发布同名制品。构建后应导入／推送到 Kubernetes 可访问的镜像源，并在 values 中指定实际仓库和标签；MongoDB、VPP 同样应采用已验证版本。代理导致构建网络异常时，可按 5.10 排查，必要时在可信构建环境评估使用 docker build --network host。

### 4.4 业务参数规划与安装

#### 4.4.1 参数核对

| 参数 | 当前环境示例 | 实施要求 |
| --- | --- | --- |
| 命名空间／Release | xcn／xcn | 与运维权限、资源命名一致 |
| AMF N2 地址 | 10.2.0.119 | gNB 可达；hostNetwork 下须为明确的本机接口地址 |
| TUN 模式 UPF N3 | 10.2.0.119 | 本机已配置且 gNB 可达，禁止使用 0.0.0.0 |
| memif 模式 UPF N3 | 10.2.0.230 | 专用广告／memif 本地地址；不能同时被主机本地地址截获 |
| VPP N3／N6 外侧 | 10.2.0.231/20／10.2.0.232/20 | 与 UPF N3 地址含义不同，需确认唯一性、下一跳、VF／交换链路 |
| UE 网段／网关 | 10.45.0.0/16／10.45.0.1 | 不与现有网络冲突；DN 具有回程路由或采用经批准的 NAT |
| 动态范围示例 | 10.45.1.2-10.45.255.254 | 仅示例；静态订阅应置于不重叠的合法地址范围 |
| PLMN／TAC／切片／DNN | 46011／1／1-010101／cmnet | 与基站、终端和订阅逐项对照，不能直接沿用未授权 PLMN |

TUN 模式中 SMF 使用 127.0.0.4:2152 的内部 GTP-U 端点。UPF 与 SMF 共享 Pod 网络命名空间，因此 UPF 的 UDP/2152 不得绑定通配地址或 127.0.0.4，否则可能截获 SMF 报文。Chart 对相关非法配置有校验。

#### 4.4.2 新建 UDP/TUN 部署

本节用于已确认无同名 Release 的新环境。已有部署应执行 4.9 的升级流程，避免覆盖原有镜像、地址、密钥和用户面模式。

准备受控文件 /tmp/xcn-site-tun.yaml，以下内容为配置示例。镜像标签须替换为实际交付标签，WebUI 两项密钥须替换为独立随机值，可用 openssl rand -hex 32 分别生成；文件权限应为 600。

```yaml
fivegc:
  hostNetwork:
    enabled: true
networking:
  amf:
    ngap:
      serverAddress: 10.2.0.119
  ue:
    ipv4Subnet: 10.45.0.0/16
    ipv4Gateway: 10.45.0.1
  smf:
    ipv4PoolRanges:
      - 10.45.1.2-10.45.255.254
  upf:
    mode: tun
    n3:
      address: 10.2.0.119
images:
  runtime:
    repository: xcn-runtime
    tag: xcn-20261022
  webui:
    repository: xcn-webui
    tag: xcn-20261022
webui:
  secretKey: REPLACE_WITH_RANDOM_SECRET_1
  jwtSecretKey: REPLACE_WITH_RANDOM_SECRET_2
```

MongoDB 版本、StorageClass、节点选择、归属网私钥及其他项目参数也应在安装前确认。上述两项 Secret 不替代管理员口令修改，也不自动开启数据库鉴权。

```bash
chmod 600 /tmp/xcn-site-tun.yaml
helm lint helm/xcn -f /tmp/xcn-site-tun.yaml
umask 077
helm template xcn helm/xcn -n xcn \
  -f /tmp/xcn-site-tun.yaml > /tmp/xcn-rendered-tun.yaml
# 审核渲染清单中的地址、镜像、资源、Secret 和暴露端口后安装。
helm install xcn helm/xcn -n xcn --create-namespace \
  -f /tmp/xcn-site-tun.yaml --wait --timeout 10m
```

TUN 启动脚本创建并启用 ogstun，配置 UE 网关、IP 转发及匹配 UE 网段的 IPv4 MASQUERADE／FORWARD 规则。hostNetwork 模式下，这些操作影响宿主网络；业务出口及防火墙应在变更前审核，并在启动后实际检查规则是否生效。

#### 4.4.3 可选 VPP/memif 部署

先完成 VF、设备插件、CPUSet 分配和大页准备，保留原有受控镜像及密钥文件；不要仅指定 mode 就假定宿主资源已自动准备。Chart 默认 VF 资源名为 intel.com/fabric_network，VPP 请求两个 VF 和 8 GiB 的 1 GiB 大页，需与平台实际资源匹配。

加速模式还依赖平台预装的 CPUSet 准入与分配组件。当前 Chart 为 Pod 设置 xnet.com/cpuset-required 标签并读取 xnet.com/used-core 分配注解，本身不安装该分配组件；默认启动保护会核对 upf、vpp、smf 的 CPU 分配信息。组件或注解缺失时可能触发 Pod 重建或启动失败，应先修复平台分配条件，不能把关闭保护当作通用安装步骤。

加速覆盖文件 /tmp/xcn-site-memif.yaml 示例：

```yaml
networking:
  upf:
    mode: memif
    n3:
      address: 10.2.0.230
vpp:
  n3:
    interfaceAddress: 10.2.0.231/20
    defaultGateway: 10.2.7.254
  n6:
    externalAddress: 10.2.0.232/20
    defaultGateway: 10.2.7.254
```

示例下一跳仅适用于对应示例网段。继承基础文件的公共参数，先检查最终清单，再在新环境安装；后面的文件覆盖前面的同名值：

```bash
helm lint helm/xcn -f /tmp/xcn-site-tun.yaml \
  -f /tmp/xcn-site-memif.yaml
umask 077
helm template xcn helm/xcn -n xcn \
  -f /tmp/xcn-site-tun.yaml -f /tmp/xcn-site-memif.yaml \
  > /tmp/xcn-rendered-memif.yaml
helm install xcn helm/xcn -n xcn --create-namespace \
  -f /tmp/xcn-site-tun.yaml -f /tmp/xcn-site-memif.yaml \
  --wait --timeout 10m
```

memif 模式下，N3 流量应经过专用 VF／VPP 路径，不能仍按 TUN 的宿主 UDP 监听判断是否正常。hostNetwork 下兼容 GTP socket 的绑定地址由 Chart 处理；应同时验证 VPP 接口、FIB、邻居、memif 连接以及上下行真实业务。切换前还应检查旧模式遗留的主机 N3 地址、路由与 NAT，按确认的影响范围处理。

### 4.5 首次启动与健康检查

Kubernetes 按探针和依赖等待条件拉起网元，并非按 YAML 中列出的顺序保证业务启动。确认 MongoDB、NRF／SCP、5GC、WebUI 及外部传输条件均可用，再开通基站和终端。

```bash
kubectl -n xcn get pods -o wide
kubectl -n xcn get deployments,statefulsets
kubectl -n xcn get svc,pvc
kubectl -n xcn rollout status deploy/xcn-core --timeout=600s
kubectl -n xcn rollout status deploy/xcn-5gc --timeout=600s
kubectl -n xcn rollout status deploy/xcn-webui --timeout=600s
kubectl -n xcn get events --sort-by=.metadata.creationTimestamp
```

TUN 模式的 xcn-5gc 应有 8 个业务容器就绪，memif 模式应有 9 个；xcn-core 为 2 个，WebUI 和 MongoDB 各 1 个。实际就绪分母以当前清单为准，初始化过程另有临时容器。探针主要检测端口可用，仍应验证网元服务发现、N4 关联及业务链路。

检查各网元日志；部分详细日志位于共享日志目录，kubectl logs 不能保证包含所有内容：

```bash
kubectl -n xcn logs deploy/xcn-5gc -c amf --tail=100
kubectl -n xcn logs deploy/xcn-5gc -c smf --tail=100
kubectl -n xcn logs deploy/xcn-5gc -c upf --tail=100
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  tail -n 100 /var/log/open5gs/amf.log
```

TUN 专项检查：

```bash
kubectl -n xcn exec deploy/xcn-5gc -c upf -- ip -d addr show ogstun
kubectl -n xcn exec deploy/xcn-5gc -c upf -- ip route
kubectl -n xcn exec deploy/xcn-5gc -c upf -- ss -lunp
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  iptables -t nat -S POSTROUTING
```

memif 专项检查：

```bash
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- vppctl show interface
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- vppctl show memif
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- vppctl show ip fib
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- vppctl show ip neighbors
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- vppctl show errors
```

### 4.6 订阅、基站与终端开通

在授权管理网访问 http://管理节点地址:9999，或使用以下仅本机可访问的临时转发，再打开 http://127.0.0.1:9999：

```bash
kubectl -n xcn port-forward svc/xcn-webui 9999:9999 \
  --address 127.0.0.1
```

1. 用受控交付的初始管理凭据登录，并完成口令修改；本说明书不携带实际口令。
2. 在订阅管理中录入与 SIM 对应的 IMSI／鉴权资料及所需订阅参数；确认 OP 与 OPc 的类型和数据一致，不在日志或标书中复制密钥。
3. 配置 SST／SD、DNN、会话类型和 QoS。静态 IPv4 必须属于 UE 子网，不能与网关、其他静态订阅及动态池重叠。
4. 如需 UE 后方网段，在对应 Session 的 IPv4 Framed Routes 中配置 CIDR。当前每个 Session 最多 8 条；页面保存不自动安装 Linux／VPP／DN 路由。
5. 在 gNB 配置 AMF N2 地址／端口、N3 地址、PLMN、TAC、切片；在 UE 配置匹配的身份、DNN 与切片。确认基站先完成 NG Setup，再验证终端注册和会话。
6. 修改会话相关订阅后，按维护计划让终端重新建立会话，并验证双向业务。

如交付同时包含本工作区 OAI Chart，可参考以下既有工程路径；命名空间、镜像和无线参数仍按 OAI 实际 Chart 确认：

```bash
helm install gnb /home/xiazhangtao/code/openairinterface5g/charts/oai-gnb \
  -n default --set amf.ip=10.2.0.119
helm install nrue /home/xiazhangtao/code/openairinterface5g/charts/oai-nr-ue \
  -n default
```

此示例为可选联调步骤，不代表 gNB、UE 或射频设备包含在 XCN 默认软件供货范围中。

### 4.7 开通验收

| 编号 | 验收项目 | 检查方法与合格判据 |
| --- | --- | --- |
| A01 | 平台与存储 | 节点 Ready；工作负载就绪；PVC Bound；无未处理调度／存储错误 |
| A02 | 网元与接口 | 服务发现和 N4 关联正常；模式、地址、路由、接口状态与批准配置一致 |
| A03 | 基站接入 | xcnctl show gnb 显示预期基站，NG Setup 成功，PLMN／TA／切片匹配 |
| A04 | 用户注册 | xcnctl show ue --detail 可定位测试用户，安全上下文、位置及 CM 状态符合当前操作 |
| A05 | PDU 会话 | DNN、PSI、UE IP、UPF-SEID、QFI／5QI 及 N3 端点与规划一致 |
| A06 | 双向数据 | 从 UE 业务接口访问指定内网 DN，并按需要测试反向访问；同时检查返回路径，不只 ping 公共 DNS |
| A07 | QoS／后路由，可选 | 用对应业务流验证实际命中、隔离、双向路由及 NAT 行为；保存配置和抓包证据 |
| A08 | 性能 | 按双方确认的硬件、负载模型和数值门限测试；填写原始结果与报告编号 |
| A09 | 恢复能力 | 在隔离或批准的维护条件下验证重启、回滚和备份恢复；记录终端重接入及实际恢复时间 |
| A10 | 安全与交付 | 示例密钥与口令已处理；管理访问受控；镜像摘要、备份及签字记录齐全 |

可先执行以下只读诊断命令：

```bash
kubectl -n xcn exec deploy/xcn-5gc -c upf -- xcnctl show gnb --detail
kubectl -n xcn exec deploy/xcn-5gc -c upf -- xcnctl show ue --detail
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show rate --level session --active-only --with-location
```

若同时部署上述 OAI nrUE，可将测试目标替换为现场可达的 DN 地址，从 UE 隧道发起探测：

```bash
kubectl -n default exec deploy/nrue-oai-nr-ue -- \
  ping -I oaitun_ue1 -c 20 -W 2 10.2.0.119
```

部署名、接口名和目标地址只适用于对应 OAI 示例。性能测试应在授权的独立业务端运行，不对未知公网目标施加负载。验收记录应包含失败与重试，不将预热、邻居未收敛或外部网络丢包静默剔除后宣称所有场景零丢包。

### 4.8 日常运维与可观测性

| 周期 | 建议项目 | 保存内容 |
| --- | --- | --- |
| 每日或按运维计划 | Pod 就绪、容器重启、节点／PVC 容量、关键告警、gNB 连接、业务抽测 | 巡检时间、异常、处置及复核结果 |
| 变更前后 | Helm 参数／清单、镜像摘要、数据库备份、网元与业务状态 | 可恢复的变更基线及审批／工单编号 |
| 定期 | 备份恢复抽测、账号权限、日志留存、依赖与安全更新评估 | 恢复证据、权限审计与版本记录 |

xcnctl show ue 的位置为 UE 最近报告的位置；reported 表示连接态最近报告，last-known 表示空闲态保留位置，unknown 表示无可靠数据，不代表连续实时定位。gNB 详情中的 observed_cells 来自 UE 上报，不是基站全部已配置小区的清单。

速率可按 user、session、bearer、rule 四层查询；CM-IDLE 与速率为零不能互相推断。统计依赖 UPF 本地 socket，UE／gNB／事件查询直接使用对应网元接口，不要求存在活动 UPF 会话。缺失或不可靠的字段以“-”或 JSON null 表示，不能当作数值零。

事件查询示例：

```bash
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show ue-event --supi imsi-460110000000100 \
  --since 30m --limit 200 --json
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show ue-event --supi imsi-460110000000100 --watch
```

SUPI 为示例，使用时替换为授权排查用户。事件时间使用 UTC，需与北京时间区分。初始 limit 默认为 100、范围 1～1000；内存容量、淘汰和重启会限制可查询历史，增加 limit 不能恢复已丢失数据。prepared、queued、accepted、completed 等为不同阶段，本地排队不代表对端收到或完整流程成功。需要长期审计时，应建设受控的外部日志／事件归档。

按 Ctrl+C 结束 watch 或 port-forward 只停止本次查询／转发，不停止核心网。show rate 不支持 --detail；详情选项用于 UE／gNB，show ue-event 也没有独立 --detail 模式。

### 4.9 备份、升级、恢复与卸载

#### 4.9.1 备份

以下为备份命令示例，会导出含敏感内容的文件，应在受控存储位置执行并按要求加密、校验和留存。当前 MongoDB 为单实例；需要严格一致性时，应先在维护窗口停止全部相关数据库写入。业务运行中的逻辑导出不能直接声明为跨集合时点一致备份。

```bash
umask 077
XCN_BACKUP_DIR="$HOME/xcn-backup/$(date +%Y%m%d-%H%M%S)"
install -d -m 700 "$XCN_BACKUP_DIR"
helm get values xcn -n xcn --all > "$XCN_BACKUP_DIR/values.yaml"
helm get manifest xcn -n xcn > "$XCN_BACKUP_DIR/manifest.yaml"
helm history xcn -n xcn > "$XCN_BACKUP_DIR/history.txt"
kubectl -n xcn get pods -o json > "$XCN_BACKUP_DIR/pods.json"
kubectl -n xcn exec statefulset/xcn-mongodb -- \
  mongodump --db open5gs --archive --gzip \
  > "$XCN_BACKUP_DIR/open5gs.archive.gz"
test -s "$XCN_BACKUP_DIR/open5gs.archive.gz"
sha256sum "$XCN_BACKUP_DIR/open5gs.archive.gz" \
  > "$XCN_BACKUP_DIR/open5gs.archive.gz.sha256"
```

另行保存与本次备份对应的 Chart、镜像摘要、硬件／平台网络配置，以及受控的外部密钥和证书。哈希值只验证文件完整性，不能替代恢复抽测。若已启用 MongoDB 鉴权，使用安全凭据注入方式补充连接参数，不把口令写入共享命令记录。

#### 4.9.2 升级与回滚

升级前将实际 Release 参数与新版本默认值核对，生成并审核完整的项目覆盖文件；保留当前 VPP 模式、资源、地址、镜像及密钥，不盲目套用本说明书示例。

```bash
helm history xcn -n xcn
helm lint helm/xcn -f /tmp/xcn-approved-values.yaml
helm upgrade xcn helm/xcn -n xcn \
  -f /tmp/xcn-approved-values.yaml \
  --atomic --wait --timeout 10m
```

此命令会产生实际变更，限于已批准的维护窗口执行。--atomic 主要处理 Helm 资源级失败，不恢复外部数据库修改或已有 UE 会话。当前 xcn-5gc 为单实例且采用重建策略；不能承诺升级无中断。

配置／Secret 更新后，若对应 Pod 模板未变化，进程未必自动重读配置。特别是 core Pod 及以 subPath 挂载的文件，应核对实际生效内容，并按变更范围安排重启，随后复验服务发现和业务。

回滚时先用 helm history 确定实际可用修订号，再按以下形式操作；N 为明确选定的历史修订号，需替换为数字：

```bash
helm rollback xcn N -n xcn --wait --timeout 10m
```

回滚完成后检查镜像与配置、N2／N4、终端重注册和业务恢复；数据格式迁移或外部系统变更需按对应版本恢复方案单独处理。

#### 4.9.3 数据恢复

先在隔离环境验证备份可恢复，再安排正式恢复。恢复前确认目标集群、命名空间、数据库及备份时间，保存当前数据并暂停写入方。下面的 --drop 会删除备份中对应集合的现有数据后恢复，包含订阅及管理账户，必须在已批准的恢复操作中使用。

```bash
test -s "$XCN_BACKUP_DIR/open5gs.archive.gz"
sha256sum -c "$XCN_BACKUP_DIR/open5gs.archive.gz.sha256"
kubectl -n xcn exec -i statefulset/xcn-mongodb -- \
  mongorestore --archive --gzip --drop --nsInclude='open5gs.*' \
  < "$XCN_BACKUP_DIR/open5gs.archive.gz"
```

XCN_BACKUP_DIR 应指向已选定的备份目录。核验恢复日志和集合内容，再恢复依赖网元与管理服务，执行注册、鉴权和双向业务测试。旧鉴权计数的恢复可能引起同步问题，应通过标准重新同步／重接入流程处理，不能将“数据库导入成功”等同于用户业务全部恢复。

#### 4.9.4 停止与卸载

按计划先停终端业务与基站接入，再卸载核心网。若使用本工程示例 Release／命名空间：

```bash
# 以下命令会停止相应业务，执行前完成备份并确认维护范围。
helm uninstall nrue -n default
helm uninstall gnb -n default
helm uninstall xcn -n xcn
kubectl -n xcn get pvc
```

StatefulSet 的 PVC 通常不会因卸载自动删除，应结合保留策略确认；卷存在也不等于已有可用备份。hostNetwork 模式的 ogstun、地址、转发开关、路由和防火墙规则可能保留，卸载后应依据变更前快照逐项检查，仅清理确认属于本部署且无其他业务使用的对象。不得用清空全部 iptables、删除整个命名空间或批量删除 PVC 代替正常卸载。

## 5. 故障排查指南

### 5.1 定位方法

采用“物理／平台 → IP 传输 → 控制面协议 → 用户面 → 业务应用”的顺序。先确认故障时间、范围、最近变更和影响用户，保存当前证据，再按最小影响范围处置。

| 层次 | 重点检查 | 典型证据 |
| --- | --- | --- |
| 物理与平台 | 供电、网口／VF、节点、Pod、PVC、CPU／内存、大页及驱动 | BMC 告警、Pod events、重启原因、资源状态 |
| IP 传输 | 地址冲突、VLAN、路由、邻居、MTU、防火墙、代理 | ip／ss／抓包、交换机端口状态 |
| 控制面 | SCTP、NG Setup、注册鉴权、SBI／NRF／SCP、PFCP | gNB／UE 查询、AMF／SMF 日志与事件 |
| 用户面 | PDU、TEID／QFI、PDR／FAR／QER、N3／N6、回程／NAT | 分段抓包、UPF 统计、VPP 接口／错误计数 |
| 业务应用 | DNS、TCP／UDP、服务端监听、访问策略 | UE／DN 双端记录、应用日志及业务探针 |

### 5.2 安装失败、Pod 未就绪或反复重启

| 现象 | 优先原因 | 检查与处理 |
| --- | --- | --- |
| Pending | CPU／内存／大页／VF 不足，节点选择不匹配，PVC 未绑定 | describe Pod 与 PVC，核对可分配资源和 StorageClass；按规划补齐资源 |
| ImagePullBackOff | 镜像缺失、标签错误、仓库不可达或凭据问题 | 核对交付摘要和每个节点镜像；区分 Docker／containerd 存储与拉取策略 |
| Init 长时间等待 | MongoDB、NRF／SCP、DNS 或依赖端口不可达 | 查初始化容器日志、Service 端点和 DNS，不仅增加等待时间 |
| CrashLoopBackOff | 配置错误、端口冲突、权限／设备缺失、CPUSet 不符合条件 | 先保存 previous 日志、终止原因和 events，再修复具体条件 |
| OOMKilled | 资源池逐步驻留、负载或诊断容量超过内存预算 | 核对实际限额和内存走势；不能凭启动 RSS 降低 AMF／SMF／UPF 内存 |

```bash
kubectl -n xcn describe pod POD_NAME
kubectl -n xcn logs POD_NAME -c smf --previous --tail=200
kubectl -n xcn get events --sort-by=.metadata.creationTimestamp
```

POD_NAME 替换为实际 Pod。previous 日志仅在存在上一实例时可用；kubectl top 需要已部署指标服务，无法获取时改用已有监控或经授权的系统指标。

### 5.3 基站无法接入／小区不可用

小区退服首先由无线侧与核心网联合定位。核心网侧检查 AMF 就绪、N2 地址和 SCTP/38412、NG Setup 结果及 PLMN／TAC／切片匹配；基站侧检查射频、授权、时钟、配置及传输链路。

```bash
kubectl -n xcn exec deploy/xcn-5gc -c upf -- xcnctl show gnb --detail
kubectl -n xcn exec deploy/xcn-5gc -c upf -- ss -lnp -A sctp
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  tail -n 200 /var/log/open5gs/amf.log
```

无 gNB 项时，先判断 SCTP 未建立还是 NG Setup 被拒绝；已有 gNB 连接不代表小区射频正常或 UE 已注册。采用非 hostNetwork 时，额外核对 SCTP 的 hostPort／NodePort／CNI 路径，不用 TCP 探测结果替代。

### 5.4 UE 注册或鉴权失败

1. 确认基站已连接、UE 选择正确 PLMN／切片，订阅身份与 SIM 一致。
2. 核对鉴权密钥、OP／OPc 类型、鉴权管理参数及同步情况，由有权限人员处理，不在工单中公开完整密钥。
3. 确认 AUSF／UDM／UDR、MongoDB 和 NRF／SCP 可用，检查相关 SBI 及鉴权事件。
4. 按 SUPI 查询事件的失败阶段和 cause，将超时、明确拒绝、同步失败分开处理。

```bash
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show ue --supi imsi-460110000000100 --detail
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show ue-event --supi imsi-460110000000100 \
  --since 30m --limit 200
```

尚未解析出 SUPI 的失败可能只存在匿名 trace 中，按 SUPI 查无记录不能直接认定未接入；必要时使用未过滤事件查询及 AMF 日志关联。UE 注销后可能保留 AMF 空闲上下文，不能仅以条目存在判断业务仍活动。

### 5.5 PDU 会话建立失败或已注册但业务不通

| 阶段 | 重点核对 |
| --- | --- |
| 会话申请 | DNN、S-NSSAI、订阅状态、会话类型、地址池可用性及静态地址冲突 |
| 策略与资源 | PCF／SMF 处理、QoS 参数、N2 资源分配及对端返回原因 |
| N4 | SMF－UPF PFCP 关联、会话建立／修改结果及 SEID |
| N3 | gNB 与 UPF 的真实可达地址、GTP-U 端口、TEID／QFI 及双向报文 |
| N6 | TUN 或 memif 实际模式、路由／转发、防火墙、NAT、DN 回程及业务监听 |

从 UE 业务接口先测试指定内网 DN，再测试 DNS 和业务端口。gNB／宿主机自身能 ping DN 不代表 UE 用户面正常。show rate 的 PSI、QFI、5QI 与 UE 详情应按同一会话关联；不要把 PSI、UPF-SEID、QFI 或 TEID 互相替代。

### 5.6 传输中断、单向不通与丢包

按 N3 入、UPF、N6 出、N6 回、N3 回逐段比对同一测试流。核对报文长度、路径 MTU、校验和、接口状态和错误计数；主机抓包的 checksum offload 显示需结合实际接收点判断，不能直接把所有“bad checksum”认定为线上错误。

TUN 模式在 UPF／hostNetwork 中可使用以下有限抓包命令；抓包包含敏感数据，注意授权和存储：

```bash
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  tcpdump -ni any -nn -c 100 'udp port 2152'
```

memif／DPDK 模式的 VF 流量可能绕过 Linux tcpdump，需使用适用的 VPP 抓包／trace 或镜像端口；主机未抓到包不能直接证明 VF 未收包。重点检查 memif 连接、队列与 Worker 数、ring／队列拥塞、VF 链路、NUMA、VPP FIB 和 NAT44。

VF 或 Pod 重建后可能出现 MAC／邻居变化；先核对 ARP／邻居收敛再定向处置，禁止盲目清空全部邻居或宿主路由。外网探测丢包需与 N6 回包和主机直连对照，不能仅凭 UE ping 就判定核心网内部丢包。

### 5.7 时钟异常、事件时间与位置不一致

| 问题 | 判断与处理 |
| --- | --- |
| 宿主时间不同步 | 检查 timedatectl、实际 NTP／PTP 服务及时间源连通性，按平台规范校时；校时前后保留时间偏差记录 |
| gNB 时钟失步 | 检查基站 GNSS／PTP／SyncE、传输与时钟源；核心网不提供射频时钟锁定能力 |
| 事件时间难以关联 | xcnctl 事件为 UTC；跨网元对时后仍须按 trace／流程标识关联，不仅按显示顺序断定因果 |
| gNB ID 与 NCI 前缀不一致 | 核对基站配置的 gNB ID、NGAP 上报 ID 位宽、NCI 和 PLMN；CLI 显示 mismatch，不应把解码 ID 覆盖上报 ID |

NCI 为 36 位，若 gNB ID 长度为 L，则本地小区号由低 36-L 位解码。示例：上报 gNB ID=3584、L=28，而 NCI=0x0000e014e 时，解码 gNB ID=3585、CELL-ID=78，两者不一致。应检查基站标识配置；CELL-ID=78 不是 PCI，也不应擅自假定位宽为 22。

### 5.8 后路由、QoS 和可选业务问题

| 现象 | 检查要点 |
| --- | --- |
| WebUI 已保存后路由但不通 | 确认在正确的切片／DNN Session 配置、会话已重建、UE 开启相应转发，并具有 UPF／Linux／VPP／DN 双向路由 |
| UE 可主动访问，DN 无法主动访问 | 检查 NAT／防火墙／回程；不能默认 MASQUERADE 允许 DN 发起新连接 |
| 小 ping 正常，TCP 大报文停顿 | 检查全路径 MTU、分片／PMTU／MSS、后方设备 MTU 与 ICMP 策略，采用抓包和实际应用数据核验 |
| QoS 参数已下发但速率不符 | 检查规则命中、方向、QFI／5QI、N2／N4 结果及无线／传输瓶颈；速率统计不是带宽保证证明 |
| ePCO 用户意图无效果 | 确认功能显式启用、运营商标识及双方私有容器一致，并查看 SMF／PCF 结果；默认关闭不属于故障 |

ePCO 用户意图当前存在明确边界：混合 Requested QoS Rules／Flow Descriptions 请求、纯 IPv6 通知及真实 RAN 的 QFI／DRB 映射需按 R07 单独确认。默认通知示例使用明文 HTTP 且不提供鉴权／重试，正式使用需先落实相应业务与安全方案。核心网处理成功不能据此认定无线独立 DRB 或算力服务处理已成功。

### 5.9 干扰异常、驻波比及硬件告警

射频干扰、SINR 异常、VSWR 告警、天馈故障及无线功率属于基站／射频系统排查范围，XCN 不提供对应物理告警阈值或馈线维修步骤。核心网维护人员应提供故障时间、注册／会话／吞吐变化和相关 gNB 信息，协同无线侧检查频谱、天馈、接头、同步与调度。

服务器电源、风扇、温度、磁盘及网卡硬件告警通过原厂 BMC／系统管理工具处理；更换流程、热插拔条件和 LED 含义以实际设备说明为准。

### 5.10 主机、容器与代理网络异常

当前环境可能使用 clashctl／Clash，排查时同时确认 shell 环境变量代理、服务代理和 TUN／透明转发规则；环境变量直连成功不代表透明代理已被绕过。N2／N3、本机回环、Kubernetes API、Pod／Service 网段和业务内网应按实际代理方案设置直连。

先保留原状态并检查代理是否截获目标，再使用单次命令绕过环境变量代理进行只读对照：

```bash
env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY \
  -u http_proxy -u https_proxy -u all_proxy \
  kubectl --request-timeout=10s -n xcn get pods
```

仍不通时，核对 NO_PROXY／no_proxy、kubeconfig 中的代理设置、透明转发、路由与防火墙。不要为排查一个目标而全局关闭防火墙或代理。Docker 构建桥接网络异常时，可在已确认安全边界的构建环境尝试 --network host，但这不替代容器 DNS、镜像仓库证书和代理规则的修正。

### 5.11 应急处理与备件更换

1. **界定影响**：确认是否为单 UE、单基站、单网元或整站故障，记录时间和最后变更。
2. **保留证据**：保存 Pod events、日志、诊断快照和必要抓包；网元重启会清除内存事件与相关运行状态。
3. **优先恢复前提**：修复供电、链路、存储、地址冲突、路由或依赖服务，避免反复重启掩盖原因。
4. **最小范围恢复**：在已批准的维护条件下选择回退配置、恢复原镜像或重建受影响工作负载。xcn-5gc 的重建会同时影响该 Pod 内所有网元，不能当作单 UE 无损复位。
5. **备件更换**：按硬件说明更换电源、磁盘、网卡或服务器，核对序列号、驱动／固件、网口／VF／MAC、存储与镜像，再恢复配置；不把故障盘上的数据视作已备份。
6. **业务复核**：检查网元就绪、gNB 重连、UE 重注册、会话和双向业务，记录实际恢复耗时与剩余风险。

当前默认单实例部署未提供通用双机热备和会话无损倒换。涉及整机损坏、数据库损坏或区域故障时，按已验证的备份恢复和替代节点方案执行；临时降载、限制新增接入或切回已验收的 TUN 模式，应作为有维护影响的受控变更。

## 6. 附录

### 6.1 物理接口与引脚说明

| 接口类型 | XCN 适用说明 | 需要附入的资料 |
| --- | --- | --- |
| 以太网电口 | 具体速率、RJ45 形式、端口编号由服务器／网卡确定 | 实际型号端口图、链路速率及线缆类别 |
| 光口 | SFP／SFP+／其他形态及光模块兼容性由设备确定 | 光模块型号、波长、距离、连接器与配对要求 |
| BMC／串口／USB | 硬件维护入口，不等同于核心网 N2／N3 接口 | 原厂管理与 Pinout 文档 |
| 电源／接地／告警口 | 由实际硬件定义，禁止使用通用软件说明推断针脚和极性 | 原厂针脚图、额定值及接线说明 |
| GNSS／北斗／PTP | 非 XCN 核心网固定硬件接口 | 若配套设备提供，附对应时钟硬件资料 |

本产品的 N2、N3、N4、N6 为逻辑接口，不能一一等同于某个固定物理插座。投标供货为软硬件一体时，应附实际面板图及完整 Pinout；当前软件仓库不足以确定这些针脚。

### 6.2 指示灯与软件状态速查

| 状态项 | 参考含义 | 后续检查 |
| --- | --- | --- |
| 服务器电源／故障／磁盘 LED | 颜色及闪烁频率随硬件型号不同 | 查原厂状态表与 BMC，不编造统一颜色规则 |
| 网口 Link／Activity LED | 物理链路与活动提示，具体含义依网卡 | 结合链路速率、错误计数与业务流量判断 |
| 节点 Ready | kubelet 与平台就绪条件满足 | 不替代核心网网元和业务检查 |
| Pod Running、READY 未满 | 容器已运行但部分未就绪 | 检查探针、依赖及事件 |
| Pod Running、READY 满额 | 当前探针通过 | 继续验证服务发现、N2／N4 与业务 |
| Pending／CrashLoopBackOff | 未调度／启动后反复退出 | 按 5.2 检查 |
| PVC Bound | 卷已绑定 | 仍需检查可写、容量、故障与备份 |
| NG Setup 成功 | gNB 与 AMF 的 NG 接入成功 | 不代表 UE 注册或无线小区全部正常 |
| CM connected／idle | UE 控制面连接态／空闲态 | idle 不等于注销，也不等于 PDU 已释放 |
| PDU active | 对应会话状态为活动 | 仍需真实双向数据验证 |
| location mismatch | 上报节点身份与 NCI 前缀不一致 | 按 5.7 核对 gNB ID、位宽和 NCI |

### 6.3 常用 CLI 索引

以下 xcnctl 命令在 UPF 容器内执行；宿主机调用时加前缀 kubectl -n xcn exec deploy/xcn-5gc -c upf --。示例 SUPI、地址和 ID 需替换为实际授权对象。

| 用途 | 命令示例 |
| --- | --- |
| UE 列表／详情 | xcnctl show ue；xcnctl show ue --supi imsi-460110000000100 --detail |
| gNB 列表／详情 | xcnctl show gnb；xcnctl show gnb --detail |
| 按基站与 PLMN 过滤 | xcnctl show ue --gnb-id 3584 --plmn 46011 --json |
| 用户汇总速率 | xcnctl show rate --level user |
| 会话速率与位置 | xcnctl show rate --level session --with-location |
| 活动 QoS 流速率 | xcnctl show rate --level bearer --active-only --watch |
| PDR 规则统计 | xcnctl show rate --level rule --json |
| 单 UE 事件 | xcnctl show ue-event --supi imsi-460110000000100 --since 30m --limit 200 |
| 实时事件观察 | xcnctl show ue-event --supi imsi-460110000000100 --watch |
| 事件帮助 | xcnctl show ue-event --help |

| 选项 | 适用说明 |
| --- | --- |
| --detail | 用于 UE／gNB 详情；不用于 rate／ue-event |
| --json | 输出结构化 JSON；watch 模式按刷新／事件模式输出，采集程序应按对应命令格式解析 |
| --watch／--interval | 连续观察及刷新周期；按 Ctrl+C 结束 |
| --active-only | 仅适用于 session／bearer／rule；需要 AMF、SMF 信息可用，否则报错，不猜测会话状态 |
| --with-location | 为 rate 增加基站／小区字段；上报位置可能为最后已知值 |
| --amf-ue-info／--amf-gnb-info／--smf-pdu-info | 覆盖对应查询 URL；默认分别为回环地址的 9091、9091、9092 端口 |
| --amf-ue-events／--smf-ue-events | 覆盖事件查询地址；仅事件子命令使用 |

速率层级 user 汇总 SUPI；session 区分会话；bearer 按同会话内 QFI 聚合；rule 展示 PDR 等规则。PSI 与 UPF-SEID 分别代表 NAS 和 PFCP 的不同标识。UPTIME 为当前 PFCP 会话存续时间，不能作为 UE 连续注册时间。

### 6.4 端口、安全与版本归档表

交付时填入以下记录并与实际清单核对：

| 归档项 | 项目值／附件 |
| --- | --- |
| XCN 项目发布版本、源码提交、Chart 版本 | 【投标人填写】 |
| Runtime／WebUI／MongoDB／VPP 镜像标签及摘要 | 【投标人填写，不能仅记录 latest】 |
| 基站／UE 型号、软件版本、SIM 及互通范围 | 【投标人填写，不在公开文件填入鉴权密钥】 |
| 服务器、网卡、驱动／固件及平台版本 | 【投标人填写】 |
| 管理地址、N2／N3／N6、VLAN、路由、防火墙 | 【引用经批准的网络规划附件】 |
| 证书、密钥及口令保管责任 | 【填写责任人与受控存放位置，不填写密钥值】 |
| 性能／安全／恢复测试报告及适用条件 | 【填写报告编号与版本】 |
| 备份周期、留存、RPO／RTO 与演练记录 | 【按项目约定填写并测试】 |

### 6.5 物料、备件与耗材清单

| 类别 | 项目 | 规格／数量说明 |
| --- | --- | --- |
| 软件 | XCN 核心网运行镜像、WebUI、Chart、说明书及许可材料 | 按实际交付版本、摘要清单和合同确认 |
| 依赖 | MongoDB；可选 VPP 26.06 及配套平台组件 | 版本、许可证和支持责任明确 |
| 服务器 | CPU、内存、SSD、电源、导轨、BMC | 【投标人填写型号、数量、尺寸、重量、功耗和环境规格】 |
| 网卡 | N2／N3／N6／管理连接；加速模式所需 SR-IOV 网卡 | 【填写速率、端口数、VF 能力及合格驱动／固件】 |
| 光电连接 | 光模块、光纤跳线、网线、配线附件 | 【填写类型、长度、数量及兼容型号】 |
| 配电／接地 | PDU、电源线、接地线及必要 UPS 配套 | 【按供电设计及硬件要求填写】 |
| 备件 | 电源、风扇、系统／数据盘、网卡、光模块 | 【按原厂可替换型号、保修和备件策略填写】 |
| 耗材 | 标签、绑扎件、防尘塞及清洁用品 | 【按现场规范填写】 |

### 6.6 技术支持与故障申报

| 支持项目 | 交付信息 |
| --- | --- |
| 责任单位／产品支持部门 | 【投标人填写】 |
| 技术支持电话／服务时段 | 【投标人填写，不预设 7×24 小时承诺】 |
| 技术支持邮箱 | 【投标人填写】 |
| 工单系统／门户 | 【投标人填写正式入口】 |
| 紧急升级联系人 | 【投标人填写姓名、岗位及授权联系方式】 |
| 响应时限／现场支持／保修 | 【按本项目合同或 SLA 填写】 |

申报时提供项目编号、产品／镜像版本、故障时间及时区、影响范围、最近变更、Pod 状态、脱敏后的 UE／gNB／事件信息和相关日志。涉及硬件故障时补充型号、序列号和原厂告警；通过指定受控渠道提交敏感证据，不在公开工单附完整鉴权资料。

### 6.7 本版编制核对与证据边界

2026-10-22 编制时，对实际 Kubernetes 环境进行了只读核对：xcn-core、xcn-5gc、xcn-webui 与 xcn-mongodb 均处于就绪状态；当前部署采用 VPP/memif，VPP 为 v26.06-release，数据库 PVC 为 1 GiB、local-path；UE／gNB／会话速率查询及事件帮助接口可用，MongoDB 镜像具有 mongodump／mongorestore 工具。

当前运行的核心网镜像标签为 localhost:5000/xcn-runtime:ue-event-observer-3cfac8f99-1021，WebUI 为 xcn-webui:latest，MongoDB 为 mongo:latest。该记录是编制时快照，不能替代实际交付的镜像摘要清单。源码、运行镜像及文档版本应分别登记。

只读核对同时观察到一处 gNB 上报身份与 NCI 前缀不一致，已在 5.7 给出识别方法；不能将服务就绪和诊断可用表述为全量验收通过。本次编制不变更运行配置，不重新执行既有记录中的压测、模式切换、恢复或无线射频验收。历史测试的配置、限制及未完成事项以 R03～R07 原记录为准。

正式投标前应完成责任人审核、项目专属信息补齐、性能与认证附件核对；正式交付时应保存双方签署的配置清单、验收结果和支持责任文件。
