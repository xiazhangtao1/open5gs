#!/usr/bin/env python3
"""Exercise the real CLI against cursor-based AMF/SMF event endpoints."""
import copy
import http.server
import json
import os
import subprocess
import sys
import threading
import time
import unittest
import urllib.parse

BINARY = os.path.abspath(sys.argv.pop(1))
BOOT = '00000000-0000-0000-0000-000000000001'
NEW_BOOT = '00000000-0000-0000-0000-000000000002'
SUPI = 'imsi-460110000000100'


def event(nf, seq, action=True, stamp=None, trace=1, protocol='ACTION', supi=SUPI):
    return dict(event_id=f'{nf}/{BOOT}/{seq}', seq=str(seq), trace_id=str(trace),
                parent_seq='0', flow_id='1', timestamp_us=stamp or 1792500000000000 + seq * 1000,
                action=action, supi=supi, protocol=protocol, direction='gNB->AMF',
                name='Registration' if action else 'InitialUEMessage', result='completed',
                cause='', info='', psi=2, gnb_id=3584, gnb_bits=28, nci='917838',
                amf_ngap_id='1', ran_ngap_id='1', upf_seid='18446744073709551615')


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        nf = 'AMF' if parsed.path.startswith('/amf/') else 'SMF'
        args = {key: values[0] for key, values in urllib.parse.parse_qs(parsed.query).items()}
        with self.server.lock:
            self.server.requests.append((nf, args))
            source = copy.deepcopy(self.server.sources[nf])
        if isinstance(source, tuple):
            status, body = source
        else:
            boot = source.get('boot', BOOT)
            latest = max([int(item['seq']) for item in source['items']] + [0])
            reset = 'boot_id' in args and args['boot_id'] != boot
            incremental = 'after' in args and not reset
            items = [item for item in source['items']
                     if ('supi' not in args or item['supi'] == args['supi'])
                     and ('trace_id' not in args or item['trace_id'] == args['trace_id'])
                     and ('trace_boot_id' not in args or args['trace_boot_id'] == boot)
                     and item['timestamp_us'] >= int(args.get('since_us', 0))
                     and (args.get('detail') == '1' or item['action'] or item['protocol'] == 'IDENTITY')
                     and (not incremental or int(item['seq']) > int(args['after']))]
            limit = int(args.get('limit', 100))
            more = len(items) > limit
            items = items[:limit] if incremental else items[-limit:]
            next_seq = int(items[-1]['seq']) if incremental and more else latest
            data = dict(nf=nf, boot_id=boot, items=items, has_more=more,
                        next_seq=str(next_seq), latest_seq=str(latest), reset=reset,
                        gap=source.get('gap', False), enabled=True)
            data.update(source.get('override', {}))
            status, body = 200, json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


class EventsTest(unittest.TestCase):
    def setUp(self):
        self.http = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.http.lock = threading.Lock()
        self.http.requests = []
        self.http.sources = {'AMF': {'items': [event('AMF', 1), event('AMF', 3, False)]},
                             'SMF': {'items': [event('SMF', 2)]}}
        self.thread = threading.Thread(target=self.http.serve_forever, kwargs={'poll_interval': 0.02})
        self.thread.start()

    def tearDown(self):
        self.http.shutdown()
        self.http.server_close()
        self.thread.join()

    def argv(self, *options):
        url = f'http://127.0.0.1:{self.http.server_port}'
        return [BINARY, 'show', 'ue-events', '--amf-ue-events', url + '/amf/ue-events',
                '--smf-ue-events', url + '/smf/ue-events', *options]

    def run_cli(self, *options, expected=0):
        r = subprocess.run(self.argv(*options), capture_output=True, text=True, timeout=10,
                           env=dict(os.environ, NO_PROXY='*', no_proxy='*'))
        self.assertEqual(r.returncode, expected, r.stdout + r.stderr)
        return r

    def test_summary_and_detail(self):
        summary = self.run_cli('--supi', SUPI, '--json')
        rows = json.loads(summary.stdout)['items']
        self.assertEqual([row['nf'] for row in rows], ['AMF', 'SMF'])
        self.assertEqual(rows[-1]['upf_seid'], '18446744073709551615')
        detail = self.run_cli('--supi', SUPI, '--detail', '--json')
        rows = json.loads(detail.stdout)['items']
        self.assertEqual([row['nf'] for row in rows], ['AMF', 'SMF', 'AMF'])
        text = self.run_cli('--detail').stdout
        self.assertIn('TIME(UTC)', text)
        self.assertIn('InitialUEMessage', text)
        self.assertIn('TRACE AMF/' + BOOT + '/1', text)
        self.assertIn('UPF-SEID 18446744073709551615', text)

    def test_limits_since_and_trace(self):
        rows = json.loads(self.run_cli('--json', '--detail', '--limit', '1').stdout)['items']
        self.assertEqual([row['seq'] for row in rows], ['3'])
        rows = json.loads(self.run_cli('--json', '--since', '2026-10-20T13:00:01Z').stdout)['items']
        self.assertEqual(rows, [])
        rows = json.loads(self.run_cli('--json', '--detail', '--trace-id', f'AMF/{BOOT}/1').stdout)['items']
        self.assertEqual([row['nf'] for row in rows], ['AMF', 'AMF'])
        self.run_cli('--json', '--since', '30m')
        self.assertTrue(any('since_us' in q for _, q in self.http.requests))

    def test_partial_and_unavailable(self):
        self.http.sources['SMF'] = (503, b'unavailable')
        result = self.run_cli('--json')
        self.assertTrue(json.loads(result.stdout)['partial'])
        self.assertIn('SMF', result.stderr)
        self.http.sources['AMF'] = (503, b'unavailable')
        self.run_cli(expected=1)

    def test_malformed_pages_are_not_empty_history(self):
        for override in ({'items': {}}, {'has_more': 'yes'}, {'boot_id': 'bogus'},
                         {'next_seq': '-1'}, {'nf': 'SMF'}):
            with self.subTest(override=override):
                self.http.sources['AMF']['override'] = override
                result = self.run_cli('--json')
                self.assertTrue(json.loads(result.stdout)['partial'])
                self.assertEqual(len(json.loads(result.stdout)['items']), 1)

    def test_missing_identity_can_be_followed_by_trace(self):
        self.http.sources['AMF']['items'][0]['supi'] = ''
        text = self.run_cli().stdout
        self.assertIn('TRACE AMF/' + BOOT + '/1', text)
        self.assertEqual(len(json.loads(self.run_cli('--supi', SUPI, '--json').stdout)['items']), 1)

    def test_no_history_is_explicit(self):
        self.http.sources['AMF']['items'] = []
        self.http.sources['SMF']['items'] = []
        self.assertIn('no retained events', self.run_cli().stdout)

    def test_bad_options(self):
        for options in [('--limit', '0'), ('--limit', '1001'), ('--limit', '-1'),
                        ('--since', '2026-02-30T00:00:00Z'), ('--since', 'bad'),
                        ('--since', '999999999999999999999d'), ('--interval', 'nan'),
                        ('--interval', 'inf'), ('--interval', '0'), ('--trace-id', '1'),
                        ('--supi', SUPI, '--trace-id', f'AMF/{BOOT}/1'), ('--with-location',)]:
            with self.subTest(options=options):
                self.run_cli(*options, expected=2)

    def watch(self, change):
        process = subprocess.Popen(self.argv('--supi', SUPI, '--detail', '--json', '--watch', '--interval', '0.1'),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 4
            while sum(nf == 'SMF' for nf, _ in self.http.requests) < 2:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            with self.http.lock:
                change()
            time.sleep(0.4)
        finally:
            process.terminate()
            stdout, stderr = process.communicate(timeout=4)
        return [json.loads(line) for line in stdout.splitlines()], stderr

    def test_watch_incremental_and_dedup(self):
        def change():
            self.http.sources['AMF']['items'].append(event('AMF', 4))
        documents, _ = self.watch(change)
        ids = [row['event_id'] for doc in documents for row in doc['items']]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertIn(f'AMF/{BOOT}/4', ids)
        self.assertTrue(any('after' in q for _, q in self.http.requests))

    def test_watch_backfills_pre_supi_steps(self):
        self.http.sources['AMF']['items'][0] = event('AMF', 1, False, supi='')
        self.http.sources['AMF']['items'] = self.http.sources['AMF']['items'][:1]
        def change():
            self.http.sources['AMF']['items'][0]['supi'] = SUPI
            bound = event('AMF', 3, False, protocol='IDENTITY')
            bound['name'] = 'IdentityResolved'
            self.http.sources['AMF']['items'].append(bound)
        documents, _ = self.watch(change)
        ids = [row['event_id'] for doc in documents for row in doc['items']]
        self.assertIn(f'AMF/{BOOT}/1', ids)
        self.assertEqual(ids.count(f'AMF/{BOOT}/1'), 1)

    def test_watch_restart_and_gap(self):
        def change():
            item = event('AMF', 1)
            item['event_id'] = f'AMF/{NEW_BOOT}/1'
            self.http.sources['AMF'] = dict(items=[item], boot=NEW_BOOT, gap=True)
        documents, stderr = self.watch(change)
        ids = [row['event_id'] for doc in documents for row in doc['items']]
        self.assertIn(f'AMF/{BOOT}/1', ids)
        self.assertIn(f'AMF/{NEW_BOOT}/1', ids)
        self.assertIn('restarted', stderr)
        self.assertIn('overwritten', stderr)


if __name__ == '__main__':
    unittest.main()
