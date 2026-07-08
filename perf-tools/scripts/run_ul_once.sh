#!/bin/sh
set -eu

rate="${1:?uplink Mbps required}"
secs="${2:-10}"
pod="${POD:?set POD to xcn 5gc pod name}"
gnb_pod="${GNB_POD:?set GNB_POD to gNB pod name}"
upf_ip="${UPF_IP:-10.2.0.119}"
teid="${UL_TEID:?set UL_TEID, for example 7b3c}"
ue_ip="${UE_IP:-10.45.0.2}"
upf_tun_ip="${UPF_TUN_IP:-10.45.0.1}"
namespace="${NAMESPACE:-xcn}"
diag_out="${OUT:-/tmp/ul_diag_${rate}.out}"
gen_out="/tmp/ul_gen_${rate}.out"

kubectl -n "$namespace" exec "$pod" -c upf -- /tmp/ul_diag.sh 20 >"$diag_out" &
diag_pid=$!
sleep 2
kubectl exec "$gnb_pod" -- /tmp/gtpu_gen "$upf_ip" "$teid" \
    "$ue_ip" "$upf_tun_ip" "$secs" "$rate" 1400 >"$gen_out"
wait "$diag_pid"

echo "===generator==="
cat "$gen_out"
cat "$diag_out"
