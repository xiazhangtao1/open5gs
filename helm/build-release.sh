#!/usr/bin/env bash
# Build the three application images and package an offline Helm release.
set -euo pipefail

HELM_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$HELM_DIR/.." && pwd)

usage() {
    printf '%s\n' 'Usage: helm/build-release.sh <chartVersion> <appVersion>' \
        'chartVersion: SemVer, e.g. 0.2.0; appVersion: Docker tag, e.g. 26.11.05.' \
        'Outputs in helm/: xcn-runtime.tar, xcn-webui.tar, xcn-monitor.tar, xcn-<chartVersion>.tgz.'
}

if (($# == 1)) && [[ $1 == -h || $1 == --help ]]; then
    usage
    exit 0
fi
(($# == 2)) || { usage >&2; exit 2; }
CHART_VERSION=$1
APP_VERSION=$2

for command in docker helm python3; do
    command -v "$command" >/dev/null || { printf 'Missing command: %s\n' "$command" >&2; exit 1; }
done
python3 - "$CHART_VERSION" "$APP_VERSION" <<'PY'
import re
import sys

number = r"(?:0|[1-9][0-9]*)"
identifier = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
semver = (rf"{number}\.{number}\.{number}"
          rf"(?:-{identifier}(?:\.{identifier})*)?"
          r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?")
if not re.fullmatch(semver, sys.argv[1]):
    sys.exit('Invalid chartVersion: use SemVer, e.g. 0.2.0 or 0.2.0-rc.1')
if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}', sys.argv[2]):
    sys.exit('Invalid appVersion: a valid Docker tag is required')
try:
    import yaml
except ImportError:
    sys.exit('Missing PyYAML: install with sudo apt-get install python3-yaml')
PY
docker info >/dev/null

STAGING_DIR=$(mktemp -d "$HELM_DIR/.build-release.XXXXXX")
trap 'rm -rf -- "$STAGING_DIR"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cp -a -- "$HELM_DIR/xcn" "$STAGING_DIR/xcn"
python3 - "$STAGING_DIR/xcn/values.yaml" "$APP_VERSION" <<'PY'
from pathlib import Path
import sys

import yaml

path = Path(sys.argv[1])
values = yaml.safe_load(path.read_text())
for name in ('runtime', 'webui'):
    values['images'][name]['tag'] = sys.argv[2]
values['monitor']['image']['tag'] = sys.argv[2]
path.write_text(yaml.safe_dump(values, sort_keys=False))
PY

"$REPO_ROOT/docker/build-images.sh" --image all --offline --tag "$APP_VERSION"

ARTIFACTS=()
for name in runtime webui monitor; do
    image="192.168.10.35/xcn/xcn-$name:$APP_VERSION"
    archive="xcn-$name.tar"
    printf 'Saving %s to %s\n' "$image" "$archive"
    docker save -o "$STAGING_DIR/$archive" "$image"
    ARTIFACTS+=("$archive")
done

helm package "$STAGING_DIR/xcn" --version "$CHART_VERSION" \
    --app-version "$APP_VERSION" --destination "$STAGING_DIR"
ARTIFACTS+=("xcn-$CHART_VERSION.tgz")

for artifact in "${ARTIFACTS[@]}"; do
    mv -f -- "$STAGING_DIR/$artifact" "$HELM_DIR/$artifact"
done
printf '\nRelease artifacts:\n'
printf '%s\n' "${ARTIFACTS[@]/#/$HELM_DIR/}"
