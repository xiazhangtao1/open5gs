"""Read real UPF snapshots during an isolated UE lifecycle test (no NF mutations)."""
import argparse
import json
from pathlib import Path
import socket
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--socket', default='/run/xcn/upf-stats.sock')
parser.add_argument('--seconds', type=int, default=150)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
started = time.monotonic()
result = dict(snapshots=0, max_sessions=0, max_rules=0, max_lock_wait_us=0, max_lock_hold_us=0,
              bootstrap_or_outage=0, legacy_reads=0, versions=[])


def read(query):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(2)
        sock.connect(args.socket)
        sock.sendall(query + b'\n')
        chunks, size = [], 0
        while True:
            data = sock.recv(65536)
            if not data:
                break
            size += len(data)
            assert size <= 16 * 1024 * 1024
            chunks.append(data)
    return json.loads(b''.join(chunks))


while time.monotonic() - started < args.seconds:
    try:
        doc = read(b'level=snapshot json=1')
        if doc.get('ready'):
            assert doc['schema'] == 1 and not doc['truncated']
            assert isinstance(doc['boot_id'], str) and doc['boot_id']
            assert abs(time.time() * 1e6 - int(doc['timestamp_us'])) < 6000000
            rows = doc['sessions']
            result['snapshots'] += 1
            result['max_sessions'] = max(result['max_sessions'], len(rows))
            result['max_rules'] = max(result['max_rules'], sum(len(s['rules']) for s in rows))
            for key in ('lock_wait_us', 'lock_hold_us'):
                result['max_' + key] = max(result['max_' + key], doc[key])
            result['versions'].append(doc['version'])
            for s in rows:
                assert isinstance(s['seid'], str) and s['seid'].isdigit()
                assert isinstance(s['dnn'], str) and isinstance(s['supi'], str)
                for r in s['rules']:
                    assert r['direction'] in ('UL', 'DL')
                    assert all(isinstance(r[k], str) and r[k].isdigit() for k in ('bytes', 'packets', 'bps', 'pps'))
        else:
            result['bootstrap_or_outage'] += 1
        legacy = read(b'level=rule json=1')
        assert isinstance(legacy['rows'], list)
        result['legacy_reads'] += 1
    except (FileNotFoundError, ConnectionRefusedError):
        result['bootstrap_or_outage'] += 1
    time.sleep(2)
result['seconds'] = time.monotonic() - started
args.output.write_text(json.dumps(result, indent=2))
assert result['snapshots'] >= 10, result
assert result['max_sessions'] >= 50, result
print(json.dumps(result, indent=2))
