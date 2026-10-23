"""Bounded-cache soak; run in a disposable 512 MiB container, never against an NF."""
import argparse
import gc
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cache import Cache
from memory import MIB, MemoryPressure
from signals import Signals, HEADER


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=3600)
    args = parser.parse_args()
    cache = Cache()
    signals = cache.signals = Signals(cache.memory)
    start, seq, peak, reads, samples = time.monotonic(), 0, 0, 0, []
    boot = b'00000000-0000-0000-0000-000000000001'
    while time.monotonic() - start < args.seconds:
        batch = bytearray()
        for _ in range(100):
            seq += 1
            pdu = b'\x7e\x00\x43' + seq.to_bytes(8, 'big') + b'x' * 2037
            batch += HEADER.pack(b'XTR1', 2, 1, 0, seq, int(time.time() * 1e6), len(pdu), boot, b'AMF\0') + pdu
        try:
            signals.admit(bytes(batch))
            ident = f'AMF/{boot.decode()}/{seq}'
            signals.remember(ident, dict(state='ready', tree=[dict(name='soak', value='v' * (256 * 1024))]))
            signals.query(ident, 'decoded'); reads += 1
        except MemoryPressure:
            cache.relieve_memory()
        used = cache.memory.sample(force=True)
        peak = max(peak, used)
        assert len(signals.raw) <= 20000 and signals.raw_bytes <= 64 * MIB
        assert len(signals.decoded) <= 256 and signals.decoded_bytes <= 32 * MIB
        if not samples or time.monotonic() - start - samples[-1]['elapsed'] >= 60:
            row = dict(elapsed=round(time.monotonic() - start, 2), used_bytes=used, **signals.status())
            samples.append(row)
            print(json.dumps(row), flush=True)
        time.sleep(.2)
    assert signals.evicted > 0 and signals.decoded_evicted > 0
    # Exercise actual cgroup/RSS watermarks, then verify successful recovery.
    pressure = []
    while cache.memory.sample(force=True) < cache.memory.high:
        pressure.append(bytearray(4 * MIB))
    cache.last_reclaim = 0
    cache.relieve_memory()
    assert not signals.raw and not signals.decoded
    pressure.clear(); gc.collect(); cache.memory.trim_allocator()
    cache.memory.sample(force=True)
    signals.admit(HEADER.pack(b'XTR1', 2, 1, 0, seq + 1, int(time.time() * 1e6), 3, boot, b'AMF\0') + b'\x7e\x00\x43')
    print(json.dumps(dict(passed=True, duration=round(time.monotonic() - start, 2),
                         generated=seq, cached_reads=reads, peak_bytes=peak,
                         recovery_used_bytes=cache.memory.used,
                         pressure_reclaims=cache.memory.reclaims, samples=samples)), flush=True)


if __name__ == '__main__':
    main()
