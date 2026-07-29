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
  --set networking.upf.gtpu.serverAddress=10.2.0.119 \
  --set networking.upf.gtpu.advertiseAddress=10.2.0.226 \
  --set networking.upf.n3.backend=memif \
  --set networking.upf.n3.memif.localAddress=10.2.0.226 \
  --set networking.upf.n6.backend=memif \
  --set vpp.enabled=true \
  --set vpp.sriov.resourceName=intel.com/fabric_network \
  --set vpp.sriov.deviceEnv=PCIDEVICE_INTEL_COM_FABRIC_NETWORK \
  --set vpp.n3.interfaceAddress=10.2.0.225/20 \
  --set vpp.n3.defaultGateway=10.2.7.254 \
  --set vpp.n6.externalAddress=10.2.0.224/20 \
  --set vpp.n6.defaultGateway=10.2.7.254
```

The chart requests two VFs, 8 exclusive CPUs for VPP, 8 CPUs for UPF, 8 GiB
of hugepages and 16 GiB of regular memory for VPP. The VPP main heap is 8 GiB.
Adjust these values to the NIC NUMA layout rather than reducing them for a
performance run. Both VFs must be link-up and use different PCI functions.
To use the cabled external PF instead, set the resource and device environment
to `intel.com/external_network` and
`PCIDEVICE_INTEL_COM_EXTERNAL_NETWORK`. The entrypoint reads the configured
environment-variable name rather than assuming one device-plugin resource.

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
`vpp.enabled=false`.

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
