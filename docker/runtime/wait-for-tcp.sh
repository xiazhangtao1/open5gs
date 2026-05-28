#!/bin/sh
set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: wait-for-tcp.sh host:port [host:port ...]" >&2
    exit 1
fi

timeout_secs="${WAIT_TIMEOUT:-120}"
interval_secs="${WAIT_INTERVAL:-2}"

for target in "$@"; do
    host="${target%:*}"
    port="${target##*:}"
    start_ts="$(date +%s)"

    echo "waiting for ${host}:${port}"
    while ! nc -z "${host}" "${port}" >/dev/null 2>&1; do
        now_ts="$(date +%s)"
        if [ $((now_ts - start_ts)) -ge "${timeout_secs}" ]; then
            echo "timeout waiting for ${host}:${port}" >&2
            exit 1
        fi
        sleep "${interval_secs}"
    done
done
