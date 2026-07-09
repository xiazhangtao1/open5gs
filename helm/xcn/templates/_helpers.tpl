{{- define "xcn.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "xcn.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}

{{- define "xcn.componentName" -}}
{{- printf "%s-%s" (include "xcn.fullname" .root) .component | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "xcn.labels" -}}
helm.sh/chart: {{ include "xcn.chart" . }}
app.kubernetes.io/name: {{ include "xcn.fullname" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end -}}

{{- define "xcn.localtimeVolumeMount" -}}
- name: host-localtime
  mountPath: /etc/localtime
  readOnly: true
{{- end -}}

{{- define "xcn.localtimeVolume" -}}
- name: host-localtime
  hostPath:
    path: /etc/localtime
    type: File
{{- end -}}
