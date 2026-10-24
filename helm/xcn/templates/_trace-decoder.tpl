{{- define "xcn.traceDecoderContainer" -}}
{{- $monitor := include "xcn.componentName" (dict "root" . "component" "monitor") -}}
{{- $colocated := eq (dig "trace" "decoderPlacement" "separate" .Values.monitor) "colocated" -}}
- name: decoder
  image: {{ printf "%s:%s" .Values.monitor.trace.decoderImage.repository .Values.monitor.trace.decoderImage.tag }}
  imagePullPolicy: {{ .Values.monitor.trace.decoderImage.pullPolicy }}
  env:
    - name: MONITOR_CPUSET
      value: {{ required "monitor.cpuSet is required" .Values.monitor.cpuSet | quote }}
    - name: MONITOR_URL
      value: {{ if $colocated }}"http://127.0.0.1:9998"{{ else }}{{ printf "http://%s:9998" $monitor | quote }}{{ end }}
    - name: TRACE_REDACT_KEYS
      value: {{ dig "trace" "redactKeys" true .Values.monitor | toString | quote }}
    - name: MONITOR_INTERNAL_TOKEN
      valueFrom:
        secretKeyRef:
          name: {{ default $monitor .Values.monitor.existingSecret }}
          key: token
  ports:
    - name: trace-http
      containerPort: 9997
  readinessProbe:
    httpGet: {path: /healthz, port: trace-http}
    periodSeconds: 5
  livenessProbe:
    httpGet: {path: /healthz, port: trace-http}
    periodSeconds: 15
  resources:
{{ toYaml .Values.monitor.trace.decoderResources | indent 4 }}
  securityContext:
    runAsNonRoot: true
    runAsUser: 65532
    allowPrivilegeEscalation: false
    readOnlyRootFilesystem: true
    capabilities: {drop: ["ALL"]}
{{- end -}}
