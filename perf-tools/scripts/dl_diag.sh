#!/bin/sh
set -eu

rate="${1:-800}"
secs="${2:-10}"
ue_ip="${UE_IP:-10.45.0.2}"
upf_tun_ip="${UPF_TUN_IP:-10.45.0.1}"
gnb_ip="${GNB_IP:?set GNB_IP, for example 172.30.180.49}"
veth="${VETH:?set VETH, for example calid06c2e59a79}"
udp_gen="${UDP_GEN:-/tmp/udp_gen}"
rule="-p udp -d $gnb_ip --dport 2152 -m comment --comment gtpu-dl-test -j ACCEPT"

while iptables -D OUTPUT $rule 2>/dev/null; do :; done

sum_softnet() {
    python3 -c '
processed = dropped = squeezed = collisions = 0
with open("/proc/net/softnet_stat") as f:
    for line in f:
        cols = line.split()
        if len(cols) < 9:
            continue
        processed += int(cols[0], 16)
        dropped += int(cols[1], 16)
        squeezed += int(cols[2], 16)
        collisions += int(cols[8], 16)
print(processed, dropped, squeezed, collisions)
'
}

snap() {
    tag="$1"
    echo "===${tag}_ogstun==="
    cat /sys/class/net/ogstun/statistics/tx_bytes
    cat /sys/class/net/ogstun/statistics/tx_packets
    cat /sys/class/net/ogstun/statistics/tx_dropped
    cat /sys/class/net/ogstun/statistics/rx_bytes
    cat /sys/class/net/ogstun/statistics/rx_packets
    cat /sys/class/net/ogstun/statistics/rx_dropped
    echo "===${tag}_veth==="
    cat /sys/class/net/"$veth"/statistics/tx_bytes
    cat /sys/class/net/"$veth"/statistics/tx_packets
    cat /sys/class/net/"$veth"/statistics/tx_dropped
    cat /sys/class/net/"$veth"/statistics/rx_bytes
    cat /sys/class/net/"$veth"/statistics/rx_packets
    cat /sys/class/net/"$veth"/statistics/rx_dropped
    echo "===${tag}_softnet_sum==="
    sum_softnet
    echo "===${tag}_udp_snmp==="
    awk '/^Udp:/{line=$0} END{print line}' /proc/net/snmp
}

snap before
iptables -I OUTPUT 1 $rule
echo "===run==="
"$udp_gen" "$upf_tun_ip" "$ue_ip" 9999 "$secs" "$rate" 1400
sleep 1
echo "===iptables==="
iptables -L OUTPUT -vxn --line-numbers | grep gtpu-dl-test || true
snap after
iptables -D OUTPUT $rule 2>/dev/null || true
