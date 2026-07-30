#!/usr/bin/env bash
set -euo pipefail

pod=${1:?usage: run_dl_qid_diag.sh POD MBPS DURATION UE_IP [OUTPUT]}
mbps=${2:?total Mbps}
duration=${3:?duration seconds}
ue_ip=${4:?UE IPv4 address}
output=${5:-/tmp/open5gs-dl-qid-diag-$(date +%Y%m%d-%H%M%S).log}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
sample_ms=${VPP_MEMIF_SAMPLE_MS:-500}
sample_sleep=$((sample_ms / 1000)).$(printf '%03d' "$((sample_ms % 1000))")
sampler_pid=

if ((sample_ms < 100)); then
    echo "VPP_MEMIF_SAMPLE_MS must be at least 100" >&2
    exit 1
fi

cleanup() {
    if [[ -n $sampler_pid ]]; then
        kill "$sampler_pid" >/dev/null 2>&1 || true
        wait "$sampler_pid" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

snapshot_tasks() {
    local container=$1
    local label=$2

    echo "=== $label $container tasks ==="
    kubectl -n xcn exec "$pod" -c "$container" -- sh -c '
        for d in /proc/1/task/[0-9]*; do
            test -r "$d/schedstat" || continue
            comm=$(cat "$d/comm" 2>/dev/null) || continue
            allowed=$(awk "/^Cpus_allowed_list:/ { print \$2 }" "$d/status")
            voluntary=$(awk "/^voluntary_ctxt_switches:/ { print \$2 }" "$d/status")
            involuntary=$(awk "/^nonvoluntary_ctxt_switches:/ { print \$2 }" "$d/status")
            read -r runtime wait slices <"$d/schedstat"
            printf "tid=%s comm=%s cpus=%s runtime_ns=%s wait_ns=%s slices=%s voluntary=%s involuntary=%s\n" \
                "${d##*/}" "$comm" "$allowed" "$runtime" "$wait" "$slices" \
                "$voluntary" "$involuntary"
        done
    '
}

sample_vpp_memif() {
    local iterations=$(((duration * 1000 + sample_ms - 1) / sample_ms))

    kubectl -n xcn exec "$pod" -c vpp -- sh -c "
        i=0
        while test \"\$i\" -lt $iterations; do
            printf 'sample_ns=%s\n' \"\$(date +%s%N)\"
            vppctl show memif | awk '
                /^interface memif2\\/0/ { wanted=1; print; next }
                /^interface / { wanted=0 }
                wanted && /slave-to-master ring|head [0-9]+ tail [0-9]+/ { print }
            '
            i=\$((i + 1))
            sleep $sample_sleep
        done
    "
}

{
    echo "OPEN5GS_DL_QID_DIAG pod=$pod mbps=$mbps duration=$duration ue_ip=$ue_ip sample_ms=$sample_ms"
    echo "=== topology ==="
    lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
    echo "=== host cpu stat before ==="
    awk '/^cpu[0-9]+ / { print }' /proc/stat
    snapshot_tasks upf before
    snapshot_tasks vpp before
    echo "=== vpp threads before ==="
    kubectl -n xcn exec "$pod" -c vpp -- vppctl show threads
    echo "=== vpp runtime before ==="
    kubectl -n xcn exec "$pod" -c vpp -- vppctl show runtime
} >"$output"

sample_vpp_memif >>"$output" 2>&1 &
sampler_pid=$!

{
    echo "=== traffic ==="
    "$script_dir/run_pg_dl_multi.sh" "$pod" "$mbps" "$duration" "$ue_ip"
} >>"$output" 2>&1

wait "$sampler_pid"
sampler_pid=

{
    snapshot_tasks upf after
    snapshot_tasks vpp after
    echo "=== host cpu stat after ==="
    awk '/^cpu[0-9]+ / { print }' /proc/stat
    echo "=== vpp runtime after ==="
    kubectl -n xcn exec "$pod" -c vpp -- vppctl show runtime
    echo "=== upf n3 tx stats ==="
    kubectl -n xcn logs "$pod" -c upf --since="$((duration + 30))s" |
        grep -E 'N3 memif TX qid:|UPF worker [0-9]+ stats' || true
} >>"$output"

echo "$output"
