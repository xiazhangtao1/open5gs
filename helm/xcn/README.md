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
  --set services.upfN3.nodePort=32152
```

## Notes

- `fullnameOverride` defaults to `xcn` so the generated service names remain aligned with the bundled xcn configs.
- `hostPorts` remain enabled by default for `AMF NGAP` and `UPF GTP-U`, matching the current lab-oriented Kubernetes manifests.
- The chart keeps the same sample keys and WebUI secrets as `k8s/`; replace them before shared use.
