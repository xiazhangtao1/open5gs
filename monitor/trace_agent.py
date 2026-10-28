"""The only native ring reader. No ASN.1 decoding and no interaction with UPF."""
import asyncio
import ctypes
import logging
import os
import time

import aiohttp
from common import config, pin_cpu, read_limited, wait_for_cpu


class Reader:
    def __init__(self):
        self.lib = ctypes.CDLL(os.getenv("TRACE_READER_LIB", "/opt/xcn-monitor/libxcn-trace.so"))
        specs = {
            "open": ([ctypes.c_char_p], ctypes.c_void_p),
            "close": ([ctypes.c_void_p], None),
            "current": ([ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int),
            "lease": ([ctypes.c_void_p, ctypes.c_int], None),
            "read": ([ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_int),
            "stats": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64)], None),
        }
        for name, (args, result) in specs.items():
            fn = getattr(self.lib, "xcn_trace_reader_" + name)
            fn.argtypes, fn.restype = args, result
            setattr(self, name, fn)
        self.handles = {}
        self.buffer = ctypes.create_string_buffer(32768 + 68)

    def refresh(self, enabled):
        for nf in ("AMF", "SMF"):
            path = (os.getenv("XCN_TRACE_DIR", "/run/xcn-trace") + "/" + nf + ".ring").encode()
            handle = self.handles.get(nf)
            if handle and not self.current(handle, path):
                self.close(handle); handle = None
            if not handle:
                handle = self.open(path)
            self.handles[nf] = handle
            if handle:
                self.lease(handle, enabled)

    def batch(self):
        result = bytearray()
        # Round robin avoids starving SMF during an AMF burst.
        for _ in range(128):
            any_read = False
            for nf, handle in self.handles.items():
                if not handle:
                    continue
                if len(result) + len(self.buffer) > 1024 * 1024:
                    return bytes(result)
                size = self.read(handle, self.buffer, len(self.buffer))
                if size < 0:
                    self.lease(handle, False)
                    raise ValueError("invalid trace ring")
                if size:
                    result.extend(self.buffer.raw[:size]); any_read = True
            if not any_read:
                break
        return bytes(result)

    def counters(self):
        result = {}
        for nf, handle in self.handles.items():
            values = (ctypes.c_uint64 * 5)()
            self.stats(handle, values)
            result[nf] = dict(zip(("captured", "full", "oversize", "limited", "wrong_thread"), values))
        return result

    def shutdown(self):
        for handle in self.handles.values():
            if handle:
                self.close(handle)


async def main():
    pin_cpu()
    reader = Reader()
    url = os.environ["MONITOR_URL"].rstrip("/")
    headers = {"Authorization": "Bearer " + os.environ["MONITOR_INTERNAL_TOKEN"]}
    enabled, due, lost = False, 0, 0
    async with aiohttp.ClientSession(headers=headers, trust_env=False,
            timeout=aiohttp.ClientTimeout(total=1, connect=.2)) as http:
        try:
            while True:
                try:
                    if time.monotonic() >= due:
                        async with http.get(url + "/internal/trace-control") as response:
                            import json
                            control = json.loads(await read_limited(response, 16384))
                        reserved = control.get("affinity", {}).get("reserved", [])
                        enabled = bool(control.get("enabled") and config().get("trace_enabled") and reserved)
                        if enabled:
                            pin_cpu(reserved)
                        reader.refresh(enabled)
                        async with http.post(url + "/internal/trace-status",
                                json=dict(rings=reader.counters(), upload_dropped=lost)) as response:
                            await read_limited(response, 16384)
                        due = time.monotonic() + 2
                    if enabled:
                        data = reader.batch()
                        if data:
                            try:
                                async with http.post(url + "/internal/signals", data=data,
                                        headers={"Content-Type": "application/octet-stream"}) as response:
                                    await read_limited(response, 16384)
                            except Exception:
                                lost += 1
                                raise
                except (aiohttp.ClientError, ValueError, OSError, TimeoutError):
                    enabled = False
                    reader.refresh(False)
                    due = time.monotonic() + 2
                    logging.warning("Trace delivery paused; payloads are never logged")
                await asyncio.sleep(.05 if enabled else .5)
        finally:
            reader.shutdown()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    wait_for_cpu()
    asyncio.run(main())
