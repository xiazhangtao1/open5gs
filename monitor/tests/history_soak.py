"""Isolated 512-series/50-reader real-time soak; never contacts a real NF.

Run in a --memory=512m, --network=none container. Historical seed is explicitly
synthetic; elapsed_seconds measures the separate real wall-clock soak.
"""
import argparse
from array import array
import asyncio
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aiohttp import ClientSession, ClientTimeout, web
from server import application
from trends import Curve


async def main(seconds):
    app = application('h' * 64, collecting=False)
    app['collector'].collection_ready = True
    cache = app['cache']
    assert cache.memory.current_file, 'requires an enforced container memory limit'
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, '127.0.0.1', 0)
    await site.start()
    base = 'http://127.0.0.1:' + str(site._server.sockets[0].getsockname()[1])
    rows = [dict(seid=str(i + 1), supi='imsi-00101' + str(i).zfill(10), dnn='cmnet',
                 ue_ip='10.45.' + str(i // 254) + '.' + str(i % 254 + 1), uptime_seconds=100,
                 rules=[dict(pdr_id=j + 1, qer_id=1, generation=1, qfi=1, direction=d,
                             bps=str((i + 1) * 1000), pps='10', bytes='100000', packets='100')
                        for j, d in enumerate(('UL', 'DL'))]) for i in range(256)]
    stats = dict(reads=0, uploads=0, errors=[], max_used_bytes=0, max_body_bytes=0)
    latency = array('d')
    async with ClientSession(headers={'Authorization': 'Bearer ' + 'h' * 64},
                             timeout=ClientTimeout(total=10)) as http:
        async def upload():
            now = time.time()
            doc = dict(schema=1, ready=True, truncated=False, boot_id='history-soak',
                       version=str(time.monotonic_ns()), timestamp_us=str(int(now * 1e6)),
                       monotonic_us=str(int(time.monotonic() * 1e6)), sample_us=1000000, sessions=rows)
            async with http.post(base + '/internal/upf', json=doc) as response:
                if response.status != 200:
                    raise RuntimeError('upload: ' + str(response.status) + ' ' + await response.text())
                await response.read()
                stats['uploads'] += 1
        await upload()
        prototype = Curve()
        end = int(time.time() // 60) * 60
        for stamp in range(end - 48 * 3600, end + 1, 2):
            prototype.append(stamp, 100000, 200000)
        for curve in cache.curves.values():
            curve.data = array('d', prototype.data)
            curve.breaks = bytearray(prototype.breaks)
            curve.minutes = array('d', prototype.minutes)
            for field in ('count', 'next', 'minute_count', 'minute_next', 'minute_version', 'last', 'previous'):
                setattr(curve, field, getattr(prototype, field))
            curve.pending = list(prototype.pending)
        del prototype
        assert len(cache.curves) == 512 and cache.trend_bytes() <= cache.max_trend_bytes
        keys = list(cache.curves)
        begin = time.monotonic()
        finish = begin + seconds

        async def producer():
            while time.monotonic() < finish:
                await upload()
                await asyncio.sleep(2)

        async def reader(i):
            window = (1800, 43200, 86400)[i % 3]
            params = dict(key=keys[i * 7 % len(keys)], seconds=str(window))
            while time.monotonic() < finish:
                started = time.monotonic()
                async with http.get(base + '/api/trends', params=params) as response:
                    body = await response.read()
                    if response.status != 200:
                        raise RuntimeError('reader: ' + str(response.status) + ' ' + body.decode()[:100])
                    data = json.loads(body)
                    assert len(data['points']) <= 3000 and len(data['buckets']) <= 1440
                    assert len(body) <= 1024 * 1024
                    params['cursor'] = data['cursor']
                    stats['max_body_bytes'] = max(stats['max_body_bytes'], len(body))
                    stats['reads'] += 1
                latency.append((time.monotonic() - started) * 1000)
                await asyncio.sleep(max(.01, 2 - (time.monotonic() - started)))

        async def observe():
            while time.monotonic() < finish:
                used = cache.memory.sample(force=True)
                stats['max_used_bytes'] = max(stats['max_used_bytes'], used)
                assert cache.trend_bytes() <= cache.max_trend_bytes
                assert len(cache.curves) <= 512 and cache.memo_bytes <= 8 * 1024 * 1024
                print(json.dumps(dict(elapsed_seconds=round(time.monotonic() - begin, 1),
                                      trend_bytes=cache.trend_bytes(), used_bytes=used,
                                      reads=stats['reads'], uploads=stats['uploads'])), flush=True)
                await asyncio.sleep(min(60, max(.01, finish - time.monotonic())))
        try:
            await asyncio.gather(producer(), observe(), *(reader(i) for i in range(50)))
            values = sorted(latency)
            result = dict(stats, elapsed_seconds=round(time.monotonic() - begin, 2),
                          synthetic_history_hours=48, curves=len(cache.curves), trend_bytes=cache.trend_bytes(),
                          p95_ms=values[int(len(values) * .95)], p99_ms=values[int(len(values) * .99)],
                          final_memory=cache.memory.status())
            path = Path('/sys/fs/cgroup/memory.events')
            if path.exists():
                events = dict(line.split() for line in path.read_text().splitlines())
                assert events['oom_kill'] == '0'
                result['memory_events'] = events
            else:
                events = dict(line.split() for line in Path('/sys/fs/cgroup/memory/memory.oom_control').read_text().splitlines())
                assert events['oom_kill'] == '0'
                assert Path('/sys/fs/cgroup/memory/memory.failcnt').read_text().strip() == '0'
                result['memory_events'] = events
            assert not cache.memory.paused
            print(json.dumps(result), flush=True)
        finally:
            await runner.cleanup()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--seconds', type=int, default=7200)
    args = parser.parse_args()
    asyncio.run(main(args.seconds))
