"""Single UPF socket reader; no per-browser work and no queue of pending snapshots."""
import asyncio
import json
import logging
import os

import aiohttp
from common import config, pin_cpu, read_limited
from memory import UPF_BODY_LIMIT


async def read_snapshot(path):
    reader, writer = await asyncio.open_unix_connection(path, limit=65536)
    try:
        writer.write(b"level=snapshot json=1\n")
        await writer.drain()
        chunks, length = [], 0
        while True:
            data = await reader.read(65536)
            if not data:
                break
            length += len(data)
            if length > UPF_BODY_LIMIT:
                raise ValueError("UPF snapshot exceeds byte budget")
            chunks.append(data)
        return b"".join(chunks)
    finally:
        writer.close()
        await writer.wait_closed()


async def main():
    pin_cpu()
    url = os.environ["MONITOR_URL"].rstrip("/")
    headers = {"Authorization": "Bearer " + os.environ["MONITOR_INTERNAL_TOKEN"]}
    path = os.getenv("UPF_STATS_SOCKET", "/run/open5gs/upf-stats.sock")
    timeout = aiohttp.ClientTimeout(total=2, connect=0.3)
    errors = 0
    async with aiohttp.ClientSession(timeout=timeout, headers=headers, trust_env=False) as http:
        while True:
            interval = config()["rates_interval"]
            try:
                async with http.get(url + "/internal/control") as response:
                    control = json.loads(await read_limited(response, 16384))
                interval = max(interval, control["interval"])
                if control["enabled"] and config()["enabled"]:
                    if control.get("affinity", {}).get("reserved"):
                        pin_cpu(control["affinity"]["reserved"])
                    data = await asyncio.wait_for(read_snapshot(path), 2.5)
                    # Inspect only tiny warm-up/error messages. Full decoding belongs
                    # to the independent collector, not this 128 MiB sidecar.
                    doc = json.loads(data) if len(data) <= 1024 else {"ready": True}
                    if not isinstance(doc, dict):
                        raise ValueError("invalid UPF snapshot object")
                    if doc.get("ready") is False:
                        # Expired subscription: allow one sampler tick, one retry.
                        await asyncio.sleep(min(2.1, max(.1, float(doc.get("retry_ms", 1100)) / 1000)))
                        data = await asyncio.wait_for(read_snapshot(path), 2.5)
                    async with http.post(url + "/internal/upf", data=data,
                                         headers={"Content-Type": "application/json"}) as response:
                        await read_limited(response, 16384)
                errors = 0
            except (aiohttp.ClientError, OSError, ValueError, KeyError, TimeoutError) as exc:
                errors += 1
                logging.warning("UPF collection failed: %s", str(exc)[:200])
                try:
                    async with http.post(url + "/internal/upf-error", json={"error": str(exc)[:200]}) as response:
                        await read_limited(response, 16384)
                except (aiohttp.ClientError, OSError, ValueError, TimeoutError):
                    pass
            await asyncio.sleep(min(max(30, interval), interval * 2 ** min(errors, 3)))


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    asyncio.run(main())
