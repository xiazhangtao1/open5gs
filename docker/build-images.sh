#!/usr/bin/env bash
# Build using committed dependencies; no implicit dependency refresh.
set -euo pipefail
TASK_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TAG=
REGISTRY=192.168.10.35/xcn
OFFLINE=0
CPUSET=
usage() {
    printf '%s\n' 'Usage: docker/build-images.sh --tag TAG [--offline] [--registry PREFIX] [--cpuset CPUS]' \
        'Defaults: Ubuntu 192.168.10.35/xnet/ubuntu:22.04; VPP 192.168.10.35/xcn/vpp:26.06' \
        'Optional environment: UBUNTU_IMAGE, VPP_IMAGE, APT_MIRROR, BUILD_JOBS (2),' \
        'BUILD_MEMORY (4g), BUILD_CPU_QUOTA (150000 us/100000 us), PUSH_TIMEOUT (60s),' \
        'BUILD_NETWORK (host; use an isolated Docker network for offline validation).'
}
while (($#)); do
    case "$1" in
        --tag|--registry|--cpuset)
            (($# >= 2)) || { usage >&2; exit 2; }
            case "$1" in --tag) TAG=$2 ;; --registry) REGISTRY=$2 ;; --cpuset) CPUSET=$2 ;; esac
            shift 2 ;;
        --offline) OFFLINE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ $TAG =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]] || { echo 'A valid --tag is required' >&2; exit 2; }
[[ $REGISTRY =~ ^[a-z0-9][a-z0-9.:/_-]*[a-z0-9]$ && $REGISTRY != *//* ]] || { echo 'Invalid --registry prefix' >&2; exit 2; }
for command in docker python3 timeout; do command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }; done
[[ ${BUILD_JOBS:-2} =~ ^[1-9][0-9]*$ && ${BUILD_CPU_QUOTA:-150000} =~ ^[1-9][0-9]*$ && ${PUSH_TIMEOUT:-60} =~ ^[1-9][0-9]*$ ]] || { echo 'Invalid build/push resource limit' >&2; exit 2; }
if [[ -n $CPUSET ]]; then
    python3 - "$CPUSET" <<'PY'
import os,re,sys
value=sys.argv[1]
if not re.fullmatch(r'\d+(?:-\d+)?(?:,\d+(?:-\d+)?)*',value):
    sys.exit('Invalid --cpuset; use 36,37 or 36-37')
cpus=set()
for part in value.split(','):
    ends=list(map(int,part.split('-')));start=ends[0];end=ends[-1]
    if end<start or end-start>65535:
        sys.exit('Invalid --cpuset range')
    cpus.update(range(start,end+1))
if not cpus.issubset(os.sched_getaffinity(0)):
    sys.exit('--cpuset includes CPUs unavailable to this process')
PY
fi
cd "$TASK_ROOT"
python3 docker/vendor.py verify all
UBUNTU_IMAGE=${UBUNTU_IMAGE:-192.168.10.35/xnet/ubuntu:22.04}
VPP_IMAGE=${VPP_IMAGE:-192.168.10.35/xcn/vpp:26.06}
for base in "$UBUNTU_IMAGE" "$VPP_IMAGE"; do
    if ! docker image inspect "$base" >/dev/null 2>&1; then
        printf 'Pulling required base image: %s\n' "$base"
        timeout 180 docker pull "$base" || { printf 'Base image unavailable: %s; preload it or specify its intranet address.\n' "$base" >&2; exit 1; }
    fi
    platform=$(docker image inspect --format '{{.Os}}/{{.Architecture}}' "$base")
    [[ $platform = linux/amd64 ]] || { printf 'Unsupported base image platform: %s (%s)\n' "$base" "$platform" >&2; exit 1; }
done
# Use preloaded images without remote frontend/metadata lookups. The local builder
# also enforces per-build cgroup limits (BuildKit ignores several legacy limits).
export DOCKER_BUILDKIT=0
COMMON=(--network "${BUILD_NETWORK:-host}" --pull=false --cpu-period 100000 --cpu-quota "${BUILD_CPU_QUOTA:-150000}"
        --memory "${BUILD_MEMORY:-4g}" --memory-swap "${BUILD_MEMORY:-4g}"
        --build-arg "UBUNTU_IMAGE=$UBUNTU_IMAGE" --build-arg "VPP_IMAGE=$VPP_IMAGE"
        --build-arg "OFFLINE=$OFFLINE" --build-arg "APT_MIRROR=${APT_MIRROR:-}"
        --build-arg "BUILD_CPUSET=$CPUSET" --build-arg "BUILD_JOBS=${BUILD_JOBS:-2}")
[[ -z $CPUSET ]] || COMMON+=(--cpuset-cpus "$CPUSET")
for key in HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY http_proxy https_proxy all_proxy no_proxy; do
    if ((OFFLINE)); then COMMON+=(--build-arg "$key="); else COMMON+=(--build-arg "$key"); fi
done
SUMMARY=()
for name in runtime webui monitor; do
    case "$name" in runtime) file=docker/runtime/Dockerfile ;; webui) file=docker/webui/Dockerfile ;; monitor) file=monitor/Dockerfile ;; esac
    image="$REGISTRY/xcn-$name:$TAG"
    printf '\nBuilding %s (offline=%s)\n' "$image" "$OFFLINE"
    if docker build "${COMMON[@]}" -f "$file" -t "$image" .; then
        if timeout "${PUSH_TIMEOUT:-60}" docker push "$image"; then
            SUMMARY+=("$image: build OK, push OK")
        else
            SUMMARY+=("$image: build OK, push FAILED (local image retained)")
            printf 'WARNING: push failed for %s; continuing with the next image.\n' "$image" >&2
        fi
    else
        status=$?
        printf 'BUILD FAILED: %s\n' "$image" >&2
        ((${#SUMMARY[@]} == 0)) || printf '%s\n' "${SUMMARY[@]}"
        exit "$status"
    fi
done
printf '\n%s\n' 'Build/push results:'
printf '%s\n' "${SUMMARY[@]}"
