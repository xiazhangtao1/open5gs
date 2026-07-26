#!/usr/bin/env bash
set -euo pipefail

pod=${1:?usage: run_pg_ul_multi.sh POD TOTAL_MBPS DURATION PCAP...}
total_mbps=${2:?total Mbps}
duration=${3:?duration seconds}
shift 3
pcaps=("$@")

if ((${#pcaps[@]} < 1 || ${#pcaps[@]} > 16)); then
    echo "PCAP count must be between 1 and 16" >&2
    exit 1
fi

packet_size=1428
wire_size=1472
total_pps=$((total_mbps * 1000000 / (packet_size * 8)))
session_count=${#pcaps[@]}

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
vppctl set interface ip table pg0 10 >/dev/null
vppctl set interface ip address pg0 198.19.0.254/24 >/dev/null

expected=0
for index in "${!pcaps[@]}"; do
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
        "{ name ul-s$id limit $limit rate $rate maxframe $maxframe size $wire_size-$wire_size pcap ${pcaps[$index]} interface pg0 node ip4-input }" \
        >/dev/null
done

vppctl clear interfaces >/dev/null
vppctl clear errors >/dev/null
vppctl packet-generator enable >/dev/null
sleep $((duration + 2))
vppctl packet-generator disable >/dev/null

echo "RESULT direction=UL sessions=$session_count target=${total_mbps}Mbps duration=${duration}s pps=$total_pps expected=$expected pacing_10us=${PACING_10US:-1}"
vppctl show packet-generator
vppctl show interface
echo "ERRORS"
vppctl show errors | awk '$1 != "0" && $1 != "Count" { print }'
