#!/usr/bin/env python3
"""Check training sources and Helm examples; optional low-rate loopback iperf.

No cluster mutations. --iperf binds only 127.0.0.1 and runs at 1 Mbit/s.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
from markdown_it import MarkdownIt
import yaml

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent


def run(command, **kwargs):
    p = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, timeout=40, **kwargs)
    if p.returncode:
        raise RuntimeError(f'{command}: {p.stdout}')
    return p.stdout


def source_check():
    source = (ROOT / '5G通信组网与运维培训.md').read_text()
    tokens = MarkdownIt('commonmark').enable('table').parse(source)
    fences = [t for t in tokens if t.type == 'fence' and t.info == 'bash']
    for t in fences:
        run(['bash', '-n'], input=t.content)
    images = re.findall(r'!\[[^\]]*\]\(([^)]+)\)', source)
    assert len(images) == 15
    assert all((ROOT / image).is_file() for image in images)
    assert not re.search(r'VPP\s*抓包|pcap\s+trace|trace\s+add', source, re.I)
    assert len(re.findall(r'^## [1-6]\. ', source, re.M)) == 6
    return {'shell_blocks': len(fences), 'images': len(images), 'chapters': 6,
            'vpp_capture_section': 'removed'}


def helm_check():
    lint = run(['helm', 'lint', './helm/xcn'], cwd=REPO)
    common = ['helm', 'template', 'training', './helm/xcn', '-n', 'training']
    values = {'fivegc.hostNetwork.enabled': 'true',
              'networking.amf.ngap.serverAddress': '192.168.1.20',
              'networking.upf.n3.address': '192.168.2.20'}
    results = []
    for mode, nat in [('tun', True), ('memif', True), ('memif', False)]:
        flags = dict(values, **{'networking.upf.mode': mode})
        if mode == 'memif':
            flags.update({'vpp.n3.interfaceAddress': '192.168.2.21/24',
                          'vpp.n6.externalAddress': '198.51.100.1/24',
                          'vpp.sriov.resourceName': 'intel.com/external_network',
                          'vpp.sriov.deviceEnv': 'PCIDEVICE_INTEL_COM_EXTERNAL_NETWORK',
                          'vpp.n6.nat44.enabled': str(nat).lower()})
        command = common + [x for k, v in flags.items() for x in ('--set', k + '=' + v)]
        rendered = run(command, cwd=REPO)
        objects = list(yaml.safe_load_all(rendered))
        deployment = next(o for o in objects if o and o['kind'] == 'Deployment'
                          and o['metadata']['name'].endswith('-5gc'))
        containers = deployment['spec']['template']['spec']['containers']
        vpp = next((c for c in containers if c['name'] == 'vpp'), None)
        if mode == 'tun':
            assert vpp is None
            assert all('intel.com/external_network' not in c.get('resources', {}).get('requests', {})
                       for c in containers)
        else:
            assert int(vpp['resources']['requests']['intel.com/external_network']) == 2
            config = next(o for o in objects if o and o['kind'] == 'ConfigMap'
                          and 'vpp-cli-commands.conf.template' in o.get('data', {}))
            cli = config['data']['vpp-cli-commands.conf.template']
            assert 'set interface state memif2/0 up' in cli
            assert 'set interface state memif1/0 up' in cli
            assert ('nat44 plugin enable' in cli) == nat
            assert ('nat44 add interface address dpdk-n6' in cli) == nat
        results.append({'mode': mode, 'nat': nat if mode == 'memif' else 'Linux initialization',
                        'status': 'pass', 'objects': len(objects)})
    return {'lint': lint.strip(), 'cases': results}


def iperf_check(log_directory):
    log_directory.mkdir(parents=True, exist_ok=True)
    cases = []
    for tool in ('iperf', 'iperf3'):
        for protocol, direction in [('tcp', 'forward'), ('tcp', 'reverse'),
                                    ('udp', 'forward'), ('udp', 'reverse'),
                                    ('tcp', 'bidirectional')]:
            with socket.socket() as s:
                s.bind(('127.0.0.1', 0))
                port = str(s.getsockname()[1])
            server = [tool, '-s', '-B', '127.0.0.1', '-p', port]
            if tool == 'iperf' and protocol == 'udp':
                server += ['-u']
            client = [tool, '-c', '127.0.0.1', '-p', port, '-t', '2', '-b', '1M']
            if protocol == 'udp':
                client += ['-u', '-l', '1200']
            if direction == 'reverse':
                client += ['-R']
            if direction == 'bidirectional':
                client += ['--bidir' if tool == 'iperf3' else '--full-duplex']
            name = f'{tool}-{protocol}-{direction}'
            with tempfile.TemporaryFile(mode='w+') as server_log:
                process = subprocess.Popen(server, stdout=server_log, stderr=subprocess.STDOUT, text=True)
                try:
                    time.sleep(.4)
                    if process.poll() is not None:
                        raise RuntimeError('Server exited: ' + name)
                    output = run(client)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                server_log.seek(0)
                server_output = server_log.read()
            assert 'bits/sec' in output, output
            assert not re.search(r'failed|unable to|connect failed', output, re.I), output
            (log_directory / (name + '.txt')).write_text(
                'SERVER: ' + ' '.join(server) + '\nCLIENT: ' + ' '.join(client) +
                '\n\nCLIENT OUTPUT\n' + output + '\nSERVER OUTPUT\n' + server_output)
            cases.append({'tool': tool, 'protocol': protocol, 'direction': direction,
                          'target': '127.0.0.1', 'rate': '1M', 'seconds': 2, 'status': 'pass'})
    return cases


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--iperf', action='store_true')
    p.add_argument('--output', type=Path, default=Path('/tmp/xcn-training-validation'))
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    result = {'source': source_check(), 'helm': helm_check()}
    if args.iperf:
        result['iperf'] = iperf_check(args.output / 'iperf')
    (args.output / 'checks.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(result, indent=2, ensure_ascii=False))
