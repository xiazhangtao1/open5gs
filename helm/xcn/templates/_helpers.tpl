{{- define "xcn.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/* Parse a canonical dotted IPv4 address using integer arithmetic. */}}
{{- define "xcn.ipv4Number" -}}
{{- if not (kindIs "string" .value) -}}
{{- fail (printf "%s must be an IPv4 address string" .path) -}}
{{- end -}}
{{- if not (regexMatch "^[0-9]{1,3}(\\.[0-9]{1,3}){3}$" .value) -}}
{{- fail (printf "%s must be a dotted IPv4 address" .path) -}}
{{- end -}}
{{- $number := int64 0 -}}
{{- range $octet := splitList "." .value -}}
{{- if or (gt (atoi $octet) 255) (and (gt (len $octet) 1) (hasPrefix "0" $octet)) -}}
{{- fail (printf "%s contains an invalid IPv4 octet" $.path) -}}
{{- end -}}
{{- $number = add (mul $number 256) (atoi $octet) -}}
{{- end -}}
{{- $number -}}
{{- end -}}

{{/* One validated IPv4 network for SMF, UPF/TUN and VPP N6. */}}
{{- define "xcn.ueIpv4" -}}
{{- $ue := .Values.networking.ue -}}
{{- $subnet := $ue.ipv4Subnet -}}
{{- if not (kindIs "string" $subnet) -}}
{{- fail "networking.ue.ipv4Subnet must be an IPv4 CIDR string" -}}
{{- end -}}
{{- if not (regexMatch "^[0-9.]+/([1-9]|[12][0-9]|30)$" $subnet) -}}
{{- fail "networking.ue.ipv4Subnet must be an IPv4 network with prefix 1..30" -}}
{{- end -}}
{{- $parts := splitList "/" $subnet -}}
{{- $prefix := atoi (index $parts 1) -}}
{{- $network := include "xcn.ipv4Number" (dict "value" (index $parts 0) "path" "networking.ue.ipv4Subnet") | int64 -}}
{{- $size := int64 1 -}}
{{- range until (int (sub 32 $prefix)) -}}
{{- $size = mul $size 2 -}}
{{- end -}}
{{- if ne (mod $network $size) (int64 0) -}}
{{- fail "networking.ue.ipv4Subnet must use the network address, without host bits" -}}
{{- end -}}
{{- $broadcast := sub (add $network $size) 1 -}}
{{- $gateway := include "xcn.ipv4Number" (dict "value" $ue.ipv4Gateway "path" "networking.ue.ipv4Gateway") | int64 -}}
{{- if or (le $gateway $network) (ge $gateway $broadcast) -}}
{{- fail "networking.ue.ipv4Gateway must be a usable host address within networking.ue.ipv4Subnet" -}}
{{- end -}}
{{- $ranges := .Values.networking.smf.ipv4PoolRanges -}}
{{- if not (kindIs "slice" $ranges) -}}
{{- fail "networking.smf.ipv4PoolRanges must be a list (empty uses the default pool)" -}}
{{- end -}}
{{- if gt (len $ranges) 16 -}}
{{- fail "networking.smf.ipv4PoolRanges supports at most 16 ranges" -}}
{{- end -}}
{{- $seen := list -}}
{{- range $index, $range := $ranges -}}
{{- $path := printf "networking.smf.ipv4PoolRanges[%d]" $index -}}
{{- if not (kindIs "string" $range) -}}
{{- fail (printf "%s must be an IPv4 start-end string" $path) -}}
{{- end -}}
{{- $ends := splitList "-" $range -}}
{{- if ne (len $ends) 2 -}}
{{- fail (printf "%s must be an IPv4 start-end string" $path) -}}
{{- end -}}
{{- $start := include "xcn.ipv4Number" (dict "value" (index $ends 0) "path" $path) | int64 -}}
{{- $end := include "xcn.ipv4Number" (dict "value" (index $ends 1) "path" $path) | int64 -}}
{{- if or (gt $start $end) (le $start $network) (ge $end $broadcast) -}}
{{- fail (printf "%s must be ordered usable host addresses within networking.ue.ipv4Subnet" $path) -}}
{{- end -}}
{{- if and (le $start $gateway) (ge $end $gateway) -}}
{{- fail (printf "%s must exclude networking.ue.ipv4Gateway" $path) -}}
{{- end -}}
{{- range $previous := $seen -}}
{{- if and (le $start $previous.end) (ge $end $previous.start) -}}
{{- fail (printf "%s overlaps another dynamic range" $path) -}}
{{- end -}}
{{- end -}}
{{- $seen = append $seen (dict "start" $start "end" $end) -}}
{{- end -}}
{{- $interface := printf "%s/%d" $ue.ipv4Gateway $prefix -}}
{{- $legacy := .Values.vpp.n6.interfaceAddress -}}
{{- if and (eq (include "xcn.vppEnabled" .) "true") (ne $legacy "") (ne $legacy $interface) -}}
{{- fail "vpp.n6.interfaceAddress must be empty or equal networking.ue.ipv4Gateway/subnet-prefix; configure networking.ue instead" -}}
{{- end -}}
{{- dict "subnet" $subnet "gateway" $ue.ipv4Gateway "interfaceAddress" $interface | toJson -}}
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
Resolve the recommended high-level UPF mode. An empty value preserves legacy
backend/vpp/sessionWorkers overrides; the values.yaml legacy defaults still
produce UDP/TUN in that case.
*/}}
{{- define "xcn.upfMode" -}}
{{- $mode := toString (default "" .Values.networking.upf.mode) -}}
{{- if not (has $mode (list "" "tun" "memif")) -}}
{{- fail (printf "networking.upf.mode must be empty, tun, or memif, got %q" $mode) -}}
{{- end -}}
{{- if eq $mode "" -}}legacy{{- else -}}{{- $mode -}}{{- end -}}
{{- end -}}

{{- define "xcn.upfN3Backend" -}}
{{- $mode := include "xcn.upfMode" . -}}
{{- if eq $mode "memif" -}}memif
{{- else if eq $mode "tun" -}}udp
{{- else -}}{{- .Values.networking.upf.n3.backend -}}{{- end -}}
{{- end -}}

{{- define "xcn.upfN6Backend" -}}
{{- $mode := include "xcn.upfMode" . -}}
{{- if eq $mode "memif" -}}memif
{{- else if eq $mode "tun" -}}tun
{{- else -}}{{- .Values.networking.upf.n6.backend -}}{{- end -}}
{{- end -}}

{{- define "xcn.upfSessionWorkersEnabled" -}}
{{- $mode := include "xcn.upfMode" . -}}
{{- if eq $mode "memif" -}}true
{{- else if eq $mode "tun" -}}false
{{- else -}}{{- .Values.networking.upf.dataplane.sessionWorkers.enabled -}}{{- end -}}
{{- end -}}

{{- define "xcn.vppEnabled" -}}
{{- $mode := include "xcn.upfMode" . -}}
{{- if eq $mode "memif" -}}true
{{- else if eq $mode "tun" -}}false
{{- else -}}{{- .Values.vpp.enabled -}}{{- end -}}
{{- end -}}

{{- define "xcn.upfGtpuServerAddress" -}}
{{- $n3Address := toString (default "" .Values.networking.upf.n3.address) -}}
{{- $legacy := toString .Values.networking.upf.gtpu.serverAddress -}}
{{- if and (ne $n3Address "") (eq (include "xcn.upfN3Backend" .) "udp") -}}
{{- $n3Address -}}
{{- else if and (eq $legacy "") (eq (include "xcn.upfN3Backend" .) "memif") -}}
127.0.0.8
{{- else -}}
{{- $legacy -}}
{{- end -}}
{{- end -}}

{{- define "xcn.upfGtpuAdvertiseAddress" -}}
{{- $n3Address := toString (default "" .Values.networking.upf.n3.address) -}}
{{- if ne $n3Address "" -}}{{- $n3Address -}}
{{- else -}}{{- .Values.networking.upf.gtpu.advertiseAddress -}}{{- end -}}
{{- end -}}

{{- define "xcn.upfN3MemifLocalAddress" -}}
{{- $n3Address := toString (default "" .Values.networking.upf.n3.address) -}}
{{- if ne $n3Address "" -}}{{- $n3Address -}}
{{- else -}}{{- .Values.networking.upf.n3.memif.localAddress -}}{{- end -}}
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
{{- if ne (include "xcn.upfSessionWorkersEnabled" .) "true" -}}
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
{{- if and (eq (include "xcn.upfSessionWorkersEnabled" $root) "true") (ne $queueCount $workerCount) -}}
{{- fail (printf "%s (%d) must equal the resolved Session Worker count (%d)" .path $queueCount $workerCount) -}}
{{- end -}}
{{- printf "%d" $queueCount -}}
{{- end -}}
{{- end -}}
