"""One collector/cache replica. Public access is through WebUI's read-only JWT proxy."""
import asyncio
import hmac
import json
import logging
import math
import os
import time

from aiohttp import web
from cache import Cache
from signals import Signals, MAX_BATCH
from collector import Collector
from common import config, wait_for_cpu
from memory import MemoryPressure, UPF_BODY_LIMIT, MIB

ENDPOINTS = {"overview", "ues", "gnbs", "sessions", "rates", "events", "trends", "health", "system", "signal"}


def validate_snapshot(doc):
    if (not isinstance(doc, dict) or doc.get("schema") != 1 or not doc.get("ready") or doc.get("truncated")
            or not isinstance(doc.get("sessions"), list) or len(doc["sessions"]) > 4096):
        raise ValueError("UPF snapshot unavailable or truncated")
    if (not isinstance(doc.get("boot_id"), str) or not 1 <= len(doc["boot_id"]) <= 128
            or not isinstance(doc.get("version"), str) or not doc["version"].isdigit()):
        raise ValueError("invalid UPF boot/version identity")
    stamp = int(doc["timestamp_us"]) / 1e6
    if not math.isfinite(stamp) or abs(time.time() - stamp) > 6:
        raise ValueError("UPF snapshot expired or clock skew exceeds 6 seconds")
    rules = 0
    for s in doc["sessions"]:
        if not isinstance(s, dict) or not isinstance(s.get("rules"), list):
            raise ValueError("invalid session")
        if not isinstance(s.get("seid"), str) or not s["seid"].isdigit() or not 0 <= int(s["seid"]) < 1 << 64:
            raise ValueError("SEID must be a decimal uint64 string")
        for key in ("supi", "dnn", "ue_ip", "ipv4", "ipv6"):
            if not isinstance(s.get(key, ""), str) or len(s.get(key, "")) > 256:
                raise ValueError("invalid session text")
        rules += len(s["rules"])
        if rules > 65536 or len(s["rules"]) > 96:
            raise ValueError("rule budget exceeded")
        for r in s["rules"]:
            if (not isinstance(r, dict) or r.get("direction") not in ("UL", "DL")
                    or type(r.get("qfi")) is not int or not 0 <= r["qfi"] <= 63):
                raise ValueError("invalid rule")
            for key in ("src_if", "dst_if"):
                if key in r and (type(r[key]) is not int or r[key] not in (0, 1, 2, 3, 4, 255)):
                    raise ValueError("invalid PFCP interface")
            for key in ("bps", "pps", "bytes", "packets"):
                v = r.get(key)
                if not isinstance(v, str) or not v.isdigit() or not 0 <= int(v) < 1 << 64:
                    raise ValueError("counter must be a decimal uint64 string")
    return stamp


def application(token=None, collecting=True):
    token = token or os.getenv("MONITOR_INTERNAL_TOKEN", "")
    if len(token) < 32:
        raise ValueError("MONITOR_INTERNAL_TOKEN must contain at least 32 characters")
    cache = Cache()
    collector = Collector(cache)
    signals = Signals(cache.memory)
    cache.signals = signals
    app = web.Application(client_max_size=UPF_BODY_LIMIT)
    app["cache"], app["collector"] = cache, collector
    ingest = {"busy": False}

    @web.middleware
    async def auth(request, handler):
        if request.path != "/healthz" and not hmac.compare_digest(
                request.headers.get("Authorization", ""), "Bearer " + token):
            raise web.HTTPUnauthorized()
        return await handler(request)

    app.middlewares.append(auth)

    async def healthz(request):
        return web.json_response({"ok": True})

    async def api(request):
        endpoint = request.match_info["endpoint"]
        if endpoint not in ENDPOINTS:
            raise web.HTTPNotFound()
        if len(request.query_string) > 1024 or len(request.query) > 16:
            raise web.HTTPBadRequest(text="query budget exceeded")
        query = dict(request.query)
        if any(len(k) > 32 or len(v) > 128 for k, v in query.items()):
            raise web.HTTPBadRequest(text="query too long")
        try:
            if endpoint == "signal":
                doc = signals.query(query.get("id", ""), query.get("view", "decoded"))
                return web.json_response(doc, headers={"Cache-Control": "no-store"})
            body, etag = cache.response(endpoint, query)
        except (MemoryPressure, MemoryError):
            cache.relieve_memory()
            raise web.HTTPServiceUnavailable(text="内存保护已暂停此查询，请稍后重试")
        except (ValueError, TypeError, OverflowError):
            raise web.HTTPBadRequest(text="invalid query")
        headers = {"ETag": etag, "Cache-Control": "private, no-cache"}
        if request.headers.get("If-None-Match") == etag:
            return web.Response(status=304, headers=headers)
        return web.Response(body=body, content_type="application/json", headers=headers)

    async def control(request):
        cfg = config()
        available = True
        try:
            cache.admit(16 * MIB, "UPF 采集准入")
        except MemoryPressure:
            available = False
        return web.json_response(dict(enabled=cfg["enabled"] and collector.collection_ready and available,
                                      interval=cfg["rates_interval"], affinity=cache.affinity))

    async def read_json(request, maximum):
        if request.content_length and request.content_length > maximum:
            raise web.HTTPRequestEntityTooLarge(max_size=maximum, actual_size=request.content_length)
        cache.memory.before_json(request.content_length or 0)
        data = bytearray()
        async for chunk in request.content.iter_chunked(65536):
            size = len(data) + len(chunk)
            if size > maximum:
                raise web.HTTPRequestEntityTooLarge(max_size=maximum, actual_size=size)
            cache.memory.before_json(size)
            data.extend(chunk)
        return json.loads(data)

    async def snapshot(request):
        if not config()["enabled"] or not collector.collection_ready:
            raise web.HTTPServiceUnavailable(text="collection paused or CPU affinity unverified")
        if ingest["busy"]:
            raise web.HTTPServiceUnavailable(text="previous snapshot in progress; no queue")
        ingest["busy"] = True
        source = cache.source("upf_rates")
        source["requests"] += 1
        source["attempted_at"] = time.time()
        try:
            doc = await asyncio.wait_for(read_json(request, UPF_BODY_LIMIT), 2)
            stamp = validate_snapshot(doc)
            identity = (str(doc.get("boot_id", "")), str(doc.get("version", "")), str(doc["timestamp_us"]))
            if source.get("identity") != identity:
                monotonic = doc.get('monotonic_us')
                if monotonic is not None:
                    if not str(monotonic).isdigit() or not 0 < int(monotonic) < 1 << 64:
                        raise ValueError('invalid snapshot monotonic timestamp')
                    monotonic = int(monotonic)
                if source.get('identity') and source['identity'][0] == identity[0]:
                    previous_mono = source.get('monotonic_us')
                    if ((monotonic is not None and previous_mono is not None and monotonic <= previous_mono) or
                            (monotonic is None and stamp <= source.get('sampled_at', 0))):
                        return web.json_response({'ok': True, 'ignored': 'out-of-order'})
                if source.get("identity") and source["identity"][0] != identity[0]:
                    source["reset_count"] = source.get("reset_count", 0) + 1
                    for key in list(cache.curves):
                        if key.startswith(("session:", "bearer:")):
                            del cache.curves[key]
                cache.replace("upf_rates", doc["sessions"], config()["rates_interval"])
                cache.record_rates(stamp, boot_id=identity[0], interval=config()['rates_interval'])
                source.update(identity=identity, sampled_at=stamp, sample_us=doc.get("sample_us"),
                              monotonic_us=monotonic,
                              lock_wait_us=doc.get("lock_wait_us"), lock_hold_us=doc.get("lock_hold_us"))
        except (MemoryPressure, MemoryError) as exc:
            cache.relieve_memory()
            cache.fail("upf_rates", str(exc) or "内存保护已暂停新快照")
            raise web.HTTPServiceUnavailable(text="memory budget exceeded; retained last snapshot")
        except (ValueError, TypeError, KeyError, TimeoutError, RecursionError) as exc:
            cache.fail("upf_rates", exc)
            raise web.HTTPBadRequest(text=str(exc)[:200])
        finally:
            ingest["busy"] = False
        return web.json_response({"ok": True})

    async def agent_error(request):
        # Error reports are tiny and remain available during memory protection.
        if request.content_length is None or request.content_length > 4096:
            raise web.HTTPBadRequest(text="error report too large")
        try:
            doc = json.loads(await request.read())
            if not isinstance(doc, dict):
                raise ValueError("invalid report")
        except (ValueError, TypeError):
            raise web.HTTPBadRequest(text="invalid error report")
        cache.fail("upf_rates", str(doc.get("error", "UPF agent unavailable"))[:200])
        return web.json_response({"ok": True})

    async def trace_control(request):
        available = config()["enabled"] and config()["trace_enabled"] and collector.collection_ready
        try:
            cache.admit(32 * MIB, "信令采集准入")
        except MemoryPressure:
            available = False
        affinity = getattr(cache, "trace_affinity", cache.affinity)
        available = available and affinity.get("verified", False)
        return web.json_response(dict(enabled=bool(available), affinity=affinity))

    async def ingest_signals(request):
        if not config()["enabled"] or not config()["trace_enabled"] or not collector.collection_ready:
            raise web.HTTPServiceUnavailable()
        try:
            cache.admit(20 * MIB, "信令批次")
            data = bytearray()
            async def read():
                async for chunk in request.content.iter_chunked(65536):
                    if len(data) + len(chunk) > MAX_BATCH:
                        raise web.HTTPRequestEntityTooLarge(max_size=MAX_BATCH, actual_size=len(data) + len(chunk))
                    data.extend(chunk)
            await asyncio.wait_for(read(), 1)
            signals.admit(bytes(data))
            cache.changed()
        except (MemoryPressure, MemoryError):
            cache.relieve_memory()
            raise web.HTTPServiceUnavailable()
        except (ValueError, UnicodeError, TimeoutError):
            raise web.HTTPBadRequest(text="invalid trace batch")
        return web.json_response({"ok": True})

    async def trace_status(request):
        doc = await read_json(request, 4096)
        if not isinstance(doc, dict) or set(doc) - {"rings", "upload_dropped"}:
            raise web.HTTPBadRequest()
        signals.agent = doc
        return web.json_response({"ok": True})

    app.router.add_get("/internal/trace-control", trace_control)
    app.router.add_post("/internal/signals", ingest_signals)
    app.router.add_post("/internal/trace-status", trace_status)
    app.cleanup_ctx.append(signals.context)
    app.router.add_get("/healthz", healthz)
    app.router.add_get("/api/{endpoint}", api)
    app.router.add_get("/internal/control", control)
    app.router.add_post("/internal/upf", snapshot)
    app.router.add_post("/internal/upf-error", agent_error)
    if collecting:
        app.cleanup_ctx.append(collector.context)
    return app


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    wait_for_cpu()
    web.run_app(application(), host=os.getenv("MONITOR_HOST", "0.0.0.0"),
                port=int(os.getenv("MONITOR_PORT", "9998")), access_log=None)
