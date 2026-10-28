{{- define "xcn.traceDecoderContainer" -}}
{{- $monitor := include "xcn.componentName" (dict "root" . "component" "monitor") -}}
{{- $colocated := eq (dig "trace" "decoderPlacement" "separate" .Values.monitor) "colocated" -}}
- name: decoder
  image: {{ printf "%s:%s" (default .Values.monitor.image.repository .Values.monitor.trace.decoderImage.repository) (default .Values.monitor.image.tag .Values.monitor.trace.decoderImage.tag) }}
  imagePullPolicy: {{ default .Values.monitor.image.pullPolicy .Values.monitor.trace.decoderImage.pullPolicy }}
  command: ["python3", "/opt/xcn-monitor/decoder.py"]
  env:
{{ include "xcn.monitorCpuEnv" (dict "root" . "resources" .Values.monitor.trace.decoderResources) | indent 4 }}
    - name: MONITOR_URL
      value: {{ if $colocated }}"http://127.0.0.1:9998"{{ else }}{{ printf "http://%s:9998" $monitor | quote }}{{ end }}
    - name: TRACE_REDACT_KEYS
      value: {{ dig "trace" "redactKeys" false .Values.monitor | toString | quote }}
    - name: MONITOR_INTERNAL_TOKEN
      valueFrom:
        secretKeyRef:
          name: {{ default $monitor .Values.monitor.existingSecret }}
          key: token
  ports:
    - name: trace-http
      containerPort: 9997
  startupProbe:
    httpGet: {path: /healthz, port: trace-http}
    periodSeconds: 5
    failureThreshold: {{ include "xcn.monitorStartupThreshold" . }}
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
