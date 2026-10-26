"""History correctness, lifecycle, incremental queries and allocation bounds."""
import math
import random
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cache import Cache, Curve
from memory import MemoryBudget, MIB


def cache():
    return Cache(memory=MemoryBudget(limit=512 * MIB, usage=lambda: 32 * MIB))


def session(seid='1', supi='imsi-1', rate=100, uptime=100):
    return dict(seid=seid, supi=supi, dnn='cmnet', ue_ip='10.45.0.1',
                ul_bps=str(rate), dl_bps=str(rate * 2), uptime_seconds=uptime)


def query(c, now, seconds=86400, **extra):
    with patch('cache.time.time', return_value=now):
        return c.query('trends', dict(seconds=str(seconds), **extra))


class HistoryTests(unittest.TestCase):
    def test_against_independent_per_second_reference(self):
        rng = random.Random(20261026)
        curve, seconds, peaks = Curve(), {}, {}
        stamp, previous = 180000, None
        for i in range(600):
            stamp += rng.choice((1, 2, 3, 6, 8, 60))
            rate = rng.randrange(0, 10000)
            broken = i % 43 == 0
            if previous and not broken and stamp - previous[0] <= 6:
                for second in range(previous[0], stamp):
                    seconds[second] = previous[1]
                    minute = second // 60 * 60
                    peaks[minute] = max(peaks.get(minute, 0), previous[1])
            minute = stamp // 60 * 60
            peaks[minute] = max(peaks.get(minute, 0), rate)
            curve.append(stamp, rate, rate * 3, discontinuity=broken)
            previous = stamp, rate
        end = stamp // 60 * 60
        points, buckets = curve.minute_points(180000, end)
        expected = {}
        for second, rate in seconds.items():
            if second < end:
                expected.setdefault(second // 60 * 60, []).append(rate)
        values = {p[0]: p[1:] for p in points if p[1] is not None}
        self.assertEqual(set(values), set(expected))
        for bucket in buckets:
            rates = expected[bucket[0]]
            self.assertAlmostEqual(values[bucket[0]][0], sum(rates) / len(rates))
            self.assertAlmostEqual(values[bucket[0]][1], 3 * sum(rates) / len(rates))
            self.assertEqual(bucket[1], peaks[bucket[0]])
            self.assertEqual(bucket[3], len(rates))

    def test_weighted_mean_peak_and_gaps(self):
        c = Curve()
        for t, rate in [(120, 10), (122, 30), (126, 90), (128, 0), (180, 5)]:
            c.append(t, rate, rate * 2)
        points, buckets = c.minute_points(120, 240)
        self.assertEqual(points, [[120, 40, 80]])
        self.assertEqual(buckets, [[120, 90, 180, 8, 4]])
        self.assertTrue(any(p[1] is None for p in c.points(120)))
        self.assertFalse(c.append(180, 100, 200))
        self.assertFalse(c.append(170, 100, 200))

    def test_cross_minute_and_partial_coverage(self):
        c = Curve()
        for t, rate in [(178, 10), (182, 30), (186, 50)]:
            c.append(t, rate, rate)
        points, buckets = c.minute_points(120, 240)
        self.assertEqual(points[0], [120, 10, 10])
        self.assertIsNone(points[1][1])
        self.assertAlmostEqual(points[2][1], 140 / 6)
        self.assertEqual([b[3] for b in buckets], [2, 6])
        self.assertEqual([b[1] for b in buckets], [10, 50])

    def test_zero_and_lifecycle_break(self):
        c = Curve()
        c.append(120, 0, 0)
        c.append(122, 0, 0)
        c.append(124, 900, 900, discontinuity=True)
        c.append(126, 900, 900)
        points, buckets = c.minute_points(120, 180)
        self.assertEqual(points, [[120, 450, 450]])
        self.assertEqual(buckets[0][3], 4)
        self.assertIsNone(c.points(120)[2][1])

    def test_48_hours_ring_wrap_and_queries(self):
        c = cache()
        curve = Curve()
        start = 180000
        for t in range(start, start + 48 * 3600 + 1, 2):
            curve.append(t, 100, 200)
        c.curves['global'] = curve
        end = start + 48 * 3600
        c.trend_stamp = end
        raw = query(c, end, 1800)
        day = query(c, end)
        half = query(c, end, 43200)
        self.assertEqual(len(raw['points']), 900)
        self.assertEqual(len(day['points']), 1440)
        self.assertEqual(len(half['points']), 720)
        self.assertTrue(all(p[1:] == [100, 200] for p in day['points']))
        self.assertTrue(all(b[3] == 60 for b in day['buckets']))
        self.assertEqual(day['available_from'], end - 86400)
        self.assertEqual(day['available_to'], end)
        self.assertLessEqual(len(query(c, end + 100000)['points']), 0)

    def test_incremental_overlap_and_epoch_reset(self):
        c = cache()
        for t in range(120, 180, 2):
            c.record_rates(t)
        old = query(c, 180)
        # Complete the previous minute only after it was already queried.
        for t in range(180, 242, 2):
            c.record_rates(t)
        delta = query(c, 242, cursor=old['cursor'])
        full = query(c, 242)
        self.assertFalse(delta['reset'])
        for field in ('points', 'buckets'):
            merged = [p for p in old[field] if full['window_start'] <= p[0] < delta['replace_from']] + delta[field]
            self.assertEqual(merged, full[field])
        c.curves.clear()
        c.record_rates(244)
        self.assertTrue(query(c, 244, cursor=delta['cursor'])['reset'])
        self.assertTrue(query(c, 244, cursor=c.curves['global'].epoch + '/999999')['reset'])
        for bad in ('abc', 'x/NaN', 'x/Infinity'):
            with self.assertRaises(ValueError):
                query(c, 244, cursor=bad)

    def test_capacity_and_no_active_churn(self):
        c = cache()
        c.sessions = [session(str(i), 'imsi-' + str(i)) for i in range(300)]
        c.record_rates(120)
        self.assertEqual(len(c.curves), 512)
        self.assertLess(c.trend_bytes(), c.max_trend_bytes)
        epochs = {k: v.epoch for k, v in c.curves.items()}
        c.record_rates(122)
        self.assertEqual(epochs, {k: v.epoch for k, v in c.curves.items()})
        self.assertGreater(c.curve_rejections, 0)
        c.sessions = [session('new', 'imsi-new')]
        c.record_rates(124)
        self.assertIn('user:imsi-new', c.curves)
        self.assertEqual(c.curves['global'].epoch, epochs['global'])
        self.assertGreater(c.curve_evictions, 0)

    def test_byte_budget_before_allocation(self):
        c = cache()
        c.max_trend_bytes = 240 * 1024
        c.sessions = [session()]
        c.record_rates(120)
        self.assertLessEqual(c.trend_bytes(), c.max_trend_bytes)
        self.assertEqual(len(c.curves), 2)
        self.assertGreater(c.curve_rejections, 0)

    def test_reused_session_and_user_resume(self):
        c = cache()
        c.sessions = [session()]
        c.record_rates(120, boot_id='a')
        old = c.curves['session:1'].epoch
        user = c.curves['user:imsi-1'].epoch
        c.sessions = []
        c.record_rates(122, boot_id='a')
        c.sessions = [session(uptime=0)]
        c.record_rates(124, boot_id='a')
        self.assertNotEqual(old, c.curves['session:1'].epoch)
        self.assertEqual(user, c.curves['user:imsi-1'].epoch)
        self.assertIsNone(c.curves['user:imsi-1'].points(120)[1][1])
        previous = c.curves['session:1'].epoch
        c.record_rates(126, boot_id='b')
        self.assertNotEqual(previous, c.curves['session:1'].epoch)
        c.record_rates(100, boot_id='b')
        self.assertEqual(c.curves['global'].count, 1)

    def test_retention_and_stable_minute_etag(self):
        c = cache()
        c.sessions = [session()]
        for t in range(120, 182, 2):
            c.record_rates(t)
        with patch('cache.time.time', return_value=190):
            body, tag = c.response('trends', {'seconds': '86400'})
            c.changed()  # unrelated signal update
            self.assertEqual(c.response('trends', {'seconds': '86400'}), (body, tag))
            c.record_rates(184)
            self.assertEqual(c.response('trends', {'seconds': '86400'}), (body, tag))
        c.sessions = []
        c.record_rates(184 + 86401)
        self.assertEqual(list(c.curves), ['global'])

    def test_invalid_numbers(self):
        for v in (math.nan, math.inf, -1):
            with self.assertRaises(ValueError):
                Curve().append(120, v, 0)


if __name__ == '__main__':
    unittest.main()
