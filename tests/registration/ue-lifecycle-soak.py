#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Run only inside an isolated 5GC test network and database."""
import argparse
import collections
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import urllib.request
import urllib.error

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, required=True, help='Built registration test executable')
parser.add_argument('--config', type=Path, required=True, help='Isolated test sample YAML')
parser.add_argument('--output', type=Path, required=True, help='New directory for evidence')
parser.add_argument('--seconds', type=int, default=1800)
parser.add_argument('--ue-count', type=int, help='Fixed count; otherwise scale 16..1024')
args = parser.parse_args()
if not 1 <= args.seconds <= 86400:
    parser.error('--seconds must be between 1 and 86400')
if args.ue_count is not None and not 1 <= args.ue_count <= 1024:
    parser.error('--ue-count must be between 1 and 1024')
if not args.binary.is_file() or not args.config.is_file():
    parser.error('--binary and --config must exist')
base = args.output.resolve()
base.mkdir(parents=True, exist_ok=False)
duration = args.seconds
env = dict(os.environ, NO_PROXY='*', no_proxy='*', XCN_STRESS_SECONDS=str(duration))
if args.ue_count is not None:
    env['XCN_STRESS_UE_COUNT'] = str(args.ue_count)
else:
    env.pop('XCN_STRESS_UE_COUNT', None)
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def fetch(address, endpoint, as_json=True):
    with opener.open('http://' + address + ':9090/' + endpoint, timeout=1) as response:
        data = response.read()
    return json.loads(data) if as_json else data.decode()


def children(parent):
    found = {}
    for path in Path('/proc').iterdir():
        if not path.name.isdigit():
            continue
        try:
            fields = dict(line.split(':', 1) for line in (path / 'status').read_text().splitlines())
            name = fields['Name'].strip()
            if int(fields['PPid']) != parent or not name.startswith('xcn-'):
                continue
            found[name] = dict(pid=int(path.name), rss_kib=int(fields.get('VmRSS', '0 kB').split()[0]),
                               hwm_kib=int(fields.get('VmHWM', '0 kB').split()[0]),
                               threads=int(fields['Threads']), fds=len(list((path / 'fd').iterdir())),
                               state=fields['State'].strip())
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return found


def metrics(address, names):
    data = fetch(address, 'metrics', False)
    result = {}
    for name in names:
        values = re.findall(r'^' + re.escape(name) + r'(?:\{[^\n]*\})?\s+([-+\d.eE]+)', data, re.M)
        result[name] = sum(float(v) for v in values) if values else None
    return result


counts = collections.Counter()
sessions = {}
cursors = {}
known_pids = {}
active_verified = {}
clean_verified = {}
checkpoints = []
last_marker = None
terminal = False
failure = None
sample_errors = 0
last_good_events = time.monotonic()
started = time.monotonic()
event_output = (base / 'events.jsonl').open('w')
sample_output = (base / 'resources.jsonl').open('w')
log_path = base / 'protocol.log'
log = log_path.open('w')
log_offset = 0
log_tail = ''
process = subprocess.Popen([str(args.binary.resolve()), '-c', str(args.config.resolve()), 'multi-ue-test'],
                           stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                           env=env, start_new_session=True)
try:
    while process.poll() is None:
        now = time.monotonic()
        if now - started > duration + 600:
            raise RuntimeError('Long protocol test exceeded its deadline')
        with log_path.open(errors='replace') as reader:
            reader.seek(log_offset)
            text = log_tail + reader.read()
            log_offset = reader.tell()
        markers = re.findall(r'XCN_SOAK_(START|ACTIVE|CLEANUP|DONE) cycle=(\d+) ue=(\d+)(?: elapsed_s=(\d+))?', text)
        log_tail = text[-200:]
        marker = markers[-1] if markers else last_marker
        if marker and marker != last_marker:
            print('MARKER', marker, 'wall_s', round(now - started), flush=True)
            last_marker = marker
        terminal = bool(marker and marker[0] == 'DONE' and int(marker[3] or 0) >= duration)
        current = children(process.pid)
        for name, state in current.items():
            if not terminal and name in known_pids:
                assert state['pid'] == known_pids[name], ('NF restarted', name, state)
                assert not state['state'].startswith('Z'), ('NF crashed', name, state)
            known_pids.setdefault(name, state['pid'])
        if known_pids and marker and not terminal:
            for name in known_pids:
                assert name in current, ('NF disappeared', name)
        amf_metrics = smf_metrics = None
        try:
            amf_metrics = metrics('127.0.0.5', ['ran_ue', 'amf_session'])
            smf_metrics = metrics('127.0.0.4', ['ues_active', 'bearers_active', 'pfcp_sessions_active'])
        except Exception:
            sample_errors += 1
        for nf, address in [('AMF', '127.0.0.5'), ('SMF', '127.0.0.4')]:
            try:
                for page in range(4):
                    query = 'ue-events?detail=1&limit=1000'
                    if nf in cursors:
                        boot, cursor = cursors[nf]
                        query += '&boot_id=' + boot + '&after=' + str(cursor)
                    doc = fetch(address, query)
                    assert doc['enabled'] and doc['capacity'] == 65536 and doc['per_ue'] == 512
                    assert not doc['gap'], ('History collector fell behind', nf, doc['next_seq'])
                    previous = cursors.get(nf, (doc['boot_id'], 0))
                    assert doc['boot_id'] == previous[0], ('NF boot ID changed', nf)
                    seq = int(previous[1])
                    for event in doc['items']:
                        assert int(event['seq']) > seq, ('Duplicate or unordered event', nf, event)
                        seq = int(event['seq'])
                        counts[nf + '_events'] += 1
                        name, result = event['name'], event['result']
                        if nf == 'SMF':
                            assert event['psi'] == 5, event
                            key = event['trace_id']
                            state = sessions.setdefault(key, dict(n2=False, n4=False, completed=False))
                            if name == 'N2SMTransfer' and 'PDU_RES_SETUP_RSP' in event['info']:
                                state['n2'] = True
                            if name == 'SessionModificationResponse' and result == 'accepted':
                                state['n4'] = True
                            if name == 'SessionEstablishment' and result == 'completed':
                                assert state['n2'] and state['n4'] and not state['completed'], event
                                state['completed'] = True
                                counts['session_completed'] += 1
                            if name == 'SessionContext' and result == 'removed':
                                assert state['completed'], ('Session removed without successful setup', event)
                                counts['session_removed'] += 1
                                sessions.pop(key)
                        if nf == 'AMF' and name == 'Registration' and result == 'completed':
                            counts['registration_completed'] += 1
                        if name == 'InitialUEMessage':
                            assert 'AMF-UE-ID=' not in event['info'], event
                        assert not (name == 'SessionEstablishment' and result in ['rejected', 'failed', 'timeout']), event
                        if counts[nf + '_events'] <= 1000 or counts[nf + '_events'] % 100 == 0:
                            event_output.write(json.dumps(dict(nf=nf, **event)) + '\n')
                    cursors[nf] = doc['boot_id'], doc['next_seq']
                    last_good_events = now
                    counts[nf + '_overwritten'] = int(doc['overwritten_total'])
                    if not doc['has_more']:
                        break
            except (OSError, ValueError, urllib.error.URLError):
                sample_errors += 1
        if now - started > 30 and not terminal:
            assert now - last_good_events < 30, 'Event endpoint unavailable for 30 seconds'
        snapshot = dict(wall_s=round(now - started, 3), marker=marker, processes=current,
                        amf=amf_metrics, smf=smf_metrics, counters=dict(counts))
        sample_output.write(json.dumps(snapshot) + '\n')
        sample_output.flush()
        if marker and amf_metrics and smf_metrics:
            kind, cycle_text, ue_text, elapsed = marker
            cycle, ue_count = int(cycle_text), int(ue_text)
            if kind == 'ACTIVE' and amf_metrics['ran_ue'] == ue_count and smf_metrics['ues_active'] == ue_count and smf_metrics['pfcp_sessions_active'] == ue_count:
                if cycle not in active_verified:
                    active_verified[cycle] = ue_count
                    print('ACTIVE_VERIFIED', cycle, ue_count, flush=True)
            if kind == 'CLEANUP' and all(v == 0 for v in [amf_metrics['ran_ue'], amf_metrics['amf_session'], smf_metrics['ues_active'], smf_metrics['bearers_active'], smf_metrics['pfcp_sessions_active']]):
                if cycle not in clean_verified:
                    retained = []
                    page = 0
                    while True:
                        context_page = fetch('127.0.0.5', 'ue-info?page=' + str(page) + '&page_size=100')
                        retained.extend(context_page['items'])
                        if not context_page.get('pager', {}).get('next'):
                            break
                        page += 1
                        assert page <= 20, 'Unbounded AMF UE contexts'
                    assert len(retained) == ue_count, ('Unexpected retained AMF context count', len(retained), ue_count)
                    assert len({ue['supi'] for ue in retained}) == ue_count, 'Duplicate AMF identities'
                    assert all(ue['cm_state'] == 'idle' and ue['pdu_sessions_count'] == 0 for ue in retained), 'AMF cleanup incomplete'
                    snapshot['retained_amf_contexts'] = len(retained)
                    clean_verified[cycle] = ue_count
                    checkpoints.append(snapshot)
                    print('CLEANUP_VERIFIED', cycle, ue_count, 'rss_kib', {k:v['rss_kib'] for k,v in current.items() if k in ['xcn-amfd','xcn-smfd','xcn-upfd']}, flush=True)
        time.sleep(0.2)
    code = process.wait()
    assert code == 0, ('Protocol test failed', code)
    text = log_path.read_text(errors='replace')
    done = re.findall(r'XCN_SOAK_DONE cycle=(\d+) ue=(\d+) elapsed_s=(\d+)', text)
    assert done and int(done[-1][2]) >= duration, 'Test ended before requested duration'
    expected = {int(c):int(u) for c,u,_ in done}
    assert active_verified == expected, ('Missing active UE/session verification', active_verified, expected)
    assert clean_verified == expected, ('UE/session contexts did not drain', clean_verified, expected)
    assert counts['session_completed'] == counts['session_removed'] == counts['registration_completed'] == sum(expected.values()), counts
    assert not sessions, ('Session tracking left over', len(sessions))
    if duration >= 1800:
        assert counts['AMF_overwritten'] and counts['SMF_overwritten'], 'Real NF event history did not roll over'
    result = dict(status='passed', duration_s=int(done[-1][2]), cycles=expected,
                  total_ue_lifecycles=sum(expected.values()), counters=dict(counts),
                  pids=known_pids, samples_with_unavailable_endpoint=sample_errors,
                  cleanup_checkpoints=checkpoints)
    (base / 'summary.json').write_text(json.dumps(result, indent=2))
    print('LONG_SOAK_PASSED', {k:v for k,v in result.items() if k not in ['cleanup_checkpoints']}, flush=True)
except BaseException as exc:
    failure = str(exc)
    (base / 'failure.json').write_text(json.dumps(dict(error=failure, counters=dict(counts), active=active_verified, cleanup=clean_verified), indent=2))
    raise
finally:
    # A driver assertion can bypass its atexit handler; terminate every NF in
    # this isolated process group as well as the driver on any failure.
    if failure:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
    process.stdin.close()
    log.close()
    event_output.close()
    sample_output.close()
