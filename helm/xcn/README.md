# xcn Helm Chart

This chart deploys the same 5GC-focused lab topology as `k8s/`:

- standalone MongoDB pod
- standalone WebUI pod
- one `xcn-core` pod with `NRF` and `SCP`
- one `xcn-5gc` pod with `AMF`, `SMF`, `UPF`, `AUSF`, `UDM`, `UDR`, `NSSF`, and `PCF`

## Build Images

```bash
docker build -f docker/runtime/Dockerfile -t xcn-runtime:latest .
docker build -f docker/webui/Dockerfile -t xcn-webui:latest .
```

## Install and Uninstall

```bash
helm uninstall xcn
helm install xcn helm/xcn

helm uninstall xcn -n xcn
helm install xcn helm/xcn -n xcn --create-namespace
```

Render the manifests without installing:

```bash
helm template xcn helm/xcn
```

## Common Overrides

```bash
helm install xcn helm/xcn \
  --set images.runtime.repository=registry.example.com/xcn-runtime \
  --set images.webui.repository=registry.example.com/xcn-webui
```

```bash
helm install xcn helm/xcn \
  --set services.amfNgap.nodePort=31412 \
  --set services.upfN3.nodePort=32152 \
  --set services.pcfSbi.nodePort=30777
```

For a `hostNetwork` deployment, bind UPF N3 to the node address rather than
`0.0.0.0`. The SMF uses `127.0.0.4:2152` for the internal CP-function GTP-U
path, so a wildcard UPF listener would receive the SMF-bound packets itself.

```bash
helm install xcn helm/xcn -n xcn --create-namespace \
  --set fivegc.hostNetwork.enabled=true \
  --set networking.amf.ngap.serverAddress=192.168.9.60 \
  --set networking.upf.gtpu.serverAddress=192.168.9.60 \
  --set networking.upf.gtpu.advertiseAddress=192.168.9.60
```

## UPF real-time rate CLI

Rate statistics are enabled by default and sampled once per second. The data
plane only increments worker-local counters for packets successfully sent on
N3 or N6, then publishes the touched counters once per TX batch. It does not
perform per-packet time reads, allocation, logging, hash lookup, mutex locking,
or cross-core atomic increments.

Run the CLI in the UPF container:

```bash
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show rate --level user
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show rate --level session --ue-ip 10.45.0.2
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show rate --level bearer --supi imsi-001010000000001
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  xcnctl show rate --level rule --json
```

`user` aggregates all PFCP Sessions of one SUPI. `session`, `bearer`, and
`rule` display the NAS PDU Session ID (`PSI`) and temporarily omit the local
PFCP identifier (`UPF-SEID`). The CLI resolves PSI from the SMF `/pdu-info`
endpoint only when the command runs, matching SUPI and UE IP; it prints `-`
instead of guessing when no reliable match exists. `bearer` aggregates UL/DL PDRs by QFI inside
one Session; `rule` shows direction, PDR ID and QER ID. `--watch` continuously
refreshes the display, while `--seid`, `--ue-ip`, and `--supi` filter results.
The socket is local to the UPF container and has mode `0600`.

The Helm deployment uses `http://127.0.0.1:9092/pdu-info` by default. For a
standalone deployment, override it with `--smf-pdu-info URL` or the
`OPEN5GS_SMF_PDU_INFO_URL` environment variable. JSON output adds `psi` and
retains `seid` for compatibility, but no longer adds `upf_seid`.

Configure or disable it through Helm:

```yaml
networking:
  upf:
    rateStats:
      enabled: true
      intervalMs: 1000
      socket: /run/open5gs/upf-stats.sock
```

Disabling `rateStats.enabled` removes both the sampler/control thread and all
data-plane counter updates. Statistics are observational only: PDR/FAR/QER/URR,
PFCP, packet ordering, Session ownership, and routing behavior are unchanged.
Text output uses dynamic column widths and numeric right alignment. `--watch`
refreshes one screen even when `kubectl exec` is used without `-t`; use
`--json` for scripts and machine parsing.

## UPF dataplane deployment modes

The chart supports two complete UPF dataplane modes. Mixed combinations are
deliberately rejected: Session Workers require both N3 and N6 memif, and the
VPP sidecar requires both memif backends.

| Mode | N3 | N6 | VPP/SR-IOV | Session Workers |
|---|---|---|---|---|
| Compatible | kernel UDP/2152 | `ogstun` | not required | disabled |
| Accelerated | raw-IP memif | raw-IP memif | two VFs | `1..16` |

### Current defaults and required configuration

A plain `helm install xcn helm/xcn` selects the compatible UDP/TUN mode. The
important defaults are:

| Value | Default | Meaning |
|---|---|---|
| `fivegc.hostNetwork.enabled` | `false` | Use the Pod network; empty AMF/N3 addresses resolve to the Pod IP at startup |
| `networking.upf.n3.backend` | `udp` | Kernel UDP/2152 N3 |
| `networking.upf.n6.backend` | `tun` | Kernel `ogstun` N6 |
| `networking.upf.dataplane.sessionWorkers.enabled` | `false` | Do not create Session Workers or memif dispatchers |
| `vpp.enabled` | `false` | Do not create the VPP sidecar or request VFs/hugepages |
| `resources.fivegc.upf.requests/limits.cpu` | `8` / `8` | Eight logical CPUs for the UPF container |
| `networking.upf.rateStats.enabled` | `true` | Create the rate/control thread; it shares the UPF control CPU |

The chart applies the following requirements:

| Scope | Required configuration or node prerequisite |
|---|---|
| Both modes | UPF CPU request and limit must be the same integer and at least `4`. The default is `8`. N3 must use a dedicated non-wildcard bind address because SMF and UPF share the Pod network namespace and both use UDP/2152. |
| Pod-network UDP/TUN | No mode override is required. Empty AMF/UPF addresses resolve to the Pod IP. The gNB still needs a reachable NGAP and GTP-U path through the configured host ports/services. The node must provide `/dev/net/tun`. |
| `hostNetwork=true` | `networking.amf.ngap.serverAddress`, `networking.upf.gtpu.serverAddress`, and `networking.upf.gtpu.advertiseAddress` are mandatory explicit addresses. For UDP/TUN, the two UPF addresses normally use the same node N3 address. |
| VPP/memif mode | Set both backends to `memif`, enable Session Workers and VPP, and set `n3.memif.localAddress`, `vpp.n3.interfaceAddress`, `vpp.n3.defaultGateway`, `vpp.n6.externalAddress`, and `vpp.n6.defaultGateway`. `n3.memif.localAddress` must equal `gtpu.advertiseAddress`. |
| VPP/memif node | The SR-IOV device plugin must expose at least two allocatable VFs under `vpp.sriov.resourceName`; `vpp.sriov.deviceEnv` must be the matching device environment variable. With defaults, the node also needs two VPP CPUs and 8 GiB of 1-GiB hugepages available. Both VFs must be link-up. |

UDP/memif or memif/TUN mixed combinations are invalid and fail Helm rendering.
The memif mode is currently IPv4-first; the chart does not configure IPv6 N6
routing or NAT.

### CPU layout by mode

The CPU values below are logical CPUs assigned to the individual container,
not whole-node CPU numbers. If the UPF cpuset sorted by logical CPU ID is
`C0..C7`, the default layouts are:

| Mode | UPF CPU layout | VPP CPU layout |
|---|---|---|
| UDP/TUN, default 8 UPF CPUs | No Session Workers, memif dispatchers, or isolated data cores. `main/control` and the enabled `rate/control` thread are both pinned to `C3`; UDP/TUN uses the legacy UPF event path. Other auxiliary work remains under the normal scheduler inside the UPF cpuset. | No VPP container. |
| VPP/memif, default 8 UPF CPUs | Five Session Workers on `C0..C4`, N3 dispatcher on `C5`, N6 dispatcher on `C6`, and `main/control` plus `rate/control` on `C7`. | Separate two-CPU container: one `vpp_main` CPU and one VPP Worker CPU by default. |
| VPP/memif, minimum 4 UPF CPUs | One Session Worker, two dedicated dispatchers, and one shared control CPU. | Still requires the separate VPP allocation; default is two CPUs. |

For VPP/memif automatic sizing,
`Session Workers = UPF CPUs - reservedCpus`; `reservedCpus` defaults to `3` and
must remain at least `3`. Thus the current 8-CPU default resolves to **5**
Session Workers and five queues on each memif side. A manual Worker count must
satisfy `count + 3 <= UPF CPUs`; both memif queue counts must equal the resolved
Worker count. The UPF and VPP CPU allocations are independent, so the default
accelerated Pod requests 8 logical CPUs for UPF plus 2 logical CPUs for VPP,
in addition to the other 5GC containers.

### Compatible UDP/TUN mode without VFs

Use this mode when the NIC or Kubernetes environment cannot provide SR-IOV
VFs. With `hostNetwork=true`, replace `10.2.0.119` with the node address
reachable by the gNB:

```bash
helm upgrade --install xcn /home/xiazhangtao/code/open5gs/helm/xcn \
  -n xcn --create-namespace \
  --set fivegc.hostNetwork.enabled=true \
  --set networking.amf.ngap.serverAddress=10.2.0.119 \
  --set networking.upf.gtpu.serverAddress=10.2.0.119 \
  --set networking.upf.gtpu.advertiseAddress=10.2.0.119 \
  --set networking.upf.n3.backend=udp \
  --set networking.upf.n6.backend=tun \
  --set networking.upf.dataplane.sessionWorkers.enabled=false \
  --set vpp.enabled=false
```

This renders no VPP container or SR-IOV resource request. The UPF container
mounts `/dev/net/tun`; `open5gs-k8s-setup` creates `ogstun`, enables IP
forwarding, and installs IPv4 MASQUERADE/FORWARD rules. When Pod networking is
used instead of `hostNetwork`, omit the three explicit AMF/UPF host addresses;
the UPF GTP-U address defaults to the Pod IP.

Verify the running mode:

```bash
kubectl -n xcn get deploy xcn-5gc \
  -o jsonpath='{.spec.template.spec.containers[*].name}{"\n"}'
kubectl -n xcn exec deploy/xcn-5gc -c upf -- ip -d addr show ogstun
kubectl -n xcn exec deploy/xcn-5gc -c upf -- ss -lunp
kubectl -n xcn exec deploy/xcn-5gc -c upf -- \
  iptables -t nat -S POSTROUTING
```

Expected results are eight 5GC containers with no `vpp`, an UP `ogstun`
interface, and `open5gs-upfd` listening on the configured N3 address at
UDP/2152. Restart or re-register the UE after changing modes because an UPF
restart cannot recover the old PFCP Session state from the UE's existing TUN.

### Accelerated VPP/memif mode with two VFs

Use the command in the next section. For a `hostNetwork` deployment, the
Open5GS compatibility GTP socket can use a dedicated loopback address such as
`127.0.0.8`; `advertiseAddress` and `n3.memif.localAddress` must both be the
N3 address exposed by VPP.

## VPP memif N3/N6 with two SR-IOV VFs

The first-stage accelerated topology keeps Open5GS UPF responsible for PFCP,
PDR/FAR/QER/URR and GTP-U semantics. N3 and N6 packet I/O use separate raw-IP
memif links to a VPP 26.06 sidecar. VPP owns two SR-IOV VFs through DPDK: one
VF and FIB table for N3, and one VF for N6/NAT44. The resource is configurable;
the chart currently defaults to `intel.com/fabric_network`. The original UDP
N3 and TUN N6 backends remain available for fallback.

Build the memif-enabled runtime image and install with free N3/N6 addresses:

```bash
docker build --network host -f docker/runtime/Dockerfile \
  -t xcn-runtime:memif-dev .

helm upgrade --install xcn helm/xcn -n xcn --create-namespace \
  --set images.runtime.tag=memif-dev \
  --set fivegc.hostNetwork.enabled=true \
  --set networking.amf.ngap.serverAddress=10.2.0.119 \
  --set networking.upf.gtpu.serverAddress=127.0.0.8 \
  --set networking.upf.gtpu.advertiseAddress=10.2.0.226 \
  --set networking.upf.n3.backend=memif \
  --set networking.upf.n3.memif.localAddress=10.2.0.226 \
  --set networking.upf.n6.backend=memif \
  --set networking.upf.dataplane.sessionWorkers.enabled=true \
  --set-string networking.upf.dataplane.sessionWorkers.count=auto \
  --set networking.upf.dataplane.sessionWorkers.reservedCpus=3 \
  --set-string networking.upf.n3.memif.queues=auto \
  --set-string networking.upf.n6.memif.queues=auto \
  --set resources.fivegc.upf.requests.cpu=8 \
  --set resources.fivegc.upf.limits.cpu=8 \
  --set vpp.enabled=true \
  --set vpp.sriov.resourceName=intel.com/fabric_network \
  --set vpp.sriov.deviceEnv=PCIDEVICE_INTEL_COM_FABRIC_NETWORK \
  --set vpp.n3.interfaceAddress=10.2.0.225/20 \
  --set vpp.n3.defaultGateway=10.2.7.254 \
  --set vpp.n6.externalAddress=10.2.0.224/20 \
  --set vpp.n6.defaultGateway=10.2.7.254
```

The chart requests two VFs, 2 exclusive logical CPUs for VPP, 8 CPUs for UPF, 8 GiB
of hugepages and 16 GiB of regular memory for VPP. The VPP main heap is 8 GiB.
Adjust these values to the NIC NUMA layout rather than reducing them for a
performance run. Both VFs must be link-up and use different PCI functions.
To use the cabled external PF instead, set the resource and device environment
to `intel.com/external_network` and
`PCIDEVICE_INTEL_COM_EXTERNAL_NETWORK`. The entrypoint reads the configured
environment-variable name rather than assuming one device-plugin resource.

### Automatic UPF Session Worker sizing

When Session Workers are enabled, the recommended configuration derives the
Worker and memif queue counts from the UPF container CPU limit:

```text
Session Workers = UPF logical CPUs - reservedCpus
```

The default `reservedCpus: 3` reserves one logical CPU for each of the N3 and
N6 dispatchers and one shared control CPU for the rate sampler and UPF main
event thread. For example, an 8-CPU UPF creates 5 Session Workers and five
N3/N6 memif queues on both Open5GS and VPP:

```bash
helm upgrade --install xcn helm/xcn -n xcn --create-namespace \
  --set resources.fivegc.upf.requests.cpu=8 \
  --set resources.fivegc.upf.limits.cpu=8 \
  --set networking.upf.dataplane.sessionWorkers.enabled=true \
  --set-string networking.upf.dataplane.sessionWorkers.count=auto \
  --set networking.upf.dataplane.sessionWorkers.reservedCpus=3 \
  --set-string networking.upf.n3.memif.queues=auto \
  --set-string networking.upf.n6.memif.queues=auto
```

Automatic mode requires equal integer CPU requests and limits, at least 4 UPF
logical CPUs, and a resolved Worker count of `1..16`, so the valid default
range is 4 to 19 UPF logical CPUs. `reservedCpus` must be at least 3. An
explicit Worker count must also leave three CPUs for the two dispatchers and
the shared control CPU. These CPUs belong only to the UPF container; VPP CPU
resources remain independent.

An explicit numeric `sessionWorkers.count` keeps manual sizing. A memif queue
value of `auto` follows either the automatically resolved or explicit Worker
count. When Session Workers are enabled, Helm rejects explicit N3/N6 queue
counts that do not match the Worker count.

The calculation deliberately uses the Helm CPU limit rather than the process
affinity mask. CPUManager can temporarily expose the node cpuset during Pod
startup; deriving the Worker count from that transient mask would create too
many threads. After CPUManager assigns the exclusive cpuset, Open5GS pins the
Workers, the N3/N6 dispatchers, and the shared rate/main control threads to the
resolved `N+3` CPUs.

### VPP CPU sizing and affinity

By default the VPP sidecar reserves one logical CPU for `vpp_main` and creates
one Worker for every remaining logical CPU. Its default two-CPU Guaranteed
allocation is one physical core with both hyperthreads: one for main and one
for the Worker. The entrypoint waits for the CPUManager cpuset, reads the Linux
CPU topology, and continuously reapplies the selected affinity. All threads
remain `SCHED_OTHER`; do not use `SCHED_FIFO`.

```yaml
vpp:
  cpu:
    workers: auto
    initialMode: dense
    affinityPollMs: 250
  resources:
    requests:
      cpu: "2"
    limits:
      cpu: "2"
```

In `dense` mode the topology-aware layout fills both hyperthreads of a physical
core before using another core. Inspect the resolved plan and the actual
per-thread assignments with:

```bash
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  cat /run/vpp/affinity-plan.json
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  cat /run/vpp/affinity-status
```

The normal mode is `dense`. An explicit numeric `workers` value remains
available for diagnostic A/B tests. `isolated` may be selected when the CPU
allocation provides `workers + 1` distinct physical cores.

With one Worker, `vpp.n6.nat44.workers` stays empty because VPP rejects the
`set nat workers` command unless at least two Workers exist. When allocating
more VPP Workers, set it explicitly if NAT should use a selected worker set.

`workers: 0` is supported only for controlled CPU-scaling diagnostics and is
not a production default.

NAT44 is enabled by default so IPv4 UE traffic keeps the former
TUN/iptables-MASQUERADE behavior. When the upstream router has a route for
`10.45.0.0/16` via the VPP external address, disable NAT with
`--set vpp.n6.nat44.enabled=false`.

Verify the connections, PCI interfaces, counters, N3 FIB, and NAT worker:

```bash
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock show memif
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock show hardware-interfaces
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock show ip fib table 10
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock show nat workers
kubectl -n xcn exec deploy/xcn-5gc -c vpp -- \
  vppctl -s /run/vpp/cli.sock show errors
kubectl -n xcn logs deploy/xcn-5gc -c upf | grep -E 'N[36] memif'
```

The expected uplink path is `dpdk-n3 -> memif2/0 -> Open5GS -> memif1/0 ->
dpdk-n6`. The default pins NAT44 to worker 2 because `memif1/0` RX is assigned
to that worker in this 8-worker layout. If interface placement changes, update
`vpp.n6.nat44.workers` after checking `show hardware-interfaces`. Each current
libmemif backend uses one ring, so extra VPP CPUs do not by themselves
parallelize a single Open5GS UPF instance.

The current implementation is IPv4-first. IPv6 UE routing/NAT is not enabled
by this VPP template. Restore the original path with
`networking.upf.n3.backend=udp`, `networking.upf.n6.backend=tun`, and
`vpp.enabled=false`. Session Workers must also be disabled when restoring the
UDP/TUN path.

### Historical runtime validation on 2026-07-30

Both modes were deployed on the same node from this chart:

- UDP/TUN rendered an `8/8 Running` 5GC Pod with no VPP container or VF
  request. `ogstun` was UP, UDP/2152 and PFCP were listening/associated, six
  PFCP Sessions were recreated, and the three OAI UE business TUN interfaces
  tested each completed five bidirectional pings with zero loss.
- VPP/memif rendered a `9/9 Running` Pod requesting two
  `intel.com/fabric_network` VFs. N3 and N6 each connected six memif rings, six
  Session Workers plus two dispatchers were pinned to the eight UPF logical
  CPUs under the then-current two-reserved-CPU layout. The current layout
  reserves a third control CPU and therefore creates five Workers with the
  same eight-UPF-CPU allocation. Six PFCP Sessions were recreated. A
  six-Session N6 injection sent 1,750 packets through `memif1/0`; Open5GS
  emitted all 1,750 through `memif2/0`, with zero loss in the Open5GS memif
  segment.

The fabric physical ports were not cabled during the memif validation, so the
second result proves the VPP/Open5GS core path but does not claim an external
physical end-to-end return path.

## External PCF APIs

The chart exposes PCF SBI through `xcn-pcf` as `NodePort 30777` by default. External modules such as a computing center can call:

```text
http://<node-ip>:30777
```

Create a dedicated bearer/QoS flow for an existing PDU session:

```bash
curl --http2-prior-knowledge -sS -i -X POST \
  http://<node-ip>:30777/xcn-dedicated-bearer/v1/bearers \
  -H 'Content-Type: application/json' \
  -d '{
    "supi": "imsi-460110000000001",
    "pduSessionId": 1,
    "mediaType": "audio",
    "flowDescriptions": [
      "permit out ip from any to assigned",
      "permit in ip from assigned to any"
    ],
    "qos": {
      "5qi": 2,
      "arp": {
        "priorityLevel": 8,
        "preemptionCapability": "NOT_PREEMPT",
        "preemptionVulnerability": "PREEMPTABLE"
      },
      "maxbrDl": "10 Mbps",
      "maxbrUl": "10 Mbps",
      "gbrDl": "5 Mbps",
      "gbrUl": "5 Mbps"
    }
  }'
```

The request is translated inside PCF into the normal policy authorization path and triggers SM policy/session modification when the target UE session exists. Required fields are `supi`, `pduSessionId`, and `flowDescriptions`. `mediaType` is still supported for legacy automatic QoS mapping (`audio` -> 5QI 1, `video` -> 5QI 2, `control` -> 5QI 5). New callers can directly specify `qos.5qi` or `qos.index`, `qos.arp.priorityLevel`, `qos.arp.preemptionCapability`, `qos.arp.preemptionVulnerability`, `qos.maxbrDl`, `qos.maxbrUl`, `qos.gbrDl`, and `qos.gbrUl`. Legacy top-level bandwidth fields `marBwDl`, `marBwUl`, `mirBwDl`, `mirBwUl`, `rrBw`, and `rsBw` are also supported.

The target session can be selected by `ueIp`, `ngapId` + `pduSessionId`, or the legacy `supi` + `pduSessionId` pair. If multiple selectors are present, PCF uses only the highest priority selector: `ueIp` first, then NGAP ID + `pduSessionId`, then `supi` + `pduSessionId`. An NGAP ID identifies the UE connection, not a PDU session, so `pduSessionId` is required with `ngapId`, `amfUeNgapId`, or `ranUeNgapId`. `ngapId` matches AMF UE NGAP ID first and then RAN UE NGAP ID.

Query XCN-created dedicated bearer triggers for a target PDU session:

```bash
curl --http2-prior-knowledge -sS -i \
  'http://<node-ip>:30777/xcn-dedicated-bearer/v1/bearers?ueIp=10.45.0.2'
curl --http2-prior-knowledge -sS -i \
  'http://<node-ip>:30777/xcn-dedicated-bearer/v1/bearers?ngapId=2&pduSessionId=1'
curl --http2-prior-knowledge -sS -i \
  'http://<node-ip>:30777/xcn-dedicated-bearer/v1/bearers?supi=imsi-460110000000001&pduSessionId=1'
```

Delete one created application session/bearer trigger by the returned `appSessionId`. This removes the PCC/QoS rules created by that trigger, not the UE PDU session:

```bash
curl --http2-prior-knowledge -sS -i -X DELETE \
  http://<node-ip>:30777/xcn-dedicated-bearer/v1/bearers/<appSessionId>
```

Query current core users and sessions:

```bash
curl --http2-prior-knowledge -sS -i \
  http://<node-ip>:30777/xcn-core-query/v1/users
curl --http2-prior-knowledge -sS -i \
  http://<node-ip>:30777/xcn-core-query/v1/users/imsi-460110000000001
curl --http2-prior-knowledge -sS -i \
  'http://<node-ip>:30777/xcn-core-query/v1/users?tmsi=3221227435'
curl --http2-prior-knowledge -sS -i \
  'http://<node-ip>:30777/xcn-core-query/v1/sessions?ueIp=10.45.0.2'
```

These lab APIs are unauthenticated HTTP endpoints. Restrict `NodePort 30777` with firewall rules, security groups, or Kubernetes network policy before using it outside a controlled lab network.

## Notes

- `fullnameOverride` defaults to `xcn` so the generated service names remain aligned with the bundled xcn configs.
- `hostPorts` remain enabled by default for `AMF NGAP` and `UPF GTP-U`, matching the current lab-oriented Kubernetes manifests.
- `PCF SBI` is exposed as `NodePort 30777` so external modules can trigger dedicated bearer creation and query UE/session state.
- The chart keeps the same sample keys and WebUI secrets as `k8s/`; replace them before shared use.
