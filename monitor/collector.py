import asyncio
import json
import logging
import math
import os
import re
import ssl
import time
from pathlib import Path

import aiohttp
from cache import encode
from common import config, cpu_list, pin_cpu, read_limited, siblings
from memory import MemoryPressure, MIB

LOG = logging.getLogger("xcn-monitor")


class Collector:
    def __init__(self, cache):
        self.cache = cache
        self.http = None
        self.cursors = {}
        self.tasks = []
        self.collection_ready = False
        self.bases = {nf: os.getenv(f"{nf.upper()}_METRICS_URL", f"http://xcn-{nf}:9090")
                      for nf in ("amf", "smf", "upf")}
        self.next_due = {}

    async def request(self, name, url, maximum=2 * 1024 * 1024, **kwargs):
        source = self.cache.source(name)
        source["requests"] += 1
        source["attempted_at"] = time.time()
        async with self.http.get(url, **kwargs) as response:
            return await read_limited(response, maximum, self.cache.memory)

    async def pages(self, name, url):
        return await asyncio.wait_for(self._pages(name, url), 3)

    async def _pages(self, name, url):
        rows, total = [], 0
        for page in range(41):
            data = await self.request(name, url, params={"page": page, "page_size": 100})
            total += len(data)
            if total > 8 * 1024 * 1024:
                raise ValueError("source total byte budget exceeded")
            doc = json.loads(data)
            if not isinstance(doc, dict):
                raise ValueError("invalid source document")
            items, pager = doc.get("items"), doc.get("pager", {})
            if (not isinstance(items, list) or not isinstance(pager, dict) or len(items) > 100 or pager.get("page") != page
                    or pager.get("count") != len(items) or pager.get("truncated", False)
                    or not all(isinstance(r, dict) for r in items) or doc.get("error")):
                raise ValueError("incomplete or invalid paged source")
            rows.extend(items)
            if len(rows) > 4096:
                raise ValueError("source item budget exceeded")
            if pager.get("next") is None:
                return rows
            if not isinstance(pager["next"], str) or not pager["next"] or not items:
                raise ValueError("invalid next page")
        raise ValueError("source page budget exceeded")

    async def events(self, nf, interval):
        name = f"{nf}_events"
        cursor = self.cursors.get(nf, {})
        for _ in range(2):
            params = dict(limit=1000, detail=1, **cursor)
            params.setdefault("after", "0")  # Drain retained NF history, not only its newest page.
            doc = json.loads(await self.request(name, self.bases[nf] + "/ue-events", params=params))
            if (not isinstance(doc, dict) or not isinstance(doc.get("items"), list) or len(doc["items"]) > 1000
                    or not isinstance(doc.get("boot_id"), str) or not doc["boot_id"]
                    or not str(doc.get("next_seq", "")).isdigit()):
                raise ValueError("invalid event page")
            for e in doc["items"]:
                # cJSON can emit integral microsecond timestamps in scientific
                # notation, which json.loads decodes as float. Only normalize
                # exactly representable integers; keep sequence IDs strict.
                stamp = e.get("timestamp_us") if isinstance(e, dict) else None
                if (isinstance(stamp, float) and math.isfinite(stamp)
                        and stamp.is_integer() and 0 <= stamp < 1 << 53):
                    e["timestamp_us"] = str(int(stamp))
                if (not isinstance(e, dict) or not str(e.get("seq", "")).isdigit()
                        or not str(e.get("timestamp_us", "")).isdigit()
                        or int(e["seq"]) >= 1 << 64 or int(e["timestamp_us"]) >= 1 << 64):
                    raise ValueError("invalid event record")
            self.cache.add_events(nf, doc)
            source = self.cache.source(name)
            # Sticky counters preserve visibility after the next successful poll.
            for flag in ("gap", "reset"):
                if doc.get(flag):
                    source[flag + "_count"] = source.get(flag + "_count", 0) + 1
            # A boot mismatch response is a newest-first page. Restart incremental
            # traversal at zero so older records in the new NF ring aren't skipped.
            next_seq = "0" if doc.get("reset") else str(doc["next_seq"])
            source.update(boot_id=doc["boot_id"], cursor=next_seq,
                          has_more=bool(doc.get("has_more") or doc.get("reset")), oldest_seq=str(doc.get("oldest_seq", "")),
                          overwritten_total=str(doc.get("overwritten_total", "0")),
                          updated_at=time.time(), error=None, errors=0, interval=interval)
            cursor = dict(after=next_seq, boot_id=doc["boot_id"])
            self.cursors[nf] = cursor
            if not doc.get("has_more") and not doc.get("reset"):
                break

    async def metrics(self, nf, interval):
        name = nf + "_metrics"
        text = (await self.request(name, self.bases[nf] + "/metrics", maximum=1024 * 1024)).decode()
        metrics = []
        for line in text.splitlines():
            match = re.fullmatch(r'([a-zA-Z_:][a-zA-Z0-9_:]*(?:\{.*\})?)\s+([^\s]+)(?:\s+\d+)?', line)
            if match:
                value = float(match[2])
                if math.isfinite(value):
                    metrics.append(dict(name=match[1][:512], value=match[2]))
            if len(metrics) >= 2000:
                raise ValueError("metric series budget exceeded")
        self.cache.replace(name, metrics, interval)

    async def kube(self, path, name):
        token = Path("/var/run/secrets/kubernetes.io/serviceaccount/token").read_text().strip()
        ca = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt"
        host = os.getenv("KUBERNETES_SERVICE_HOST", "kubernetes.default.svc")
        port = os.getenv("KUBERNETES_SERVICE_PORT_HTTPS", "443")
        data = await self.request(name, f"https://{host}:{port}{path}", maximum=2 * 1024 * 1024,
                                  ssl=ssl.create_default_context(cafile=ca),
                                  headers={"Authorization": "Bearer " + token})
        return json.loads(data)

    async def pods(self, interval):
        namespace = os.getenv("POD_NAMESPACE", "xcn")
        doc = await self.kube(f"/api/v1/namespaces/{namespace}/pods", "pods")
        own_node = os.getenv("NODE_NAME")
        reserved, trace_reserved, rows = set(), set(), []
        for p in doc.get("items", []):
            meta, spec, status = p.get("metadata", {}), p.get("spec", {}), p.get("status", {})
            containers = status.get("containerStatuses", [])
            rows.append(dict(name=meta.get("name"), node=spec.get("nodeName"), phase=status.get("phase"),
                             containers=[dict(name=c["name"], ready=c.get("ready", False),
                                              restarts=c.get("restartCount", 0), state=c.get("state"))
                                         for c in containers]))
            if spec.get("nodeName") != own_node:
                continue
            assigned = json.loads(meta.get("annotations", {}).get("xnet.com/used-core", "{}"))
            for name in ("upf", "vpp"):
                if name in assigned:
                    reserved.update(cpu_list(str(assigned[name].get("cpu", ""))))
            for name in ("amf", "smf"):
                if name in assigned:
                    trace_reserved.update(cpu_list(str(assigned[name].get("cpu", ""))))
        if not reserved and os.getenv("MONITOR_REQUIRE_AFFINITY", "1") == "1":
            raise ValueError("无法取得本节点 UPF/VPP CPU 分配，已停止采集；检查 xnet.com/used-core 注解")
        try:
            # Revalidate when UPF/VPP placement changes. Affinity is never widened.
            cpus = pin_cpu(reserved)
            self.cache.affinity = dict(verified=True, cpus=cpus, reserved=sorted(reserved), checked_at=time.time())
            trace_reserved.update(reserved)
            self.cache.trace_affinity = dict(verified=not bool(set(cpus) & siblings(trace_reserved)),
                                            cpus=cpus, reserved=sorted(trace_reserved), checked_at=time.time())
            self.collection_ready = True
        except (OSError, ValueError) as exc:
            self.collection_ready = False
            self.cache.affinity = dict(verified=False, error=str(exc))
            raise
        self.cache.replace("pods", rows, interval)

    async def nf_loop(self, nf):
        # One serial scheduler per NF: event and detail reads cannot overlap.
        jobs = ["events", "info"]
        if nf == "amf":
            jobs.append("gnbs")
        while True:
            cfg = config()
            self.cache.enabled = cfg["enabled"]
            if cfg["enabled"] and self.collection_ready:
                for job in jobs:
                    key = nf + "_" + job
                    interval = cfg[job + "_interval"]
                    if time.monotonic() < self.next_due.get(key, 0):
                        continue
                    source = dict(info=nf + ("_ues" if nf == "amf" else "_pdus"),
                                  events=nf + "_events", metrics=nf + "_metrics", gnbs="amf_gnbs")[job]
                    try:
                        self.cache.admit(16 * MIB, "周期采集")
                        if job == "events":
                            await self.events(nf, interval)
                        elif job == "metrics":
                            await self.metrics(nf, interval)
                        else:
                            path = "/gnb-info" if job == "gnbs" else "/ue-info" if nf == "amf" else "/pdu-info"
                            rows = await self.pages(source, self.bases[nf] + path)
                            self.cache.replace(source, rows, interval)
                    except (aiohttp.ClientError, OSError, ValueError, KeyError, TypeError, TimeoutError, MemoryPressure, MemoryError) as exc:
                        self.cache.relieve_memory()
                        self.cache.fail(source, exc)
                    finally:
                        rows = None
                    # Skip missed ticks; no backlog or immediate catch-up burst.
                    errors = self.cache.source(source)["errors"]
                    self.next_due[key] = time.monotonic() + min(max(60, interval), interval * 2 ** min(errors, 4))
            await asyncio.sleep(0.2)

    async def pod_loop(self):
        local = os.getenv("MONITOR_REQUIRE_AFFINITY", "1") != "1"
        if local:
            self.cache.affinity = dict(verified=False, local_validation=True, cpus=pin_cpu())
            self.collection_ready = True
        while True:
            if not local:
                try:
                    await self.pods(config()["pods_interval"])
                except (aiohttp.ClientError, OSError, ValueError, KeyError, TypeError, TimeoutError, MemoryPressure, MemoryError) as exc:
                    self.cache.relieve_memory()
                    self.collection_ready = False
                    self.cache.affinity = dict(verified=False, error=str(exc)[:200])
                    self.cache.fail("pods", exc)
            await asyncio.sleep(config()["pods_interval"])

    async def context(self, app):
        timeout = aiohttp.ClientTimeout(total=0.8, connect=0.2)
        async with aiohttp.ClientSession(timeout=timeout, trust_env=False,
                                         connector=aiohttp.TCPConnector(limit=4)) as self.http:
            self.tasks = [asyncio.create_task(self.pod_loop()), asyncio.create_task(self.memory_loop())]
            self.tasks += [asyncio.create_task(self.nf_loop(nf)) for nf in ("amf", "smf")]
            try:
                yield
            finally:
                for task in self.tasks:
                    task.cancel()
                await asyncio.gather(*self.tasks, return_exceptions=True)

    async def memory_loop(self):
        while True:
            self.cache.relieve_memory()
            await asyncio.sleep(1)
