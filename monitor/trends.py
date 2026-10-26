"""Bounded sampled-rate history. No NF I/O and no persistence.

Minute means describe the collected rate samples (time-weighted, previous-value
hold), not billing counters. Gaps and lifecycle boundaries are never filled.
"""
from array import array
import math
import sys
import uuid


class Curve:
    capacity = 900
    minute_capacity = 1440
    # Arrays plus a conservative allowance for the object, keys, OrderedDict
    # entry, scalar state and current bucket. Checked again by budget_bytes().
    allocation_bytes = 112 * 1024

    def __init__(self):
        self.data = array('d', [0]) * (self.capacity * 3)
        self.breaks = bytearray(self.capacity)
        self.minutes = array('d', [0]) * (self.minute_capacity * 7)
        self.count = self.next = self.minute_count = self.minute_next = 0
        self.minute_version = 0
        self.last = 0
        self.previous = (0, 0)
        self.pending = None
        self.epoch = uuid.uuid4().hex
        self.active = False
        self.identity = None
        self.uptime = None
        self.max_gap = 6

    def budget_bytes(self, key=''):
        arrays = sys.getsizeof(self.data) + sys.getsizeof(self.breaks) + sys.getsizeof(self.minutes)
        # Includes the bounded seven-value pending bucket and scalar metadata.
        return arrays + 8192 + sys.getsizeof(key)

    def _bucket(self, start):
        if self.pending is None or self.pending[0] != start:
            if self.pending is not None and self.pending[5] > 0:
                offset = self.minute_next * 7
                self.minutes[offset:offset + 7] = array('d', self.pending)
                self.minute_next = (self.minute_next + 1) % self.minute_capacity
                self.minute_count = min(self.minute_capacity, self.minute_count + 1)
                self.minute_version += 1
            # start, UL integral, DL integral, UL peak, DL peak, seconds, samples
            self.pending = [start, 0., 0., 0., 0., 0., 0.]
        return self.pending

    def append(self, stamp, ul, dl, discontinuity=False, interval=2):
        if not all(math.isfinite(v) for v in (stamp, ul, dl)) or stamp <= 0 or min(ul, dl) < 0:
            raise ValueError('invalid trend sample')
        if self.last and stamp <= self.last:
            return False
        self.max_gap = max(6, interval * 3)
        broken = discontinuity or not self.last or stamp - self.last > self.max_gap
        if not broken:
            at = self.last
            while at < stamp:
                start = math.floor(at / 60) * 60
                end = min(stamp, start + 60)
                bucket = self._bucket(start)
                seconds = end - at
                bucket[1] += self.previous[0] * seconds
                bucket[2] += self.previous[1] * seconds
                bucket[3] = max(bucket[3], self.previous[0])
                bucket[4] = max(bucket[4], self.previous[1])
                bucket[5] += seconds
                at = end
        bucket = self._bucket(math.floor(stamp / 60) * 60)
        bucket[3], bucket[4] = max(bucket[3], ul), max(bucket[4], dl)
        bucket[6] += 1
        self.data[self.next * 3:self.next * 3 + 3] = array('d', [stamp, ul, dl])
        self.breaks[self.next] = broken
        self.next = (self.next + 1) % self.capacity
        self.count = min(self.count + 1, self.capacity)
        self.last, self.previous = stamp, (ul, dl)
        return True

    def points(self, since):
        out = []
        for i in range(self.count):
            slot = (self.next - self.count + i) % self.capacity
            t, ul, dl = self.data[slot * 3:slot * 3 + 3]
            if t >= since:
                if out and (self.breaks[slot] or t - out[-1][0] > self.max_gap):
                    out.append([out[-1][0] + .001, None, None])
                out.append([t, ul, dl])
        return out

    def minute_points(self, start, end):
        points, buckets = [], []
        previous = None
        def add(row):
            nonlocal previous
            t, ul, dl, peak_ul, peak_dl, covered, samples = row
            if not start <= t < end or covered <= 0:
                return
            partial = covered < 60 - 1e-6
            if previous and (t - previous[0] > 60 or previous[1] or partial):
                points.append([previous[0] + .001, None, None])
            points.append([t, ul / covered, dl / covered])
            buckets.append([t, peak_ul, peak_dl, min(60, covered), int(samples)])
            previous = (t, partial)
        for i in range(self.minute_count):
            offset = (self.minute_next - self.minute_count + i) % self.minute_capacity * 7
            add(self.minutes[offset:offset + 7])
        if self.pending:
            add(self.pending)
        return points, buckets
