#!/usr/bin/env python3
"""Explicit ONLINE refresh of pinned build inputs. Normal image builds never call this."""
import argparse
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vendor import ROOT, VENDOR, digest, unpack

SOURCES = {
    'node': ('19.9.0', 'https://nodejs.org/download/release/v19.9.0/node-v19.9.0-linux-x64.tar.xz',
             'e8ea737ee15a62c8752159d12765ed757b5d4b18036b1011d9e4c9e692a4e58d', ['webui'], 'LICENSE'),
    'python': ('3.12.15', 'https://www.python.org/ftp/python/3.12.15/Python-3.12.15.tar.xz',
               'c2c4321961fab0fb999d66e0cecf521c2ab3994c7992873ea99e306c1094fd5a', ['monitor'], 'LICENSE'),
    'wireshark': ('4.6.9', 'https://www.wireshark.org/download/src/wireshark-4.6.9.tar.xz',
                  'd062d416a1ab3a49975aafe3a671c882dde0562ccd71b078808ceb59d465fc0f', ['monitor'], 'COPYING'),
    'wait': ('2.9.0', 'https://github.com/ufoscout/docker-compose-wait/releases/download/2.9.0/wait',
             'b945d1bc2b6bac5b8998bdc56791a66d99085432a6b164ef6726c52653f4b8d5', ['webui'], 'Apache-2.0'),
}
PROJECTS = {
    'freeDiameter': ('https://github.com/open5gs/freeDiameter.git', '14725af3ba0edbf9ff61c4e3239ed42464423b2e', 'LICENSE'),
    'prometheus-client-c': ('https://github.com/open5gs/prometheus-client-c.git', 'a58ba25bf87a9b1b7c6be4e6f4c62047d620f402', 'LICENSE'),
}


def run(args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def download(url, target, expected=None):
    if not target.is_file() or (expected and digest(target) != expected):
        partial = target.with_suffix(target.suffix + '.partial')
        run(['curl', '-fL', '--retry', '4', '--connect-timeout', '20', '--max-time', '900', '-o', partial, url])
        if expected and digest(partial) != expected:
            raise ValueError('upstream SHA-256 mismatch: ' + url)
        partial.replace(target)


def archive_tree(path, target, prefix):
    # Stable tar ownership/timestamps; no host credentials or Git metadata.
    with target.open('wb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode='w') as archive:
            for filename in sorted(path.rglob('*')):
                if '.git' in filename.relative_to(path).parts:
                    continue
                member = archive.gettarinfo(str(filename), prefix + '/' + str(filename.relative_to(path)))
                member.uid = member.gid = member.mtime = 0
                member.uname = member.gname = ''
                with filename.open('rb') if member.isfile() else open(os.devnull, 'rb') as source:
                    archive.addfile(member, source if member.isfile() else None)


def record(data, name, path, components, **metadata):
    maximum = 64 * 1024 * 1024
    paths = [path]
    if path.stat().st_size > maximum:
        paths = []
        with path.open('rb') as source:
            index = 0
            while block := source.read(maximum):
                part = path.with_name(path.name + '.part%03d' % index)
                part.write_bytes(block)
                paths.append(part)
                index += 1
        path.unlink()
    data['assets'][name] = dict(metadata, components=components, files=[
        {'path': str(p.relative_to(VENDOR)), 'sha256': digest(p), 'bytes': p.stat().st_size} for p in paths])


def save(data):
    (VENDOR / 'manifest.json').write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources-only', action='store_true', help='prepare source archives before npm/pip caches')
    args = parser.parse_args()
    VENDOR.mkdir(parents=True, exist_ok=True)
    assets = VENDOR / 'archives'
    assets.mkdir(exist_ok=True)
    data = {'format': 1, 'platform': 'linux/amd64', 'assets': {}, 'locks': {}}
    for name, (version, url, expected, components, license_name) in SOURCES.items():
        path = assets / (url.rsplit('/', 1)[1] if name != 'wait' else 'wait-2.9.0')
        download(url, path, expected)
        record(data, name, path, components, version=version, source=url, license=license_name)
        save(data)
    # Preserve the exact, already validated upstream commits, never mutable wrap branches.
    for name, (url, commit, license_name) in PROJECTS.items():
        target = assets / (name + '-' + commit + '.tar.gz')
        if not target.exists():
            with tempfile.TemporaryDirectory(prefix='xcn-vendor-project-') as directory:
                temp = Path(directory)
                local = ROOT / 'subprojects' / name
                gitdir = temp / 'git'
                if (local / '.git').is_dir():
                    shutil.copytree(local / '.git', gitdir)
                    head = subprocess.check_output(['git', '--git-dir=' + str(gitdir), 'rev-parse', 'HEAD'], text=True).strip()
                    if head != commit:
                        raise ValueError(name + ' local commit differs from the pinned version')
                    dirty = subprocess.check_output(['git', '--git-dir=' + str(gitdir), '--work-tree=' + str(local), 'diff', 'HEAD', '--'], text=True)
                    if dirty:
                        raise ValueError(name + ' has local patches; review and pin them before refresh')
                else:
                    run(['git', 'init', '--bare', gitdir], stdout=subprocess.DEVNULL)
                    run(['git', '--git-dir=' + str(gitdir), 'fetch', '--depth=1', url, commit])
                with target.open('wb') as raw, gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as zipped:
                    tar = subprocess.check_output(['git', '--git-dir=' + str(gitdir), 'archive', '--format=tar', '--prefix=' + name + '/', commit])
                    zipped.write(tar)
        record(data, name, target, ['runtime'], version=commit, source=url, license=license_name)
        save(data)
    license_path = assets / 'wait-2.9.0.LICENSE'
    download('https://raw.githubusercontent.com/ufoscout/docker-compose-wait/2.9.0/LICENSE', license_path)
    record(data, 'wait-license', license_path, ['webui'], version='2.9.0', license='Apache-2.0',
           source='https://github.com/ufoscout/docker-compose-wait/tree/2.9.0')
    save(data)
    if args.sources_only:
        print('Source assets prepared; run again without --sources-only for npm/pip dependencies')
        return
    with tempfile.TemporaryDirectory(prefix='xcn-vendor-cache-') as directory:
        temp = Path(directory)
        unpack('node', temp)
        node_bin = temp / 'node-v19.9.0-linux-x64/bin'
        env = dict(os.environ, PATH=str(node_bin) + os.pathsep + os.environ['PATH'], NEXT_TELEMETRY_DISABLED='1')
        web = temp / 'webui'
        web.mkdir()
        for name in ('package.json', 'package-lock.json'):
            shutil.copy2(ROOT / 'webui' / name, web / name)
        cache = temp / 'npm-cache'
        run([node_bin / 'npm', 'ci', '--ignore-scripts', '--no-audit', '--fund=false', '--cache', cache], cwd=web, env=env)
        # Only content-addressed public packages/indexes, no logs, .npmrc or user credentials.
        clean_cache = temp / 'public-npm-cache'
        clean_cache.mkdir()
        shutil.copytree(cache / '_cacache', clean_cache / '_cacache')
        cache_path = assets / 'npm-cache.tar.gz'
        archive_tree(clean_cache, cache_path, 'npm-cache')
        record(data, 'npm-cache', cache_path, ['webui'], source='webui/package-lock.json', version='npm-9.6.3')
        wheel_dir = temp / 'wheels'
        wheel_dir.mkdir()
        locked = []
        for line in (ROOT / 'monitor/requirements.lock').read_text().splitlines():
            if line and not line.startswith('#'):
                locked.append(line.split(' --hash=')[0])
        input_lock = temp / 'python.in'
        input_lock.write_text('\n'.join(locked) + '\n')
        run([sys.executable, '-m', 'pip', 'download', '--only-binary=:all:', '--no-deps', '--python-version', '3.12',
             '--implementation', 'cp', '--abi', 'cp312', '--platform', 'manylinux_2_17_x86_64',
             '--platform', 'manylinux_2_28_x86_64', '--platform', 'manylinux_2_34_x86_64',
             '--platform', 'manylinux_2_35_x86_64', '--platform', 'manylinux2014_x86_64',
             '--dest', wheel_dir, '-r', input_lock])
        hashes = {}
        for wheel in wheel_dir.glob('*.whl'):
            name, version = wheel.name.split('-')[:2]
            hashes[(name.lower().replace('_', '-'), version)] = digest(wheel)
        result = ['# Pinned Linux/amd64 CPython 3.12 dependencies; refreshed by docker/prepare-offline-deps.py.']
        for line in locked:
            name, version = line.split('==')
            result.append(line + ' --hash=sha256:' + hashes[(name.lower().replace('_', '-'), version)])
        (ROOT / 'monitor/requirements.lock').write_text('\n'.join(result) + '\n')
        wheel_path = assets / 'python-wheels.tar.gz'
        archive_tree(wheel_dir, wheel_path, 'wheels')
        record(data, 'python-wheels', wheel_path, ['monitor'], source='https://pypi.org/', version='CPython-3.12-linux-amd64')
    for filename, components in [('webui/package-lock.json', ['webui']), ('webui/package.json', ['webui']),
                                  ('monitor/requirements.txt', ['monitor']), ('monitor/requirements.lock', ['monitor'])]:
        data['locks'][filename] = {'sha256': digest(ROOT / filename), 'components': components}
    save(data)
    print('Offline dependencies prepared; run python3 docker/vendor.py verify all')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
