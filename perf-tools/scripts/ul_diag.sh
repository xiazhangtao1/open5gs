#!/bin/sh
set -eu

secs="${1:-20}"
gnb_ip="${GNB_IP:?set GNB_IP, for example 172.30.180.49}"
veth="${VETH:?set VETH, for example calid06c2e59a79}"
rule="-p udp -s $gnb_ip --dport 2152 -m comment --comment gtpu-ul-test -j ACCEPT"

while iptables -D INPUT $rule 2>/dev/null; do :; done

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
iptables -I INPUT 1 $rule
echo "===wait==="
sleep "$secs"
echo "===iptables==="
iptables -L INPUT -vxn --line-numbers | grep gtpu-ul-test || true
snap after
iptables -D INPUT $rule 2>/dev/null || true
