#!/usr/bin/env python3
"""Create editable Archify candidates. Run from the repository root."""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parent / 'assets'
REVISION = '4ccc714f5f7b39e685c39a9637e2497705ab2d19'


def node(key, label, sub, x, y, kind='backend', width=210):
    return dict(id=key, type=kind, label=label, sublabel=sub,
                pos=[x, y], size=[width, 76])


def edge(a, b, label='', emphasis=True):
    return dict(from_=a, to=b, label=label,
                variant='emphasis' if emphasis else 'default')


def write(stem, title, nodes, edges, notes, repository=False):
    candidate = dict(schema_version=1, diagram_type='architecture',
                     meta=dict(title=title, locale='zh-CN',
                               output=f'docs/training/assets/{stem}.html',
                               quality_profile='showcase'),
                     components=nodes,
                     connections=[{('from' if k == 'from_' else k): v
                                   for k, v in e.items()} for e in edges],
                     cards=[dict(dot='cyan', title='读图说明', items=notes)])
    if repository:
        candidate['meta']['repository'] = dict(
            url='https://github.com/xiazhangtao1/open5gs', revision=REVISION,
            provider='github', link_mode='local-only')
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f'{stem}.json').write_text(
        json.dumps(candidate, ensure_ascii=False, indent=2) + '\n')


def path_diagram(stem, title, items, labels, notes, repository=False):
    # Snake placement keeps long packet paths readable in portrait documents.
    positions = [(40, 40), (410, 40), (780, 40), (780, 260),
                 (410, 260), (40, 260), (40, 480), (410, 480)]
    nodes = [node(f'n{i}', *item[:2], *positions[i],
                  kind=item[2] if len(item) > 2 else 'backend')
             for i, item in enumerate(items)]
    if repository:
        for n in nodes:
            n['sources'] = [dict(path='helm/xcn/README.md', line=540, end_line=580)]
        for n in nodes[1:3] + nodes[4:6]:
            n['sources'].append(dict(path='helm/xcn/templates/config.yaml',
                                     line=332, end_line=363))
        nodes[3]['sources'].append(dict(path='src/upf/gtp-path.c',
                                        line=1147, end_line=1175))
    write(stem, title, nodes,
          [edge(f'n{i}', f'n{i+1}', label) for i, label in enumerate(labels)],
          notes, repository)


def main():
    write('01-nfvi', 'NFVI：从物理资源到业务容器', [
        node('cpu', '双路 CPU', '72 物理核 / 144 逻辑 CPU', 40, 40),
        node('pool', '隔离 CPU 池', '126 个逻辑 CPU', 410, 40),
        node('cpuset', 'CPUSet 平台组件', '分配注解与实际 cpuset', 780, 40),
        node('nic', 'X710 / PF', 'ens5f0、ens5f1 / NUMA 1', 40, 280),
        node('vf', 'VF 与设备插件', '12 / 20 个 VF 资源', 410, 280),
        node('app', 'UPF 与 VPP 容器', 'CPU + VF + 大页', 780, 280),
        node('mem', 'NUMA 本地内存', '1 GiB 大页池 / 本机 250 GiB', 780, 500),
    ], [edge('cpu', 'pool', '划分'), edge('pool', 'cpuset', '申请'),
        edge('cpuset', 'app', '绑核'), edge('nic', 'vf', 'SR-IOV'),
        edge('vf', 'app', '分配 PCI'), edge('mem', 'app', '内存支撑')],
        ['连线表示资源关系，不是报文路径。HiPHY 54 与 Asso 16 是隔离池子集。',
         '多个 VF 共享 PF 物理带宽；CPU、内存、网卡的 NUMA 位置需要匹配。'])

    write('02-platform', '镜像、发布与容器网络的职责', [
        node('build', 'Docker 构建', '代码 + Dockerfile', 40, 40),
        node('registry', 'Harbor 镜像仓库', '项目 / 镜像 / 权限', 410, 40, 'database'),
        node('runtime', 'containerd', '节点拉取镜像与启动容器', 780, 40),
        node('helm', 'Helm', 'Chart + values', 40, 290),
        node('k8s', 'Kubernetes', 'API / 调度 / kubelet', 410, 290),
        node('pod', '业务 Pod', '普通网络 + 可选 VF', 780, 290),
        node('cni', 'CNI 与设备插件', '网络配置 / PCI 资源分配', 780, 510),
    ], [edge('build', 'registry', '推送'), edge('registry', 'runtime', '镜像拉取'),
        edge('helm', 'k8s', '提交清单'), edge('k8s', 'pod', '维护副本'),
        edge('runtime', 'pod', '执行容器'), edge('cni', 'pod', '接入与分配')],
        ['Kubernetes 通过 kubelet/CRI 调用运行时；图中按职责概括，不展示全部内部 RPC。',
         '本机 Docker 与 Kubernetes containerd 镜像存储不能假定共用。'])

    for number, slug, title, split_switch in [
        (3, 'shared-port', '方式一：一个交换机，一个物理口', False),
        (4, 'shared-switch', '方式二：一个交换机，两个物理口', False),
        (5, 'split-switches', '方式三：两个交换机，两个物理口', True),
    ]:
        if number == 3:
            nodes = [node('gnb', '基站 网口1', '控制面 + 用户面', 40, 40),
                     node('sw', '共享交换机', '10G 或 100G 设计示例', 410, 40, 'cloud'),
                     node('core', '核心网 网口1', 'PF 内核 + VF 用户态', 780, 40),
                     node('dn', 'DN / 上游路由器', '经共享网络或明确的下一跳', 410, 280, 'external')]
            edges = [edge('gnb', 'sw', '上行'), edge('sw', 'gnb', '下行'),
                     edge('sw', 'core', '上行'), edge('core', 'sw', '下行'),
                     edge('sw', 'dn', '到 DN'), edge('dn', 'sw', '从 DN')]
        else:
            nodes = [node('gc', '基站 网口1', '控制 192.168.1.10/24', 40, 40),
                     node('gu', '基站 网口2', '用户 192.168.2.10/24', 40, 280),
                     node('cc', '核心网 网口3', '控制 192.168.1.20/24', 780, 40),
                     node('cu', '核心网 网口4', '用户面 PF / VF', 780, 280),
                     node('dn', 'DN / 路由器', '数据网或三层网关', 410, 530, 'external')]
            if split_switch:
                nodes += [node('cs', '控制面交换机', '10Gbps 设计示例', 410, 40, 'cloud'),
                          node('us', '用户面交换机', '100Gbps 设计示例', 410, 280, 'cloud')]
                cs, us = 'cs', 'us'
            else:
                nodes += [node('sw', '同一台交换机', '建议明确 VLAN / 端口规划', 410, 160, 'cloud')]
                cs = us = 'sw'
            edges = []
            for a, b, label in [('gc', cs, '控制'), (cs, 'cc', '控制'),
                                ('gu', us, '用户'), (us, 'cu', '用户'), (us, 'dn', 'DN')]:
                edges += [edge(a, b, label+'去程'), edge(b, a, label+'回程')]
        write(f'{number:02d}-{slug}', title, nodes, edges,
              ['图示为设计拓扑；当前 X710 活跃口协商为 1Gbps，不是 100G 实测。',
               'DN 必须有明确的二层接入或三层下一跳。N3、N6 可用同一用户面 PF 的不同 VF。',
               '不同网段不等于不同广播域；需要二层隔离时配置 VLAN。'])

    write('06-overview', '5G 业务：经核心网与基站边缘分流', [
        node('ue', '终端 UE', '商用 / 自研模拟终端', 40, 270, 'external'),
        node('gnb', '自研基站', '按目的 IP 匹配边缘 CIDR', 410, 270),
        node('core', '核心网 UPF + VPP', 'GTP-U 终结 / N6 转发', 780, 270),
        node('central', '核心 DN', '198.51.100.10', 780, 510, 'external'),
        node('edge', '边缘 DN', '203.0.113.10', 410, 510, 'external'),
        node('amf', '核心网 AMF', 'N2 控制面 / Linux SCTP', 410, 40),
    ], [edge('ue', 'gnb', '无线/仿真'), edge('gnb', 'core', '未命中 / GTP-U'),
        edge('core', 'central', 'N6 / IP'), edge('gnb', 'edge', '命中 / 无 GTP-U'),
        edge('gnb', 'amf', 'N2 / SCTP', False)],
        ['箭头画上行，下行沿对应路径返回。基站 N3 与边缘 DN 共用同一个用户面 VF。',
         '边缘业务绕过核心网用户面；注册、鉴权和会话控制仍与核心网交互。',
         '自研分流功能来自需求说明；下行会话匹配代码未提供。'])

    path_diagram('07-core-memif', '核心网 memif：封装由 UPF 处理，NAT 位于 VPP', [
        ('基站 N3', '外层目的 UPF 10.2.0.230', 'external'),
        ('VPP · dpdk-n3', '接口 10.2.0.231 / VRF 10'),
        ('VPP · memif2/0', 'N3 原始 IP / 含 UDP、GTP-U'),
        ('Open5GS UPF', 'PDR/FAR/QER / GTP-U 解封装'),
        ('VPP · memif1/0', 'N6 原始业务 IP / 10.45.0.1'),
        ('VPP · dpdk-n6', 'NAT 出口 10.2.0.232'),
        ('外部 DN', '收到普通 IP 报文', 'external'),
    ], ['GTP-U', '目的 /32 路由', '交给 UPF', '解封装后 IP', 'NAT44', '出口 IP'],
        ['上行按箭头，下行反向；同一 VPP 进程在图中按接口分开表示。',
         'UPF 的 N3 地址 .230 与 VPP 接口 .231 不同；代理 ARP 和 /32 路由导向 memif。',
         '当前两个 VF 同属 ens5f0 的 external_network 资源。IP 来自 2026-10-25 快照。'], True)

    path_diagram('08-core-tun', '核心网 TUN：Linux UDP、ogstun 与内核 NAT', [
        ('基站 N3', 'UDP/2152 + GTP-U', 'external'),
        ('Linux UDP socket', 'UPF 绑定可达 N3 地址'),
        ('Open5GS UPF', 'GTP-U 解封装 / 会话规则'),
        ('ogstun + Linux', '路由 / FORWARD / MASQUERADE'),
        ('外部 DN', '收到普通 IP 报文', 'external'),
    ], ['N3', 'UDP 交付', '内层 IP', 'N6'],
        ['箭头为上行；下行经 Linux 路由进入 ogstun，UPF 封装 GTP-U 后返回基站。',
         '当前 Chart 的 TUN 初始化会添加 MASQUERADE；VPP NAT 开关不控制此规则。'])

    for stem, title, via_core, nat in [
        ('09-core-nat', '经核心网：N6 动态 NAT', True, True),
        ('10-core-route', '经核心网：N6 静态路由', True, False),
        ('11-edge-nat', '直达边缘 DN：基站动态 NAT', False, True),
        ('12-edge-route', '直达边缘 DN：基站静态路由', False, False),
    ]:
        prefix = '198.51.100' if via_core else '203.0.113'
        items = [('UE 会话', '源 10.45.0.2:40000', 'external'),
                 ('基站分流', '未命中边缘 CIDR' if via_core else '命中 203.0.113.0/24'),
                 ('UPF 解封装' if via_core else '基站 memif / VPP',
                  '内层源 10.45.0.2' if via_core else '不加 GTP-U 头'),
                 ('核心 VPP NAT' if via_core and nat else
                  '核心 VPP 路由' if via_core else '基站 VPP NAT' if nat else '基站 VPP 路由',
                  f'源转换 {prefix}.1:p1' if nat else '源保留 10.45.0.2:40000'),
                 ('核心 DN' if via_core else '边缘 DN', f'目的 {prefix}.10:5201', 'external')]
        path_diagram(stem, title, items,
                     ['无线/仿真', 'N3 / GTP-U' if via_core else '原始 IP',
                      '普通 IP', '送往 DN'],
                     ['箭头为上行。UE 侧若另有 LAN NAT，进入此图时源已是 UE 会话地址。',
                      f'下行：DN → {prefix}.1:p1 → 逆 NAT → UE 10.45.0.2:40000。' if nat else
                      f'下行：DN → UE 10.45.0.2；DN 路由经 {prefix}.1，转发侧需对应会话路由。',
                      '示例端口 p1 由 NAT 分配，不保证保留原端口。' if nat else
                      '停用本跳 NAT 不代表全路径没有其他 NAT。'])

    path_diagram('13-wifi-ap', '模拟 UE 下挂 Wi-Fi：AP / 桥接模式', [
        ('手机或电脑', '192.168.40.10:40000', 'external'),
        ('Wi-Fi AP / 桥接', '不进行 IP 源转换'),
        ('模拟 UE · LAN', 'dpdk0 / 192.168.40.1/22'),
        ('模拟 UE · memif', 'SNAT → 10.45.0.2:p1'),
        ('基站及后续路径', '核心 DN / 边缘 DN', 'external'),
    ], ['Wi-Fi', '以太网', 'LAN → UE NAT', '无线/仿真'],
        ['AP 接入侧使用模拟 UE LAN 网段；DHCP/DNS 需另行提供，函数本身不配置 DHCP。',
         '下行先逆 UE NAT，再经 LAN 与 AP 返回 192.168.40.10。'])

    path_diagram('14-wifi-router', '模拟 UE 下挂 Wi-Fi：WAN 路由 / NAT 模式', [
        ('手机或电脑', '192.168.50.10:40000', 'external'),
        ('Wi-Fi 路由器', 'WAN 192.168.40.2:p1'),
        ('模拟 UE · LAN', '网关 192.168.40.1/22'),
        ('模拟 UE · memif', '再 SNAT → 10.45.0.2:p2'),
        ('基站及后续出口', '可再做核心/边缘 NAT', 'external'),
    ], ['Wi-Fi NAT', 'WAN → LAN', 'UE NAT', '无线/仿真'],
        ['Wi-Fi LAN 192.168.50.0/24 与模拟 UE LAN 192.168.40.0/22 不重叠。',
         '回包按出口 NAT、UE NAT、Wi-Fi NAT 的逆序逐层还原。',
         '同一物理 dpdk0 不能同时属于多个 session VRF；多会话共享 LAN 需额外设计。'])

    flow = dict(schema_version=2, diagram_type='workflow', meta=dict(
        title='业务不通：先定位断点，再处理性能', locale='zh-CN',
        output='docs/training/assets/15-troubleshoot.html', quality_profile='showcase'),
        lanes=[dict(id=key, label=label) for key,label in
               [('deploy','部署'),('physical','链路'),('access','接入'),
                ('routing','路由'),('translate','转换'),('service','应用')]],
        mainPath=['pod','link','session','route','nat','app'], nodes=[], edges=[],
        cards=[dict(dot='amber', title='分支处理原则', items=[
            '任一项不通过：停在该层，结合计数、日志和抓包定位；修复后从该层复验。',
            'NAT 路径检查映射和反向转换；静态路由路径检查回程与 UE/后方网段匹配。',
            '先小包、TCP/UDP 连通，再处理 MTU、QoS、CPU、NUMA 与吞吐。'])])
    spec = [('pod','deploy',0,'Pod 与配置','事件 / 容器 / 模式'),
            ('link','physical',0,'链路与邻居','PF/VF / MAC / ARP'),
            ('session','access',0,'接入与会话','N2 / UE / PDU'),
            ('route','routing',0,'去回路由','Linux / VPP / VRF'),
            ('nat','translate',0,'转换与封装','GTP-U / NAT 或路由'),
            ('app','service',0,'应用与性能','端口 / MTU / 负载')]
    flow['nodes']=[dict(id=i,lane=l,col=c,type='backend',label=t,sublabel=s,width=240,height=84)
                   for i,l,c,t,s in spec]
    flow['edges']=[dict(id=f'check-{i}', **{'from':a,'to':b}, label='通过', role='main')
                   for i,(a,b) in enumerate(zip(flow['mainPath'],flow['mainPath'][1:]))]
    (OUT/'15-troubleshoot.json').write_text(json.dumps(flow,ensure_ascii=False,indent=2)+'\n')


if __name__ == '__main__':
    main()
