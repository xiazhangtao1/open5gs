#!/bin/sh
set -eu

dl_rate="${1:?downlink Mbps required}"
ul_rate="${2:?uplink Mbps required}"
secs="${3:-10}"
pod="${POD:?set POD to xcn 5gc pod name}"
gnb_pod="${GNB_POD:?set GNB_POD to gNB pod name}"
upf_ip="${UPF_IP:-10.2.0.119}"
teid="${UL_TEID:?set UL_TEID, for example 7b3c}"
ue_ip="${UE_IP:-10.45.0.2}"
upf_tun_ip="${UPF_TUN_IP:-10.45.0.1}"
namespace="${NAMESPACE:-xcn}"
diag_out="${OUT:-/tmp/du_diag_dl${dl_rate}_ul${ul_rate}.out}"
ul_out="/tmp/du_ul_dl${dl_rate}_ul${ul_rate}.out"

kubectl -n "$namespace" exec "$pod" -c upf -- \
    /tmp/du_diag.sh "$dl_rate" "$secs" 3 >"$diag_out" &
diag_pid=$!
sleep 3
kubectl exec "$gnb_pod" -- /tmp/gtpu_gen "$upf_ip" "$teid" \
    "$ue_ip" "$upf_tun_ip" "$secs" "$ul_rate" 1400 >"$ul_out"
wait "$diag_pid"

echo "===ul_generator==="
cat "$ul_out"
cat "$diag_out"
