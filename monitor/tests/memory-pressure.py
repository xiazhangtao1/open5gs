"""Run in a small, isolated --memory container; never against a live NF."""
import asyncio
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aiohttp import ClientSession, web
from cache import Curve
from memory import MIB
from server import application


async def main():
    app = application("m" * 64, collecting=False)
    cache = app["cache"]
    app["collector"].collection_ready = True
    assert cache.memory.current_file, "this test requires an enforced container memory limit"
    cache.add_events("amf", dict(boot_id="memory-test", items=[
        dict(seq=str(i), timestamp_us=str(int(time.time() * 1e6)), detail="x" * 512)
        for i in range(2000)]))
    for i in range(32):
        cache.curves[str(i)] = Curve()
    cache.curves["global"] = Curve()
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    url = "http://127.0.0.1:" + str(site._server.sockets[0].getsockname()[1])
    held = []
    try:
        async with ClientSession(headers={"Authorization": "Bearer " + "m" * 64}) as http:
            # Touch pages until just above 70%; retain ample headroom to the hard limit.
            while cache.memory.sample(force=True) <= cache.memory.high + MIB:
                held.append(bytearray(b"x" * MIB))
            async with http.post(url + "/internal/upf", json={"schema": 1}) as response:
                assert response.status == 503, response.status
            async with http.get(url + "/api/health") as response:
                pressure = (await response.json())["memory"]
                assert response.status == 200 and pressure["paused"]
            assert len(cache.events) == 1000 and list(cache.curves) == ["global"]
            held.clear()
            cache.memory.trim_allocator()
            cache.memory.sample(force=True)
            async with http.get(url + "/api/ues") as response:
                assert response.status == 200, response.status
                await response.read()
            await asyncio.sleep(2.1)  # Public health responses share a 2-second age bucket.
            async with http.get(url + "/api/health") as response:
                recovered = (await response.json())["memory"]
                assert not recovered["paused"]
            events_file = Path("/sys/fs/cgroup/memory.events")
            events = events_file.read_text() if events_file.exists() else Path(
                "/sys/fs/cgroup/memory/memory.oom_control").read_text()
            if events_file.exists():
                assert dict(line.split() for line in events.splitlines())["oom_kill"] == "0"
            else:
                assert dict(line.split() for line in events.splitlines())["oom_kill"] == "0"
                assert Path("/sys/fs/cgroup/memory/memory.failcnt").read_text().strip() == "0"
            print(json.dumps(dict(pressure=pressure, recovered=recovered, cgroup_events=events), ensure_ascii=False))
    finally:
        await runner.cleanup()


asyncio.run(main())
