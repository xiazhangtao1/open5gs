"""Bearer identity, aggregation, admission priority and bounded retention."""
import copy
import json
import sys
import unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cache import Cache, Curve
from memory import MemoryBudget, MemoryPressure, MIB


def session(seid='1', qfis=(1,), generation=1):
    rules = []
    for qfi in qfis:
        for direction, rate in [('UL', 100 * qfi), ('DL', 200 * qfi)]:
            rules.append(dict(qfi=qfi, pdr_id=len(rules) + 1, qer_id=qfi, generation=generation,
                              direction=direction, src_if=0 if direction == 'UL' else 1,
                              dst_if=1 if direction == 'UL' else 0,
                              bps=str(rate), pps='1', bytes='100', packets='1'))
    return dict(seid=str(seid), supi='imsi-' + str(seid), dnn='cmnet', ue_ip='10.45.0.1',
                uptime_seconds=100, rules=rules)


def make():
    return Cache(MemoryBudget(limit=512 * MIB, usage=lambda: 32 * MIB))


def sample(c, sessions, t=120, boot='a'):
    c.replace('upf_rates', copy.deepcopy(sessions), 2)
    c.record_rates(t, boot)


def trend(c, key='bearer:1:1', now=120, **args):
    with patch('cache.time.time', return_value=now):
        return c.query_trends(dict(key=key, seconds='1800', **args))


class BearerHistoryTests(unittest.TestCase):
    def test_multi_pdr_aggregation_and_session_isolation(self):
        c = make()
        s = session(qfis=(1, 7))
        extra = dict(s['rules'][0], pdr_id=99, bps='300')
        s['rules'].append(extra)
        sample(c, [s, session('2')])
        self.assertEqual(trend(c)['points'], [[120, 400, 200]])
        self.assertEqual(trend(c, 'bearer:1:7')['points'], [[120, 700, 1400]])
        self.assertEqual(trend(c, 'bearer:2:1')['points'], [[120, 100, 200]])
        row = c.query('rates', {'level': 'bearer'})['items'][0]
        self.assertEqual(row['trend_epoch'], trend(c)['epoch'])
        self.assertEqual(c.query('sessions', {})['items'][0]['bearers'][0]['trend_key'], 'bearer:1:1')

    def test_qos_edit_continuity_but_generation_reuse_resets(self):
        c, s = make(), session()
        sample(c, [s]); old = trend(c)
        s['rules'][0]['bps'] = '500'
        s['rules'].reverse()  # ordering and rates are not identities
        sample(c, [s], 122)
        self.assertEqual(trend(c, now=122)['epoch'], old['epoch'])
        s['rules'][0]['generation'] += 1
        sample(c, [s], 124)
        new = trend(c, now=124, cursor=old['cursor'])
        self.assertTrue(new['reset']); self.assertNotEqual(new['epoch'], old['epoch'])
        self.assertEqual(len(new['points']), 1)

    def test_release_recreate_reboot_uptime_and_missing_metadata(self):
        c, s = make(), session()
        sample(c, [s]); old = trend(c)
        sample(c, [], 122)
        self.assertEqual(trend(c, now=122)['history_state'], 'released')
        for t, boot, uptime in [(124, 'a', 100), (126, 'b', 100), (128, 'b', 0)]:
            s['uptime_seconds'] = uptime
            sample(c, [s], t, boot)
            new = trend(c, now=t)
            self.assertNotEqual(new['epoch'], old['epoch']); old = new
        del s['rules'][0]['generation']
        sample(c, [s], 130, 'b')
        self.assertEqual(trend(c, now=130)['history_reason'], 'identity')
        self.assertEqual(trend(c, now=130)['points'], [])
        self.assertEqual(c.sessions[0]['ul_bps'], '100')

    def test_caps_and_no_active_history_churn(self):
        c = make()
        sessions = [session(i) for i in range(1, 130)]
        sample(c, sessions)
        admitted = {k: v.epoch for k, v in c.curves.items() if k.startswith('bearer:')}
        self.assertEqual(len(admitted), 128)
        self.assertEqual(trend(c, 'bearer:129:1')['history_reason'], 'capacity')
        sample(c, list(reversed(sessions)), 122)
        self.assertEqual(admitted, {k: v.epoch for k, v in c.curves.items() if k.startswith('bearer:')})
        self.assertLessEqual(c.bearer_bytes(), 16 * MIB)
        self.assertLessEqual(c.trend_bytes(), 128 * MIB)
        sample(c, sessions[1:], 124)  # release oldest bearer, then admit 129
        self.assertIn('bearer:129:1', c.curves)
        self.assertNotIn('bearer:1:1', c.curves)

    def test_sub_budget_and_base_history_priority(self):
        c = make(); c.max_bearer_bytes = 240 * 1024
        sample(c, [session(qfis=(1, 2, 3))])
        self.assertEqual(sum(k.startswith('bearer:') for k in c.curves), 2)
        self.assertLessEqual(c.bearer_bytes(), c.max_bearer_bytes)
        c.max_curves = 5
        sample(c, [session(qfis=(1, 2, 3)), session('2')], 122)
        self.assertEqual(len(c.curves), 5)
        self.assertIn('session:2', c.curves); self.assertIn('user:imsi-2', c.curves)
        self.assertFalse(any(k.startswith('bearer:') for k in c.curves))
        c = make(); c.max_trend_bytes = 480 * 1024
        sample(c, [session()]); sample(c, [session(), session('2')], 122)
        self.assertLessEqual(c.trend_bytes(), c.max_trend_bytes)
        self.assertNotIn('bearer:1:1', c.curves)

    def test_tight_total_byte_limits_include_dictionary_growth(self):
        for limit in range(200000, 3000000, 117013):
            c = make(); c.max_trend_bytes = limit
            sample(c, [session(i) for i in range(1, 35)])
            self.assertLessEqual(c.trend_bytes(), limit)

    def test_zero_gap_expiry_and_49_hour_ring(self):
        c = make(); s = session()
        for r in s['rules']: r['bps'] = '0'
        sample(c, [s]); sample(c, [s], 122); sample(c, [s], 132)
        self.assertEqual(trend(c, now=132)['points'][0][1:], [0, 0])
        self.assertTrue(any(p[1] is None for p in trend(c, now=132)['points']))
        curve = c.curves['bearer:1:1']
        budget = c.bearer_bytes()
        for t in range(134, 49 * 3600 + 134, 2): curve.append(t, 100, 200)
        self.assertEqual(curve.count, 900); self.assertEqual(curve.minute_count, 1440)
        self.assertEqual(c.bearer_bytes(), budget)
        sample(c, [], curve.last + 86401)
        self.assertNotIn('bearer:1:1', c.curves)

    def test_memory_pressure_recovery_and_no_stale_minute_response(self):
        c = make(); sample(c, [session()])
        for t in range(122, 182, 2): sample(c, [session()], t)
        with patch('cache.time.time', return_value=190):
            a = c.response('trends', dict(key='bearer:1:1', seconds='86400'))
            sample(c, [], 184)
            b = c.response('trends', dict(key='bearer:1:1', seconds='86400'))
            self.assertNotEqual(a[1], b[1]); self.assertEqual(json.loads(b[0])['history_state'], 'released')
        c.memory.usage = lambda: 400 * MIB
        with self.assertRaises(MemoryPressure): sample(c, [session()], 186)
        self.assertFalse(any(k.startswith('bearer:') for k in c.curves))
        c.memory.usage = lambda: 32 * MIB
        sample(c, [session()], 188)
        self.assertEqual(trend(c, now=188)['history_state'], 'recording')

    def test_malformed_metadata_and_keys(self):
        for bad in (-1, 2**32, '1', None, True):
            c, s = make(), session(); s['rules'][0]['generation'] = bad
            sample(c, [s])
            self.assertEqual(trend(c)['history_reason'], 'identity')
        for key in ('bearer:1', 'bearer:1:0', 'bearer:1:64', 'bearer:abc:1', 'bearer:18446744073709551616:1'):
            with self.assertRaises(ValueError): trend(make(), key)

if __name__ == '__main__': unittest.main()
