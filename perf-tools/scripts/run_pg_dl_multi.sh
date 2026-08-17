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

pg_name=
pg_created=0
stream_names=()

cleanup() {
    status=$?
    trap - EXIT INT TERM HUP
    set +e
    for stream_name in "${stream_names[@]}"; do
        vppctl packet-generator disable "$stream_name" >/dev/null 2>&1
        vppctl packet-generator delete "$stream_name" >/dev/null 2>&1
    done
    if ((pg_created)); then
        vppctl delete packet-generator interface "$pg_name" >/dev/null 2>&1
    fi
    if ((status != 0)); then
        echo "stopped; cleaned packet-generator resources for ${pg_name:-unallocated}" >&2
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

for _ in $(seq 1 16); do
    run_key="${HOSTNAME:-unknown}:$$:$RANDOM:$(date +%s%N)"
    checksum=$(printf '%s' "$run_key" | cksum)
    pg_id=$((${checksum%% *} % 2147482647 + 1000))
    pg_name="pg$pg_id"
    interface_output=$(vppctl show interface "$pg_name" 2>&1)
    [[ $interface_output != *"unknown input"* ]] && continue
    vppctl create packet-generator interface "$pg_name" >/dev/null
    pg_created=1
    break
done
if ((! pg_created)); then
    echo "failed to allocate an independent packet-generator interface" >&2
    exit 1
fi

address_index=$(((pg_id % 4096) * 32))
address_second=$((18 + address_index / 65536))
address_remainder=$((address_index % 65536))
address_third=$((address_remainder / 256))
address_fourth=$((address_remainder % 256))
vppctl set interface state "$pg_name" up >/dev/null
vppctl set interface ip address "$pg_name" \
    "198.$address_second.$address_third.$((address_fourth + 31))/27" >/dev/null
printf -v pg_tag '%x' "$pg_id"

expected=0
for index in "${!session_ips[@]}"; do
    id=$((index + 1))
    stream_name="d${pg_tag}s$id"
    stream_names+=("$stream_name")
    source_ip="198.$address_second.$address_third.$((address_fourth + id))"
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
        "{ name $stream_name limit $limit rate $rate maxframe $maxframe size $packet_size-$packet_size interface $pg_name node ip4-input data { UDP: $source_ip -> ${session_ips[$index]} UDP: $((10000 + id)) -> $((9000 + id)) length 1408 checksum 0 incrementing 1400 } }" \
        >/dev/null
done

for stream_name in "${stream_names[@]}"; do
    vppctl packet-generator enable "$stream_name" >/dev/null
done
sleep $((duration + 2))
for stream_name in "${stream_names[@]}"; do
    vppctl packet-generator disable "$stream_name" >/dev/null
done

echo "RESULT direction=DL pg=$pg_name streams=${stream_names[*]} sessions=$session_count target=${total_mbps}Mbps duration=${duration}s pps=$total_pps expected=$expected pacing_10us=${PACING_10US:-1}"
vppctl show packet-generator
vppctl show interface
echo "ERRORS_CUMULATIVE"
vppctl show errors | awk '$1 != "0" && $1 != "Count" { print }'
