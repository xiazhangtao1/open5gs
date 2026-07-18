#!/bin/sh
set -eu

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

ip addr del 10.45.0.1/16 dev ogstun 2> /dev/null || true
ip addr add 10.45.0.1/16 dev ogstun
ip addr del 2001:db8:cafe::1/48 dev ogstun 2> /dev/null || true
ip addr add 2001:db8:cafe::1/48 dev ogstun
ip link set ogstun up

ensure_iptables_rule nat POSTROUTING -s 10.45.0.0/16 ! -o ogstun -j MASQUERADE
ensure_iptables_rule filter INPUT -i ogstun -j ACCEPT
ensure_iptables_rule filter FORWARD -i ogstun -j ACCEPT
ensure_iptables_rule filter FORWARD -o ogstun -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT

if command -v ip6tables > /dev/null 2>&1; then
    ensure_ip6tables_rule nat POSTROUTING -s 2001:db8:cafe::/48 ! -o ogstun -j MASQUERADE || true
    ensure_ip6tables_rule filter INPUT -i ogstun -j ACCEPT || true
    ensure_ip6tables_rule filter FORWARD -i ogstun -j ACCEPT || true
    ensure_ip6tables_rule filter FORWARD -o ogstun -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT || true
fi
