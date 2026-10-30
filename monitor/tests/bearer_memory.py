"""Real cgroup admission/recovery regression; run only in an isolated 512 MiB container.

Uses actual allocated/touched memory. Never run in the production collector.
"""
import gc
import json
from pathlib import Path
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cache import Cache
from memory import MemoryPressure, MIB
from test_bearer_trends import session

c = Cache()
assert c.memory.current_file and c.memory.limit == 512 * MIB, 'requires isolated --memory=512m container'
rows = [session(i, tuple(range(1, 17)) if i <= 8 else ()) for i in range(1, 320)]
begin = time.monotonic()
c.replace('upf_rates', rows, 2); c.record_rates(time.time(), 'memory-test')
assert len(c.curves) == 1023 and sum(k.startswith('bearer:') for k in c.curves) == 128
initial = c.memory.status()
for n in range(100):
    c.replace('upf_rates', rows, 2); c.record_rates(time.time() + n * 2, 'memory-test')
    for key in list(c.curves)[-50:]:
        c.response('trends', dict(key=key, seconds='86400', cursor='old/' + str(n)))
    assert len(c.memo) <= 128 and c.memo_bytes <= 8 * MIB
    assert c.trend_bytes() <= 128 * MIB and c.bearer_bytes() <= 16 * MIB
before = c.memory.status()
# bytearray initializes and touches its pages; reserve stays well below OOM.
ballast = bytearray(max(0, c.memory.high - c.memory.sample(force=True) + 12 * MIB))
peak = c.memory.sample(force=True)
try:
    c.record_rates(time.time() + 202, 'memory-test')
    raise AssertionError('expected pressure admission rejection')
except MemoryPressure:
    pass
assert not any(k.startswith('bearer:') for k in c.curves)
assert peak >= c.memory.high and c.memory.reclaims > 0
assert c.memory.paused or c.memory.sample(force=True) <= c.memory.low
assert len(c.sessions) == 319  # latest state survives history eviction
pressure = c.memory.status()
del ballast
gc.collect(); c.memory.trim_allocator()
assert c.memory.sample(force=True) < c.memory.low
c.record_rates(time.time() + 204, 'memory-test')
assert sum(k.startswith('bearer:') for k in c.curves) == 128
assert sum(k.startswith('rule:') for k in c.curves) == 256
assert not c.memory.paused
print(json.dumps(dict(seconds=time.monotonic()-begin, initial=initial, before=before,
                     pressure=pressure, recovered=c.memory.status(), peak_bytes=peak,
                     curves=len(c.curves), trend_bytes=c.trend_bytes(), bearer_bytes=c.bearer_bytes())))
