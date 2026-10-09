#!/bin/sh
set -eu

# Host-network UDP deployments can opt in to a dedicated, persistent N3 alias.
if [ -n "${UPF_N3_HOST_INTERFACE:-}" ]; then
    if [ "${UPF_N3_BACKEND:-udp}" != "udp" ]; then
        echo "N3 host interface provisioning requires UDP" >&2
        exit 1
    fi
    ip link show dev "$UPF_N3_HOST_INTERFACE" >/dev/null
    if ! ip -o link show dev "$UPF_N3_HOST_INTERFACE" | grep -q '<[^>]*UP'; then
        echo "N3 host interface is administratively down" >&2
        exit 1
    fi
    case "${UPF_GTPU_SERVER_ADDRESS:-}" in
        ''|*[!0-9.]*) echo "N3 host provisioning requires an IPv4 address" >&2; exit 1 ;;
    esac
    if ! ip -o -4 addr show dev "$UPF_N3_HOST_INTERFACE" |
        awk -v address="$UPF_GTPU_SERVER_ADDRESS" '
            { split($4, parts, "/"); if (parts[1] == address) found = 1 }
            END { exit found ? 0 : 1 }'; then
        ip addr add "${UPF_GTPU_SERVER_ADDRESS}/32" dev "$UPF_N3_HOST_INTERFACE"
    fi
fi

ensure_iptables_rule() {
    table="${1:-}"
    chain="${2:-}"
    
    if [ -z "$table" ] || [ -z "$chain" ]; then
        echo "iptables: missing table or chain, skiping rule" >&2
        return 0
    fi
    
    shift 2
    
    if ! iptables -t "$table" -L "$chain" >/dev/null; then
        echo "iptables: skiping unavailable $table/$chain chain" >&2
        return 0
    fi
    
    if ! iptables -t "$table" -C "$chain" "$@" 2>/dev/null; then
        iptables -t "$table" -A "$chain" "$@" 
    fi
}

ensure_ip6tables_rule() {
    table="${1:-}"
    chain="${2:-}"
    
    if [ -z "$table" ] || [ -z "$chain" ]; then
        echo "ip6tables: missing table or chain, skiping rule" >&2
        return 0
    fi
    
    shift 2
    
    if ! ip6tables -t "$table" -L "$chain" >/dev/null; then
        echo "ip6tables: skiping unavailable $table/$chain chain" >&2
        return 0
    fi
    
    if ! ip6tables -t "$table" -C "$chain" "$@" 2>/dev/null; then
        ip6tables -t "$table" -A "$chain" "$@" 
    fi
}


if [ "${UPF_N6_BACKEND:-tun}" = "memif" ]; then
    echo "N6 memif backend selected; skipping TUN and iptables setup"
    exit 0
fi

if ! grep "ogstun" /proc/net/dev > /dev/null 2>&1; then
    ip tuntap add name ogstun mode tun
fi

sysctl -w net.ipv4.ip_forward=1 > /dev/null
sysctl -w net.ipv6.conf.all.forwarding=1 > /dev/null
sysctl -w net.ipv6.conf.ogstun.disable_ipv6=0 > /dev/null || true

ue_ipv4_subnet="${UE_IPV4_SUBNET:-10.45.0.0/16}"
ue_ipv4_gateway="${UE_IPV4_GATEWAY:-10.45.0.1}"
# ogstun is managed by this script. Remove its previous IPv4 NAT rule when
# changing subnets, without touching unrelated interfaces or firewall rules.
for previous_subnet in $(ip -4 route show dev ogstun proto kernel scope link | awk '{print $1}'); do
    if [ "$previous_subnet" != "$ue_ipv4_subnet" ]; then
        while iptables -t nat -C POSTROUTING -s "$previous_subnet" ! -o ogstun -j MASQUERADE 2>/dev/null; do
            iptables -t nat -D POSTROUTING -s "$previous_subnet" ! -o ogstun -j MASQUERADE
        done
    fi
done
ip -4 addr flush dev ogstun scope global
ip addr add "${ue_ipv4_gateway}/${ue_ipv4_subnet##*/}" dev ogstun
ip addr del 2001:db8:cafe::1/48 dev ogstun 2> /dev/null || true
ip addr add 2001:db8:cafe::1/48 dev ogstun
ip link set ogstun up

ensure_iptables_rule nat POSTROUTING -s "$ue_ipv4_subnet" ! -o ogstun -j MASQUERADE
ensure_iptables_rule filter INPUT -i ogstun -j ACCEPT
ensure_iptables_rule filter FORWARD -i ogstun -j ACCEPT
ensure_iptables_rule filter FORWARD -o ogstun -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT

if command -v ip6tables > /dev/null 2>&1; then
    ensure_ip6tables_rule nat POSTROUTING -s 2001:db8:cafe::/48 ! -o ogstun -j MASQUERADE || true
    ensure_ip6tables_rule filter INPUT -i ogstun -j ACCEPT || true
    ensure_ip6tables_rule filter FORWARD -i ogstun -j ACCEPT || true
    ensure_ip6tables_rule filter FORWARD -o ogstun -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT || true
fi
