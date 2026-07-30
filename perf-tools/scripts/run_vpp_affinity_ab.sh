#!/usr/bin/env bash
set -euo pipefail

pod=${1:?usage: run_vpp_affinity_ab.sh POD MBPS DURATION UE_IP [OUTPUT]}
mbps=${2:?total Mbps}
duration=${3:?duration seconds}
ue_ip=${4:?UE IPv4 address}
output=${5:-/tmp/open5gs-vpp-affinity-ab-$(date +%Y%m%d-%H%M%S).log}
runs=${RUNS_PER_MODE:-3}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if ((runs < 1)); then
    echo "RUNS_PER_MODE must be at least 1" >&2
    exit 1
fi

vpp_exec() {
    kubectl -n xcn exec "$pod" -c vpp -- "$@"
}

set_mode() {
    local mode=$1
    local status=
    local attempt

    vpp_exec sh -c "printf '%s\\n' '$mode' > /run/vpp/affinity-mode"
    for attempt in $(seq 1 40); do
        status=$(vpp_exec cat /run/vpp/affinity-status 2>/dev/null || true)
        if [[ $status == mode="$mode "* ]]; then
            echo "$status"
            return
        fi
        sleep 0.25
    done
    echo "timed out waiting for VPP affinity mode $mode; last status: $status" >&2
    exit 1
}

snapshot() {
    local label=$1

    echo "=== $label affinity ==="
    vpp_exec cat /run/vpp/affinity-plan.json
    vpp_exec cat /run/vpp/affinity-status
    vpp_exec sh -c '
        pid=$(pidof vpp)
        for d in /proc/$pid/task/[0-9]*; do
            comm=$(cat "$d/comm" 2>/dev/null) || continue
            case "$comm" in
                vpp_main|vpp_wk_*)
                    allowed=$(awk "/^Cpus_allowed_list:/ {print \$2}" "$d/status")
                    policy=$(chrt -p "${d##*/}" 2>/dev/null |
                        awk -F": " "/current scheduling policy/ {print \$2}")
                    read -r runtime wait slices <"$d/schedstat"
                    voluntary=$(awk "/^voluntary_ctxt_switches:/ {print \$2}" "$d/status")
                    involuntary=$(awk "/^nonvoluntary_ctxt_switches:/ {print \$2}" "$d/status")
                    printf "tid=%s comm=%s cpu=%s policy=%s runtime_ns=%s wait_ns=%s slices=%s voluntary=%s involuntary=%s\n" \
                        "${d##*/}" "$comm" "$allowed" "$policy" \
                        "$runtime" "$wait" "$slices" "$voluntary" "$involuntary"
                    ;;
            esac
        done
    '
    vpp_exec vppctl show interface rx-placement
    vpp_exec vppctl show threads
}

restore_isolated() {
    set_mode isolated >/dev/null 2>&1 || true
}
trap restore_isolated EXIT

{
    echo "OPEN5GS_VPP_AFFINITY_AB pod=$pod mbps=$mbps duration=$duration ue_ip=$ue_ip runs=$runs"
    for phase in dense:A1 isolated:B dense:A2; do
        mode=${phase%%:*}
        label=${phase##*:}
        echo "=== phase $label mode=$mode ==="
        set_mode "$mode"
        sleep 2
        snapshot "$label-before"
        for run in $(seq 1 "$runs"); do
            echo "=== phase $label run $run ==="
            "$script_dir/run_pg_dl_multi.sh" \
                "$pod" "$mbps" "$duration" "$ue_ip"
        done
        snapshot "$label-after"
        kubectl -n xcn logs "$pod" -c upf --since="$((runs * (duration + 15)))s" |
            grep -E 'N3 memif TX qid:|UPF worker [0-9]+ stats' |
            tail -120 || true
    done
} >"$output" 2>&1

restore_isolated
trap - EXIT
echo "$output"
