#!/usr/bin/env python3
"""Verify and unpack the committed Linux/amd64 build dependencies (no network)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tarfile
import sys

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'docker/vendor'


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def manifest():
    try:
        data = json.loads((VENDOR / 'manifest.json').read_text())
    except FileNotFoundError:
        raise ValueError('missing docker/vendor/manifest.json; run docker/prepare-offline-deps.py online first')
    if data.get('format') != 1 or data.get('platform') != 'linux/amd64':
        raise ValueError('unsupported vendor manifest format/platform')
    return data


def asset_files(asset):
    for item in asset['files']:
        path = VENDOR / item['path']
        if not path.resolve().is_relative_to(VENDOR.resolve()):
            raise ValueError('invalid vendor path')
        if not path.is_file():
            raise ValueError('missing offline asset: ' + str(path.relative_to(ROOT)))
        if digest(path) != item['sha256']:
            raise ValueError('SHA-256 mismatch: ' + str(path.relative_to(ROOT)))
        yield path


def verify(component):
    data = manifest()
    selected = [v for v in data['assets'].values()
                if component == 'all' or component in v['components']]
    if not selected:
        raise ValueError('no assets for component ' + component)
    for asset in selected:
        list(asset_files(asset))
    for name, entry in data['locks'].items():
        if component == 'all' or component in entry['components']:
            path = ROOT / name
            if not path.is_file() or digest(path) != entry['sha256']:
                raise ValueError(name + ' changed; refresh offline dependencies before building')
    return len(selected)


def unpack(name, destination):
    """Extract only hash-verified archives; reject escaping links and paths."""
    asset = manifest()['assets'][name]
    paths = list(asset_files(asset))
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    # Individual assets are currently below 64 MiB; support verified split archives too.
    import tempfile
    with tempfile.TemporaryFile() as combined:
        for path in paths:
            with path.open('rb') as source:
                import shutil
                shutil.copyfileobj(source, combined)
        combined.seek(0)
        with tarfile.open(fileobj=combined, mode='r:*') as archive:
            for member in archive.getmembers():
                target = destination / member.name
                if not target.resolve().is_relative_to(destination):
                    raise ValueError('archive path escapes destination: ' + member.name)
                if member.isdev() or member.isfifo():
                    raise ValueError('unsupported special file in archive')
                if member.issym() or member.islnk():
                    link = (target.parent if member.issym() else destination) / member.linkname
                    if not link.resolve().is_relative_to(destination):
                        raise ValueError('archive link escapes destination: ' + member.name)
                # Resolve again after earlier members, including symlinks, were created.
                member.uid = os.getuid()
                member.gid = os.getgid()
                member.uname = member.gname = ''
                member.mode &= 0o777
                archive.extract(member, destination, set_attrs=True, numeric_owner=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    check = commands.add_parser('verify')
    check.add_argument('component', choices=['all', 'runtime', 'webui', 'monitor'], default='all', nargs='?')
    extract = commands.add_parser('unpack')
    extract.add_argument('asset')
    extract.add_argument('destination')
    args = parser.parse_args()
    try:
        if args.command == 'verify':
            print('Verified %d offline assets (%s)' % (verify(args.component), args.component))
        else:
            unpack(args.asset, args.destination)
    except (ValueError, KeyError, OSError, tarfile.TarError) as error:
        print('Offline dependencies: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
