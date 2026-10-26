# xcn Kubernetes Manifests

This directory adds native Kubernetes manifests for a 5GC-focused xcn deployment.

If you prefer Helm, a matching chart is available in `helm/xcn`.

Deployment layout:

- `xcn-mongodb`: standalone `StatefulSet`
- `xcn-core`: single `Deployment` with multiple containers for `NRF` and `SCP`
- `xcn-5gc`: single `Deployment` with multiple containers for `AMF`, `SMF`, `UPF`, `AUSF`, `UDM`, `UDR`, `NSSF`, and `PCF`
- `xcn-webui`: standalone `Deployment`

Included components:

- MongoDB
- WebUI
- NRF
- SCP
- AMF
- SMF
- UPF
- AUSF
- UDM
- UDR
- NSSF
- PCF

Not included in this first pass:

- MME
- SGWC
- SGWU
- HSS
- PCRF

## Images

Build the shared runtime image:

```bash
docker build -f docker/runtime/Dockerfile -t xcn-runtime:latest .
```

Build the WebUI image:

```bash
docker build -f docker/webui/Dockerfile -t xcn-webui:latest .
```

If you push to a registry, update the `image:` fields in the manifests.

## Deploy

Apply the manifests in order:

```bash
kubectl apply -f k8s/00-config.yaml
kubectl apply -f k8s/00-runtime-config.yaml
kubectl apply -f k8s/01-mongodb.yaml
kubectl apply -f k8s/02-core.yaml
kubectl apply -f k8s/03-5gc.yaml
kubectl apply -f k8s/04-webui.yaml
```

Check rollout status:

```bash
kubectl get pods,svc
kubectl rollout status deploy/xcn-core
kubectl rollout status deploy/xcn-5gc
kubectl rollout status statefulset/xcn-mongodb
kubectl rollout status deploy/xcn-webui
```

## Helm

The same topology is also packaged as a Helm chart:

```bash
helm install xcn helm/xcn
helm template xcn helm/xcn
```

The chart keeps `fullnameOverride: xcn` by default so the generated service names stay aligned with the xcn configuration.

External-address related knobs:

- Helm:
  - `.Values.networking.amf.ngap.serverAddress`
  - `.Values.networking.upf.gtpu.serverAddress`
  - `.Values.networking.upf.gtpu.advertiseAddress`
  - `.Values.networking.webui.hostname`
  - `.Values.networking.webui.port`
- Raw manifests:
  - update `xcn-runtime-config`

The `UPF` advertise address falls back to the node IP from `status.hostIP` when `UPF_GTPU_ADVERTISE_ADDRESS` is left empty.

## Networking Defaults

- `xcn-amf-ngap` exposes `NGAP/SCTP` with `NodePort 31412`.
- `xcn-upf-n3` exposes `GTP-U/UDP` with `NodePort 32152`.
- `xcn-pcf` exposes `PCF SBI/HTTP` with `NodePort 30777`.
- The combined `xcn-5gc` Pod uses `hostPort 38412` for `AMF NGAP`.
- The combined `xcn-5gc` Pod uses `hostPort 2152` for `UPF GTP-U`.
- The `xcn-webui` Pod uses `hostPort 9999` for direct access on the node IP.
- The combined `xcn-5gc` Deployment uses `strategy: Recreate` because the Pod cannot roll while those `hostPort`s are still occupied.
- The `xcn-webui` Deployment also uses `strategy: Recreate` for the same reason.
- All other xcn services are `ClusterIP`.

The host ports are enabled because most lab integrations expect the standard ports directly on the node IP. The NodePort services are still present for clusters that want explicit service objects for external exposure.

## External PCF APIs

PCF exposes lab-oriented APIs on:

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

These APIs are unauthenticated HTTP endpoints. Restrict `NodePort 30777` with firewall rules, security groups, or Kubernetes network policy before using it outside a controlled lab network.

## WebUI Access

The conservative default is direct access through the node IP:

```bash
http://<node-ip>:9999
```

Use port-forward only when your cluster forbids `hostPort` or you do not want to expose the node IP:

```bash
kubectl port-forward service/xcn-webui 9999:9999
```

Default account:

- Username: `admin`
- Password: `1423`

## Config And Secrets

- xcn NF configs are stored in the `xcn-config` `ConfigMap`.
- UDM home network keys are stored in the `xcn-udm-hnet` `Secret`.
- WebUI bootstrap secrets are stored in the `xcn-webui-env` `Secret`.

The provided secrets and keys are sample values suitable for development and lab use. Replace them before using the manifests in a shared or production-like environment.

## Cluster Requirements

- Kubernetes must support `SCTP` services.
- Nodes running `xcn-upf` must allow:
  - privileged pods
  - `NET_ADMIN`
  - mounting `/dev/net/tun`
  - `hostPort 2152/UDP`
- Nodes running the combined `xcn-5gc` Pod must allow `hostPort 38412/SCTP`.
- Nodes running `xcn-webui` must allow `hostPort 9999/TCP`.

## Lab Notes

- `UPF` renders its GTP-U advertise address from the node IP via `status.hostIP` when no explicit advertise address is configured.
- `AMF NGAP`, `UPF GTP-U server.address`, and `WebUI` listen on `0.0.0.0` by default. External access is provided by node IP plus `hostPort`, not by binding the process directly to the node IP.
- `WEBUI_PORT` in the raw manifests must stay aligned with the static `Service` and probe port definitions in [k8s/04-webui.yaml](/mnt/d/codex/xcn-v2/xcn/k8s/04-webui.yaml).
- The `xcn-core` and `xcn-5gc` Pods use unique internal ports for each NF and keep the existing service names stable through `targetPort` remapping.
- In a multi-node cluster, direct node-IP access only works against the node currently hosting the Pod unless you add a load balancer or pin the workload to a dedicated node.
- If your cluster forbids `hostPort`, or if your CNI handles `NodePort`/`SCTP` differently, you may need a follow-up variant using `hostNetwork` or a dedicated load balancer.
- These manifests intentionally do not modify the existing `docker/docker-compose.yml` flow.

## Basic Validation

```bash
kubectl get pods
kubectl get svc
kubectl logs deploy/xcn-core -c nrf
kubectl logs deploy/xcn-core -c scp
kubectl logs deploy/xcn-5gc -c smf
kubectl logs deploy/xcn-5gc -c upf
```

Expected checks:

- NRF registers the other 5GC functions.
- SCP registers to NRF and the remaining NFs register through SCP.
- SMF associates with UPF over PFCP.
- WebUI connects to MongoDB.
