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

{{/*
Return the configured UPF CPU limit as an integer logical-CPU count.
This also enforces the minimum CPU allocation needed to isolate one Worker,
two dispatchers, and the shared control CPU. CPUManager may temporarily expose
the node cpuset while a Pod starts, so use the immutable Helm resource limit
instead of inspecting the runtime affinity mask.
*/}}
{{- define "xcn.upfCpuCount" -}}
{{- $request := toString .Values.resources.fivegc.upf.requests.cpu -}}
{{- $limit := toString .Values.resources.fivegc.upf.limits.cpu -}}
{{- if ne $request $limit -}}
{{- fail (printf "UPF CPU isolation requires resources.fivegc.upf.requests.cpu (%s) to equal limits.cpu (%s)" $request $limit) -}}
{{- end -}}
{{- if not (regexMatch "^[0-9]+$" $limit) -}}
{{- fail (printf "UPF CPU isolation requires an integer CPU limit, got %q" $limit) -}}
{{- end -}}
{{- $cpuCount := int $limit -}}
{{- if lt $cpuCount 4 -}}
{{- fail "resources.fivegc.upf.limits.cpu must be at least 4 (one Session Worker, two dispatchers, and one control CPU)" -}}
{{- end -}}
{{- printf "%d" $cpuCount -}}
{{- end -}}

{{/*
Resolve networking.upf.dataplane.sessionWorkers.count. A numeric value keeps
manual sizing. "auto" reserves three logical CPUs for the N3/N6 dispatchers
and the shared rate/main control CPU by default, and assigns the remaining
CPUs to Session Workers.
*/}}
{{- define "xcn.upfWorkerCount" -}}
{{- $configured := toString .Values.networking.upf.dataplane.sessionWorkers.count -}}
{{- $cpuCount := include "xcn.upfCpuCount" . | int -}}
{{- if eq $configured "auto" -}}
{{- if not .Values.networking.upf.dataplane.sessionWorkers.enabled -}}
{{- printf "1" -}}
{{- else -}}
{{- $reservedText := toString .Values.networking.upf.dataplane.sessionWorkers.reservedCpus -}}
{{- if not (regexMatch "^[0-9]+$" $reservedText) -}}
{{- fail (printf "networking.upf.dataplane.sessionWorkers.reservedCpus must be an integer of at least 3, got %q" $reservedText) -}}
{{- end -}}
{{- $reserved := int $reservedText -}}
{{- if lt $reserved 3 -}}
{{- fail (printf "networking.upf.dataplane.sessionWorkers.reservedCpus must reserve at least 3 CPUs for N3/N6 dispatchers and control, got %d" $reserved) -}}
{{- end -}}
{{- $workerCount := sub $cpuCount $reserved | int -}}
{{- if or (lt $workerCount 1) (gt $workerCount 16) -}}
{{- fail (printf "automatic UPF worker count is %d (%d CPUs - %d reserved); expected 1..16" $workerCount $cpuCount $reserved) -}}
{{- end -}}
{{- printf "%d" $workerCount -}}
{{- end -}}
{{- else -}}
{{- if not (regexMatch "^[0-9]+$" $configured) -}}
{{- fail (printf "networking.upf.dataplane.sessionWorkers.count must be \"auto\" or an integer, got %q" $configured) -}}
{{- end -}}
{{- $workerCount := int $configured -}}
{{- if or (lt $workerCount 1) (gt $workerCount 16) -}}
{{- fail (printf "networking.upf.dataplane.sessionWorkers.count must be between 1 and 16, got %d" $workerCount) -}}
{{- end -}}
{{- if gt (add $workerCount 3) $cpuCount -}}
{{- fail (printf "networking.upf.dataplane.sessionWorkers.count=%d requires at least %d UPF CPUs (workers + two dispatchers + control), got %d" $workerCount (add $workerCount 3) $cpuCount) -}}
{{- end -}}
{{- printf "%d" $workerCount -}}
{{- end -}}
{{- end -}}

{{/*
Resolve one N3/N6 memif queue count. "auto" follows the Session Worker count.
When Session Workers are enabled, reject an explicit queue count that would
violate Open5GS' worker-to-TX-qid ownership requirement.
*/}}
{{- define "xcn.upfMemifQueueCount" -}}
{{- $root := .root -}}
{{- $configured := toString .value -}}
{{- $workerCount := include "xcn.upfWorkerCount" $root | int -}}
{{- if eq $configured "auto" -}}
{{- printf "%d" $workerCount -}}
{{- else -}}
{{- if not (regexMatch "^[0-9]+$" $configured) -}}
{{- fail (printf "%s must be \"auto\" or an integer, got %q" .path $configured) -}}
{{- end -}}
{{- $queueCount := int $configured -}}
{{- if or (lt $queueCount 1) (gt $queueCount 16) -}}
{{- fail (printf "%s must be between 1 and 16, got %d" .path $queueCount) -}}
{{- end -}}
{{- if and $root.Values.networking.upf.dataplane.sessionWorkers.enabled (ne $queueCount $workerCount) -}}
{{- fail (printf "%s (%d) must equal the resolved Session Worker count (%d)" .path $queueCount $workerCount) -}}
{{- end -}}
{{- printf "%d" $queueCount -}}
{{- end -}}
{{- end -}}
