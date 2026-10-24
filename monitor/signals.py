"""Bounded raw PDUs and lazy, coalesced decoding. Never reads an NF on a UI request."""
import asyncio
import json
import os
import re
import struct
from collections import OrderedDict

import aiohttp
from common import config, trace_redact_keys
from memory import MIB, MemoryPressure

HEADER = struct.Struct("!4sBBHQQI36s4s")
ID = re.compile(r"(?:AMF|SMF)/[0-9a-fA-F-]{36}/[1-9][0-9]{0,19}\Z")
KINDS = {1: "NGAP", 2: "NAS-5GMM", 3: "NAS-5GSM"}
MAX_BATCH = MIB


def unpack_batch(data):
    rows, offset = [], 0
    while offset < len(data):
        if len(data) - offset < HEADER.size or len(rows) >= 256:
            raise ValueError("invalid trace batch")
        magic, kind, security, reserved, seq, stamp, size, boot, nf = HEADER.unpack_from(data, offset)
        offset += HEADER.size
        nf, boot = nf.rstrip(b"\0").decode("ascii"), boot.decode("ascii")
        ident = f"{nf}/{boot}/{seq}"
        if (magic != b"XTR1" or kind not in KINDS or reserved or not ID.fullmatch(ident)
                or security & ~63 or security & 32 or not 0 < size <= 32768
                or offset + size > len(data) or not 0 < stamp < 1 << 53
                or (nf == "SMF" and kind != 3)):
            raise ValueError("invalid trace record")
        rows.append((ident, kind, security, stamp, data[offset:offset + size]))
        offset += size
    return rows


class Signals:
    raw_limit = 64 * MIB
    raw_count = 20000
    decoded_limit = 32 * MIB
    decoded_count = 256

    def __init__(self, memory):
        self.memory = memory
        self.raw = OrderedDict()
        self.decoded = OrderedDict()
        self.raw_bytes = self.decoded_bytes = 0
        self.evicted = self.decoded_evicted = self.failures = self.requests = 0
        self.decode_errors = 0
        self.queue = asyncio.Queue(maxsize=64)
        self.pending = set()
        self.seen = OrderedDict()
        self.agent = {}
        self.decoder_version = None
        self.decoder_ready = False
        self.redact_keys = trace_redact_keys()

    def admit(self, data):
        self.memory.check(16 * MIB + len(data) * 4, "信令详情采集")
        rows = unpack_batch(data)
        for ident, kind, security, stamp, pdu in rows:
            if ident in self.raw:
                old = self.raw[ident]
                if old[0:3] != (kind, security, stamp) or old[3] != pdu:
                    raise ValueError("trace event identity collision")
                continue
            self.raw[ident] = (kind, security, stamp, pdu)
            self.raw_bytes += len(pdu)
            self.seen[ident] = True
            while len(self.seen) > self.raw_count:
                self.seen.popitem(last=False)
        self.trim()

    def trim(self, pressure=False):
        while self.decoded and (pressure or len(self.decoded) > self.decoded_count or self.decoded_bytes > self.decoded_limit):
            _, body = self.decoded.popitem(last=False)
            self.decoded_bytes -= len(body)
            self.decoded_evicted += 1
        target = 0 if pressure else self.raw_limit
        count = 0 if pressure else self.raw_count
        while self.raw and (self.raw_bytes > target or len(self.raw) > count):
            self.raw_bytes -= len(self.raw.popitem(last=False)[1][3])
            self.evicted += 1

    def status(self):
        return dict(raw_records=len(self.raw), raw_bytes=self.raw_bytes,
                    decoded_records=len(self.decoded), decoded_bytes=self.decoded_bytes,
                    evicted=self.evicted, decoded_evicted=self.decoded_evicted,
                    pending=len(self.pending), failures=self.failures, decode_errors=self.decode_errors,
                    decoder_version=self.decoder_version, decoder_ready=self.decoder_ready, redact_keys=self.redact_keys,
                    decode_requests=self.requests, enabled=bool(config()["enabled"] and config().get("trace_enabled")), agent=self.agent)

    def remember(self, ident, doc):
        if doc.get("redact_keys", True) is not self.redact_keys:
            raise ValueError("decoder disclosure policy does not match collector")
        body = json.dumps(doc, ensure_ascii=False, separators=(",", ":")).encode()
        if len(body) > MIB:
            body = b'{"state":"error","reason":"output_limit"}'
        if ident in self.decoded:
            self.decoded_bytes -= len(self.decoded.pop(ident))
        self.decoded[ident] = body
        self.decoded_bytes += len(body)
        self.trim()

    def query(self, ident, view):
        if not ID.fullmatch(ident) or view not in ("decoded", "hex"):
            raise ValueError("invalid signal query")
        self.memory.check(16 * MIB, "信令详情查询")
        if ident in self.decoded:
            self.decoded.move_to_end(ident)
            doc = json.loads(self.decoded[ident])
            if view == "decoded":
                doc.pop("hex", None)
            else:
                doc.pop("tree", None)
            return dict(doc, event_id=ident)
        if ident not in self.raw:
            return dict(state="unavailable", reason="evicted" if ident in self.seen else "not_collected", event_id=ident)
        cfg = config()
        if not cfg["enabled"] or not cfg.get("trace_enabled"):
            return dict(state="unavailable", reason="disabled", event_id=ident)
        if ident not in self.pending:
            if self.queue.full():
                return dict(state="busy", reason="queue_full", retry_ms=2000, event_id=ident)
            self.pending.add(ident)
            self.queue.put_nowait(ident)
        return dict(state="queued", retry_ms=1000, event_id=ident)

    async def worker(self):
        url = os.getenv("TRACE_DECODER_URL", "").rstrip("/")
        token = os.getenv("MONITOR_INTERNAL_TOKEN", "")
        timeout = aiohttp.ClientTimeout(total=4, connect=.3)
        async with aiohttp.ClientSession(timeout=timeout, trust_env=False) as http:
            while True:
                ident = await self.queue.get()
                try:
                    row = self.raw.get(ident)
                    if row is None:
                        continue
                    cfg = config()
                    if not cfg["enabled"] or not cfg.get("trace_enabled") or not url:
                        continue
                    self.memory.check(32 * MIB, "信令解码")
                    kind, security, stamp, pdu = row
                    self.requests += 1
                    async with http.post(url + "/decode", data=pdu,
                            headers={"Authorization": "Bearer " + token,
                                     "X-Trace-Kind": str(kind), "Content-Type": "application/octet-stream"}) as response:
                        if response.status != 200:
                            raise ValueError("decoder unavailable")
                        data = bytearray()
                        async for chunk in response.content.iter_chunked(65536):
                            if len(data) + len(chunk) > MIB:
                                raise ValueError("decoder output limit")
                            data.extend(chunk)
                        doc = json.loads(data)
                    if not isinstance(doc, dict) or doc.get("state") not in ("ready", "error"):
                        raise ValueError("invalid decoder response")
                    version = doc.get("decoder_version")
                    if self.decoder_version and version != self.decoder_version:
                        self.decoded.clear(); self.decoded_bytes = 0
                    self.decoder_version = version
                    if doc["state"] == "error":
                        self.decode_errors += 1
                    doc.update(protocol=KINDS[kind], security=security, captured_at_us=str(stamp))
                    self.remember(ident, doc)
                except asyncio.CancelledError:
                    raise
                except (aiohttp.ClientError, ValueError, OSError, TimeoutError, MemoryError, MemoryPressure):
                    self.failures += 1
                    # Short-lived infrastructure failures remain retryable; never cache them forever.
                    await asyncio.sleep(.5)
                finally:
                    self.pending.discard(ident)
                    self.queue.task_done()

    async def context(self, app):
        async def versions():
            url = os.getenv("TRACE_DECODER_URL", "").rstrip("/")
            async with aiohttp.ClientSession(trust_env=False,
                    timeout=aiohttp.ClientTimeout(total=1)) as http:
                while True:
                    cfg = config()
                    if url and cfg["enabled"] and cfg.get("trace_enabled"):
                        try:
                            async with http.get(url + "/healthz") as response:
                                data = await response.content.read(4097)
                                if response.status != 200 or len(data) > 4096:
                                    raise ValueError("decoder health unavailable")
                                health = json.loads(data)
                            if health.get("redact_keys", True) is not self.redact_keys:
                                raise ValueError("decoder disclosure policy does not match collector")
                            version = health.get("decoder_version")
                            if not isinstance(version, str) or not 1 <= len(version) <= 128:
                                raise ValueError("invalid decoder version")
                            if self.decoder_version and version != self.decoder_version:
                                self.decoded.clear(); self.decoded_bytes = 0
                            self.decoder_version = version
                            self.decoder_ready = bool(health.get("ready"))
                        except (aiohttp.ClientError, OSError, ValueError, TimeoutError):
                            self.decoder_ready = False
                    else:
                        self.decoder_ready = False
                    await asyncio.sleep(5)
        tasks = [asyncio.create_task(self.worker()), asyncio.create_task(versions())]
        try:
            yield
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
