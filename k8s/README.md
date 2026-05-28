# Open5GS Kubernetes Manifests

This directory adds native Kubernetes manifests for a 5GC-focused Open5GS deployment.

If you prefer Helm, a matching chart is available in `helm/open5gs`.

Deployment layout:

- `open5gs-mongodb`: standalone `StatefulSet`
- `open5gs-core`: single `Deployment` with multiple containers for `NRF` and `SCP`
- `open5gs-5gc`: single `Deployment` with multiple containers for `AMF`, `SMF`, `UPF`, `AUSF`, `UDM`, `UDR`, `NSSF`, and `PCF`
- `open5gs-webui`: standalone `Deployment`

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
docker build -f docker/runtime/Dockerfile -t open5gs-runtime:latest .
```

Build the WebUI image:

```bash
docker build -f docker/webui/Dockerfile -t open5gs-webui:latest .
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
kubectl rollout status deploy/open5gs-core
kubectl rollout status deploy/open5gs-5gc
kubectl rollout status statefulset/open5gs-mongodb
kubectl rollout status deploy/open5gs-webui
```

## Helm

The same topology is also packaged as a Helm chart:

```bash
helm install open5gs helm/open5gs
helm template open5gs helm/open5gs
```

The chart keeps `fullnameOverride: open5gs` by default so the generated service names stay aligned with the Open5GS configuration.

External-address related knobs:

- Helm:
  - `.Values.networking.amf.ngap.serverAddress`
  - `.Values.networking.upf.gtpu.serverAddress`
  - `.Values.networking.upf.gtpu.advertiseAddress`
  - `.Values.networking.webui.hostname`
  - `.Values.networking.webui.port`
- Raw manifests:
  - update `open5gs-runtime-config`

The `UPF` advertise address falls back to the node IP from `status.hostIP` when `UPF_GTPU_ADVERTISE_ADDRESS` is left empty.

## Networking Defaults

- `open5gs-amf-ngap` exposes `NGAP/SCTP` with `NodePort 31412`.
- `open5gs-upf-n3` exposes `GTP-U/UDP` with `NodePort 32152`.
- The combined `open5gs-5gc` Pod uses `hostPort 38412` for `AMF NGAP`.
- The combined `open5gs-5gc` Pod uses `hostPort 2152` for `UPF GTP-U`.
- The `open5gs-webui` Pod uses `hostPort 9999` for direct access on the node IP.
- The combined `open5gs-5gc` Deployment uses `strategy: Recreate` because the Pod cannot roll while those `hostPort`s are still occupied.
- The `open5gs-webui` Deployment also uses `strategy: Recreate` for the same reason.
- All other Open5GS services are `ClusterIP`.

The host ports are enabled because most lab integrations expect the standard ports directly on the node IP. The NodePort services are still present for clusters that want explicit service objects for external exposure.

## WebUI Access

The conservative default is direct access through the node IP:

```bash
http://<node-ip>:9999
```

Use port-forward only when your cluster forbids `hostPort` or you do not want to expose the node IP:

```bash
kubectl port-forward service/open5gs-webui 9999:9999
```

Default account:

- Username: `admin`
- Password: `1423`

## Config And Secrets

- Open5GS NF configs are stored in the `open5gs-config` `ConfigMap`.
- UDM home network keys are stored in the `open5gs-udm-hnet` `Secret`.
- WebUI bootstrap secrets are stored in the `open5gs-webui-env` `Secret`.

The provided secrets and keys are sample values suitable for development and lab use. Replace them before using the manifests in a shared or production-like environment.

## Cluster Requirements

- Kubernetes must support `SCTP` services.
- Nodes running `open5gs-upf` must allow:
  - privileged pods
  - `NET_ADMIN`
  - mounting `/dev/net/tun`
  - `hostPort 2152/UDP`
- Nodes running the combined `open5gs-5gc` Pod must allow `hostPort 38412/SCTP`.
- Nodes running `open5gs-webui` must allow `hostPort 9999/TCP`.

## Lab Notes

- `UPF` renders its GTP-U advertise address from the node IP via `status.hostIP` when no explicit advertise address is configured.
- `AMF NGAP`, `UPF GTP-U server.address`, and `WebUI` listen on `0.0.0.0` by default. External access is provided by node IP plus `hostPort`, not by binding the process directly to the node IP.
- `WEBUI_PORT` in the raw manifests must stay aligned with the static `Service` and probe port definitions in [k8s/04-webui.yaml](/mnt/d/codex/open5gs-v2/open5gs/k8s/04-webui.yaml).
- The `open5gs-core` and `open5gs-5gc` Pods use unique internal ports for each NF and keep the existing service names stable through `targetPort` remapping.
- In a multi-node cluster, direct node-IP access only works against the node currently hosting the Pod unless you add a load balancer or pin the workload to a dedicated node.
- If your cluster forbids `hostPort`, or if your CNI handles `NodePort`/`SCTP` differently, you may need a follow-up variant using `hostNetwork` or a dedicated load balancer.
- These manifests intentionally do not modify the existing `docker/docker-compose.yml` flow.

## Basic Validation

```bash
kubectl get pods
kubectl get svc
kubectl logs deploy/open5gs-core -c nrf
kubectl logs deploy/open5gs-core -c scp
kubectl logs deploy/open5gs-5gc -c smf
kubectl logs deploy/open5gs-5gc -c upf
```

Expected checks:

- NRF registers the other 5GC functions.
- SCP registers to NRF and the remaining NFs register through SCP.
- SMF associates with UPF over PFCP.
- WebUI connects to MongoDB.
