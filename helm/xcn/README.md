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
  'http://<node-ip>:30777/xcn-core-query/v1/users?amfUeNgapId=5'
curl --http2-prior-knowledge -sS -i \
  'http://<node-ip>:30777/xcn-core-query/v1/sessions?ueIp=10.45.0.2'
```

The `tmsi` and `amfUeNgapId` query parameters each select one user. If both
are present, `tmsi` takes precedence.

`amfUeNgapId` and `ranUeNgapId` identify the UE-associated NG connection,
not an individual PDU session. They are therefore returned on the user
object, while PDU-session-specific data remains under `sessions`:

```json
{
  "supi": "imsi-460110000000001",
  "imsi": "460110000000001",
  "registered": true,
  "amfUeNgapId": 5,
  "ranUeNgapId": 9,
  "sessions": [
    {
      "pduSessionId": 1,
      "dnn": "internet",
      "pduSessionType": 1,
      "ipv4": "10.45.0.2",
      "sNssai": {
        "sst": 1
      }
    }
  ]
}
```

These lab APIs are unauthenticated HTTP endpoints. Restrict `NodePort 30777` with firewall rules, security groups, or Kubernetes network policy before using it outside a controlled lab network.

## Notes

- `fullnameOverride` defaults to `xcn` so the generated service names remain aligned with the bundled xcn configs.
- `hostPorts` remain enabled by default for `AMF NGAP` and `UPF GTP-U`, matching the current lab-oriented Kubernetes manifests.
- `PCF SBI` is exposed as `NodePort 30777` so external modules can trigger dedicated bearer creation and query UE/session state.
- The chart keeps the same sample keys and WebUI secrets as `k8s/`; replace them before shared use.
