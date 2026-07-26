#!/usr/bin/env bash
set -euo pipefail

pod=${1:?usage: run_pg_dl_multi.sh POD TOTAL_MBPS DURATION UE_IP...}
total_mbps=${2:?total Mbps}
duration=${3:?duration seconds}
shift 3
session_ips=("$@")

if ((${#session_ips[@]} < 1 || ${#session_ips[@]} > 16)); then
    echo "UE_IP count must be between 1 and 16" >&2
    exit 1
fi

packet_size=1428
total_pps=$((total_mbps * 1000000 / (packet_size * 8)))
session_count=${#session_ips[@]}

vppctl() {
    kubectl -n xcn exec "$pod" -c vpp -- vppctl "$@"
}

vppctl packet-generator disable >/dev/null 2>&1 || true
for id in $(seq 1 16); do
    vppctl packet-generator delete "dl-s$id" >/dev/null 2>&1 || true
    vppctl packet-generator delete "ul-s$id" >/dev/null 2>&1 || true
done
vppctl delete packet-generator interface pg0 >/dev/null 2>&1 || true
vppctl create packet-generator interface pg0 >/dev/null
vppctl set interface state pg0 up >/dev/null
vppctl set interface ip address pg0 198.18.0.254/24 >/dev/null

expected=0
for index in "${!session_ips[@]}"; do
    id=$((index + 1))
    rate=$((total_pps / session_count))
    if ((index < total_pps % session_count)); then
        rate=$((rate + 1))
    fi
    limit=$((rate * duration))
    expected=$((expected + limit))
    if [[ ${PACING_10US:-1} == 1 ]]; then
        maxframe=$(((rate + 99999) / 100000))
    else
        maxframe=256
    fi

    vppctl packet-generator new \
        "{ name dl-s$id limit $limit rate $rate maxframe $maxframe size $packet_size-$packet_size interface pg0 node ip4-input data { UDP: 198.18.0.$id -> ${session_ips[$index]} UDP: $((10000 + id)) -> $((9000 + id)) length 1408 checksum 0 incrementing 1400 } }" \
        >/dev/null
done

vppctl clear interfaces >/dev/null
vppctl clear errors >/dev/null
vppctl packet-generator enable >/dev/null
sleep $((duration + 2))
vppctl packet-generator disable >/dev/null

echo "RESULT direction=DL sessions=$session_count target=${total_mbps}Mbps duration=${duration}s pps=$total_pps expected=$expected pacing_10us=${PACING_10US:-1}"
vppctl show packet-generator
vppctl show interface
echo "ERRORS"
vppctl show errors | awk '$1 != "0" && $1 != "Count" { print }'
