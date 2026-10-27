#!/bin/sh
# Configure only this build stage. OFFLINE never falls back to a public mirror.
set -eu
case "${OFFLINE:-0}" in 0|1) ;; *) echo 'OFFLINE must be 0 or 1' >&2; exit 1 ;; esac
if [ "${OFFLINE:-0}" = 1 ] || [ -n "${APT_MIRROR:-}" ]; then
    cp /opt/xcn-build/sources.list /etc/apt/sources.list
    if [ -n "${APT_MIRROR:-}" ]; then
        case "$APT_MIRROR" in http://*|https://*) ;; *) echo 'APT_MIRROR must be an HTTP(S) URL' >&2; exit 1 ;; esac
        # Validate before sed; keep addresses as data, without shell evaluation.
        case "$APT_MIRROR" in *[!a-zA-Z0-9:/._~-]*) echo 'Invalid APT_MIRROR' >&2; exit 1 ;; esac
        sed -i "s|http://192.168.10.36/ubuntu_22_04/|${APT_MIRROR%/}/|g" /etc/apt/sources.list
    fi
    rm -f /etc/apt/sources.list.d/ubuntu.sources
fi
if [ "${OFFLINE:-0}" = 1 ]; then
    printf '%s\n' 'Acquire::http::Proxy "DIRECT";' 'Acquire::https::Proxy "DIRECT";' > /etc/apt/apt.conf.d/99xcn-offline
fi
