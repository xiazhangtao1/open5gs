"""Cache/source/API regression, including exact CLI fixture equivalence when XCNCTL is set."""
import asyncio
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cache import Cache, Curve, normalize_location
from collector import Collector
from server import application, validate_snapshot
from common import cpu_list, config
from memory import MemoryBudget, MemoryPressure, MIB
from aiohttp import ClientSession, web

# Share the existing CLI's source fixtures, rather than inventing a second schema.
argv = sys.argv[:]
sys.argv = [sys.argv[0], os.getenv("XCNCTL", "/nonexistent/xcnctl")]
spec = importlib.util.spec_from_file_location("cli_fixtures", Path(__file__).resolve().parents[2] / "tests/unit/xcnctl-test.py")
fixtures = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixtures)
sys.argv = argv


def snapshot():
    return dict(schema=1, ready=True, truncated=False, version="1", boot_id="test-boot", timestamp_us=str(int(time.time() * 1e6)),
                sessions=[dict(supi=fixtures.SUPI, seid="811", dnn="internet", ue_ip="10.45.0.3",
                               ipv4="10.45.0.3", ipv6="2001:db8:45::3", worker=0, uptime_seconds=9,
                               rules=[dict(qfi=1, direction="UL", pdr_id=1, qer_id=3, generation=1,
                                           bps="8000", pps="10", bytes="18446744073709551615", packets="10"),
                                      dict(qfi=1, direction="DL", pdr_id=2, qer_id=3, generation=1,
                                           bps="16000", pps="20", bytes="1000", packets="20")])])


def seeded():
    cache = Cache()
    cache.replace("amf_ues", [fixtures.ue()], 10)
    cache.replace("smf_pdus", [fixtures.pdu()], 10)
    cache.replace("upf_rates", snapshot()["sessions"], 2)
    return cache


class CacheTests(unittest.TestCase):
    def test_quiet_events_survive_without_time_limit(self):
        cache = Cache()
        stamp = time.time() - 7 * 86400
        cache.add_events("amf", dict(boot_id="old", items=[dict(seq="1", timestamp_us=str(int(stamp * 1e6)))]))
        with patch("cache.time.time", return_value=time.time() + 7 * 86400):
            body, _ = cache.response("events", {})
            result = json.loads(body)
            self.assertEqual(result["total"], 1)
            self.assertAlmostEqual(result["coverage"]["oldest_at"], stamp, places=4)
            self.assertIsNone(result["coverage"]["retention_seconds"])
        cache.max_events = 1
        cache.add_events("amf", dict(boot_id="old", items=[dict(seq="2", timestamp_us=str(int(time.time() * 1e6)))]))
        self.assertEqual([e[0]["seq"] for e in cache.events.values()], ["2"])
        self.assertEqual(cache.event_evictions, 1)
        cache.add_events("smf", dict(boot_id="late", items=[dict(seq="1", timestamp_us=str(int(stamp * 1e6)))]))
        self.assertEqual([e[0]["seq"] for e in cache.events.values()], ["2"])

    def test_memory_admission_reclaim_hysteresis_and_resume(self):
        used = [20 * MIB]
        memory = MemoryBudget(limit=128 * MIB, usage=lambda: used[0])
        cache = Cache(memory=memory)
        cache.replace("upf_rates", snapshot()["sessions"], 2)
        cache.record_rates(time.time())
        cache.add_events("amf", dict(boot_id="a", items=[dict(seq=str(i), timestamp_us=str(int(time.time() * 1e6)))
                                                       for i in range(1500)]))
        rows = copy.deepcopy(cache.sessions)
        used[0] = 91 * MIB
        with self.assertRaises(MemoryPressure):
            cache.replace("upf_rates", [], 2)
        self.assertEqual(cache.sessions, rows)
        self.assertEqual(len(cache.events), 1000)
        self.assertEqual(list(cache.curves), ["global"])
        self.assertTrue(memory.paused)
        used[0] = 80 * MIB
        with self.assertRaises(MemoryPressure):
            memory.check()
        used[0] = 60 * MIB
        cache.replace("upf_rates", [], 2)
        self.assertFalse(memory.paused)
        self.assertEqual(cache.sessions, [])
        self.assertEqual(memory.reclaims, 1)
        # Reject large wire documents before allocating decoded Python objects.
        with self.assertRaises(MemoryPressure):
            memory.before_json(2 * MIB)

    def test_rebuild_memory_failure_is_atomic(self):
        cache = seeded()
        previous = (cache.ues, cache.gnbs, cache.sessions)
        source = cache.source("upf_rates")["data"]
        original = copy.deepcopy
        calls = [0]
        def failing_clone(value):
            calls[0] += 1
            if calls[0] == 3:
                raise MemoryError("simulated clone failure")
            return original(value)
        with patch("cache.copy.deepcopy", side_effect=failing_clone):
            with self.assertRaises(MemoryError):
                cache.replace("upf_rates", [], 2)
        self.assertEqual((cache.ues, cache.gnbs, cache.sessions), previous)
        self.assertIs(cache.source("upf_rates")["data"], source)

    def test_session_correlations_precision_and_levels(self):
        cache = seeded()
        s = cache.sessions[0]
        self.assertEqual((s["psi"], s["bearers"][0]["5qi"], s["cm_state"]), (2, 9, "connected"))
        self.assertEqual(s["ul_bytes"], "18446744073709551615")
        self.assertEqual(cache.query("rates", {"level": "user"})["items"][0]["ul_bytes"], s["ul_bytes"])
        self.assertEqual(len(cache.query("rates", {"level": "rule"})["items"]), 2)
        self.assertEqual(cache.query("rates", {"level": "bearer"})["items"][0]["qfi"], 1)
        # Two sessions can reuse an address: SEID is authoritative.
        smf = fixtures.pdu()
        other = copy.deepcopy(smf["pdu"][0])
        other.update(psi=7, upf_seid="9007199254740993")
        smf["pdu"].append(other)
        cache.replace("smf_pdus", [smf], 10)
        self.assertEqual(cache.sessions[0]["psi"], 2)
        rows = copy.deepcopy(snapshot()["sessions"])
        rows[0]["seid"] = "9007199254740993"
        cache.replace("upf_rates", rows, 2)
        self.assertEqual(cache.sessions[0]["psi"], 7)
        self.assertFalse(cache.sessions[0]["active_in_amf"])
        cache.sessions[0]["seid"] = "8"
        cache.replace("upf_rates", cache.sessions, 2)
        self.assertIsNone(cache.sessions[0]["psi"])

    def test_ipv6_and_ambiguous_match(self):
        cache = seeded()
        rows = snapshot()["sessions"]
        rows[0]["ue_ip"] = "2001:0db8:0045:0:0:0:0:3"
        cache.replace("upf_rates", rows, 2)
        self.assertEqual(cache.sessions[0]["psi"], 2)
        smf = fixtures.pdu()
        smf["pdu"].append(copy.deepcopy(smf["pdu"][0]))
        cache.replace("smf_pdus", [smf], 10)
        self.assertIsNone(cache.sessions[0]["psi"])

    def test_slice_mismatch_and_idle_location(self):
        cache = seeded()
        smf = fixtures.pdu()
        smf["pdu"][0]["snssai"]["sd"] = "abcdef"
        cache.replace("smf_pdus", [smf], 10)
        self.assertEqual(cache.ues[0]["pdu_sessions"][0]["smf_info_status"], "not-matched")
        ue = fixtures.ue(bits=24, ident=0x123456, state="idle")
        ue["gnb"] = None
        cache.replace("amf_ues", [ue], 10)
        loc = cache.ues[0]["location"]
        self.assertEqual((loc["status"], loc["nr_cgi"]["cell_id"]), ("last-known", 3))
        self.assertEqual(cache.query("ues", {"gnb_id": str(0x123456), "gnb_id_bits": "24", "plmn": "00101"})["total"], 1)
        self.assertEqual(cache.sessions[0]["cm_state"], "idle")
        self.assertEqual(cache.sessions[0]["gnb_peer"], "[2001:db8::1]:38412")

    def test_events_identity_reset_gap_dedup_and_bounds(self):
        cache = Cache()
        cache.max_events = 3
        event = dict(seq="9007199254740993", trace_id="99", name="InitialUEMessage", supi="", timestamp_us="1")
        cache.add_events("amf", dict(boot_id="a", items=[event]))
        cache.add_events("amf", dict(boot_id="a", items=[dict(event, seq="9007199254740994", name="IdentityResolved", supi=fixtures.SUPI)]))
        self.assertEqual(next(iter(cache.events.values()))[0]["supi"], fixtures.SUPI)
        cache.add_events("amf", dict(boot_id="a", items=[event]))
        self.assertEqual(len(cache.events), 2)
        self.assertTrue(all(v[0]["supi"] == fixtures.SUPI for v in cache.events.values()))
        self.assertFalse(cache.anonymous)
        cache.add_events("smf", dict(boot_id="a", items=[event]))
        cache.add_events("amf", dict(boot_id="b", items=[event]))
        self.assertEqual(len(cache.events), 3)
        self.assertGreater(cache.event_evictions, 0)
        self.assertEqual(cache.event_bytes, sum(v[1] for v in cache.events.values()))

    def test_gnb_current_association_and_observed_cells_are_distinct(self):
        cache = seeded()
        current = fixtures.ue()
        idle = fixtures.ue(state="idle")
        idle["supi"] += "2"
        idle["gnb"] = None
        moving = fixtures.ue(ident=101)
        moving["supi"] += "3"
        moving["location"] = copy.deepcopy(current["location"])
        cache.replace("amf_ues", [current, idle, moving], 10)
        cache.replace("amf_gnbs", [dict(gnb_id=100, gnb_id_bits=28, plmn="00101",
                                          ng=dict(sctp=dict(peer=current["gnb"]["peer"])))], 15)
        self.assertEqual(len(cache.gnbs[0]["associated_ues"]), 1)
        cell = cache.gnbs[0]["observed_cells"][0]
        self.assertEqual((cell["ue_count"], cell["connected_ue_count"], cell["idle_ue_count"]), (3, 2, 1))
        smf = fixtures.pdu()
        smf["pdu"][0]["dnn"] = "different"
        cache.replace("smf_pdus", [smf], 10)
        self.assertIsNone(cache.sessions[0]["psi"])
        self.assertFalse(cache.sessions[0]["active_in_amf"])

    def test_last_complete_snapshot_and_staleness(self):
        cache = seeded()
        old = copy.deepcopy(cache.ues)
        with self.assertRaises(ValueError):
            cache.replace("amf_ues", [dict(supi=fixtures.SUPI, pdu_sessions=[None])], 10)
        self.assertEqual(cache.ues, old)
        cache.fail("amf_ues", "timeout")
        cache.source("amf_ues")["updated_at"] = time.time() - 31
        self.assertTrue(cache.health()["sources"]["amf_ues"]["stale"])
        self.assertEqual(cache.ues, old)

    def test_snapshot_validation(self):
        self.assertGreater(validate_snapshot(snapshot()), 0)
        for key, v in (("truncated", True), ("timestamp_us", "1"), ("sessions", [None])):
            doc = snapshot()
            doc[key] = v
            with self.assertRaises((ValueError, TypeError, AttributeError)):
                validate_snapshot(doc)
        doc = snapshot()
        doc["sessions"][0]["seid"] = 9007199254740993
        with self.assertRaises(ValueError):
            validate_snapshot(doc)

    def test_bounds_curves_memo_pagination(self):
        cache = seeded()
        cache.max_curves = 3
        for i in range(1000):
            cache.record_rates(time.time() - 2000 + i * 2)
        curve = cache.curves["global"]
        self.assertEqual(curve.count, 900)
        self.assertEqual(len(curve.data), 2700)
        self.assertLessEqual(len(cache.query("trends", {})["points"]), 900)
        for i in range(150):
            cache.response("ues", {"page": str(i)})
        self.assertLessEqual(len(cache.memo), 128)
        self.assertLessEqual(cache.memo_bytes, 8 * 1024 * 1024)
        self.assertEqual(cache.query("ues", {"limit": "100000"})["limit"], 100)
        self.assertEqual(cpu_list("2-3,6"), {2, 3, 6})
        cache.max_curves = 2
        cache.curves.clear()
        for i in range(20):
            cache.record_rates(time.time() + i * 2)
        self.assertEqual(len(cache.curves), 2)
        self.assertEqual(cache.curves["global"].count, 20, "global history must survive per-UE eviction")


class AsyncTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.app = application("t" * 64, collecting=False)
        self.app["collector"].collection_ready = True
        self.runner = web.AppRunner(self.app)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await self.site.start()
        self.base = "http://127.0.0.1:" + str(self.site._server.sockets[0].getsockname()[1])
        self.http = ClientSession(headers={"Authorization": "Bearer " + "t" * 64})

    async def asyncTearDown(self):
        await self.http.close()
        await self.runner.cleanup()

    async def test_auth_etag_and_no_reader_io(self):
        async with ClientSession() as plain:
            async with plain.get(self.base + "/api/overview") as response:
                self.assertEqual(response.status, 401)
        async with self.http.get(self.base + "/api/ues") as response:
            self.assertEqual(response.status, 200)
            etag = response.headers["ETag"]
        async with self.http.get(self.base + "/api/ues", headers={"If-None-Match": etag}) as response:
            self.assertEqual(response.status, 304)
        before = sum(s["requests"] for s in self.app["cache"].sources.values())
        async def read():
            async with self.http.get(self.base + "/api/overview") as response:
                self.assertEqual(response.status, 200)
                await response.read()
        await asyncio.gather(*(read() for _ in range(100)))
        self.assertEqual(before, sum(s["requests"] for s in self.app["cache"].sources.values()))
        async with self.http.get(self.base + "/api/ues?limit=no") as response:
            self.assertEqual(response.status, 400)

    async def test_snapshot_push_rejects_partial_retains_good(self):
        async with self.http.post(self.base + "/internal/upf", json=snapshot()) as response:
            self.assertEqual(response.status, 200)
        rows = copy.deepcopy(self.app["cache"].sessions)
        doc = snapshot()
        doc["truncated"] = True
        async with self.http.post(self.base + "/internal/upf", json=doc) as response:
            self.assertEqual(response.status, 400)
        self.assertEqual(rows, self.app["cache"].sessions)

    async def test_pressure_rejects_body_before_decode_and_keeps_health(self):
        cache = self.app["cache"]
        used = [20 * MIB]
        cache.memory = MemoryBudget(limit=128 * MIB, usage=lambda: used[0])
        async with self.http.post(self.base + "/internal/upf", json=snapshot()) as response:
            self.assertEqual(response.status, 200)
        old = copy.deepcopy(cache.sessions)
        used[0] = 91 * MIB
        async with self.http.post(self.base + "/internal/upf", json=snapshot()) as response:
            self.assertEqual(response.status, 503)
        self.assertEqual(cache.sessions, old)
        async with self.http.get(self.base + "/api/health") as response:
            self.assertEqual(response.status, 200)
            self.assertTrue((await response.json())["memory"]["paused"])
        async with self.http.get(self.base + "/internal/control") as response:
            self.assertFalse((await response.json())["enabled"])
        async with self.http.get(self.base + "/api/ues") as response:
            self.assertEqual(response.status, 503)
        used[0] = 50 * MIB
        async with self.http.post(self.base + "/internal/upf", json=snapshot()) as response:
            self.assertEqual(response.status, 200)

    async def test_paging_truncation_and_event_cursor(self):
        collector = self.app["collector"]
        async def paged(name, url, **kwargs):
            page = kwargs["params"]["page"]
            return json.dumps(dict(items=[dict(supi=str(page))], pager=dict(page=page, count=1, next="?page=1" if page == 0 else None))).encode()
        collector.request = paged
        self.assertEqual(len(await collector.pages("amf_ues", "unused")), 2)
        async def truncated(*args, **kwargs):
            return b'{"items":[],"pager":{"page":0,"count":0,"truncated":true}}'
        collector.request = truncated
        with self.assertRaises(ValueError):
            await collector.pages("amf_ues", "unused")
        params = []
        async def events(name, url, **kwargs):
            params.append(kwargs["params"])
            return json.dumps(dict(boot_id="new-boot", items=[], next_seq="9007199254740994",
                                   gap=True, reset=len(params) == 1, has_more=True)).encode()
        collector.request = events
        await collector.events("amf", 2)
        self.assertEqual(len(params), 2)
        self.assertNotIn("since_us", params[0])
        self.assertEqual(params[0]["after"], "0")
        self.assertEqual(params[1]["after"], "0")
        self.assertEqual(collector.cursors["amf"]["after"], "9007199254740994")
        self.assertEqual(collector.cache.source("amf_events")["gap_count"], 2)
        self.assertEqual(collector.cursors["amf"]["boot_id"], "new-boot")

    async def test_event_scientific_notation_timestamp(self):
        collector = self.app["collector"]
        # Actual cJSON wire format from AMF/SMF; uint64 IDs remain strings.
        stamp = "1.79269235803839e+15"
        async def events(*args, **kwargs):
            return ('{"boot_id":"live","next_seq":"9007199254740993",'
                    '"items":[{"seq":"9007199254740993","timestamp_us":'
                    + stamp + ',"name":"RegistrationRequest"}]}').encode()
        collector.request = events
        await collector.events("amf", 2)
        row = collector.cache.query("events", {})["items"][0]
        self.assertEqual(row["timestamp_us"], "1792692358038390")
        self.assertEqual(row["seq"], "9007199254740993")
        self.assertEqual(collector.cursors["amf"]["after"], "9007199254740993")
        for invalid in ("1.5", "NaN", "Infinity", "9007199254740992.0", "null", "true", "18446744073709551616"):
            stamp = invalid
            with self.subTest(stamp=invalid), self.assertRaises(ValueError):
                await collector.events("amf", 2)
            self.assertEqual(len(collector.cache.events), 1)
            self.assertEqual(collector.cursors["amf"]["after"], "9007199254740993")

    @unittest.skipUnless(os.getenv("XCNCTL"), "set XCNCTL for real CLI equivalence")
    async def test_cli_ue_equivalence(self):
        source = web.Application()
        async def info(request):
            rows = [fixtures.ue()] if request.path == "/ue-info" else [dict(gnb_id=100, gnb_id_bits=28, plmn="00101",
                ng=dict(setup_success=True, sctp=dict(peer="[2001:db8::1]:38412")))] if request.path == "/gnb-info" else [fixtures.pdu()]
            return web.json_response(dict(items=rows, pager=dict(page=0, count=len(rows))))
        source.router.add_get('/ue-info', info)
        source.router.add_get('/pdu-info', info)
        source.router.add_get('/gnb-info', info)
        runner = web.AppRunner(source)
        await runner.setup()
        site = web.TCPSite(runner, '127.0.0.1', 0)
        await site.start()
        base = 'http://127.0.0.1:' + str(site._server.sockets[0].getsockname()[1])
        try:
            process = await asyncio.create_subprocess_exec(os.environ['XCNCTL'], 'show', 'ue', '--detail', '--json',
                '--amf-ue-info', base + '/ue-info', '--smf-pdu-info', base + '/pdu-info', stdout=asyncio.subprocess.PIPE)
            out, _ = await process.communicate()
            self.assertEqual(process.returncode, 0)
            row = json.loads(out)['items'][0]
            ours = seeded().ues[0]
            for key in ('location', 'pdu_sessions', 'cm_state', 'gnb', 'security'):
                self.assertEqual(ours[key], row[key], key)
            process = await asyncio.create_subprocess_exec(os.environ['XCNCTL'], 'show', 'gnb', '--detail', '--json',
                '--amf-ue-info', base + '/ue-info', '--amf-gnb-info', base + '/gnb-info', stdout=asyncio.subprocess.PIPE)
            out, _ = await process.communicate()
            self.assertEqual(process.returncode, 0)
            row = json.loads(out)['items'][0]
            cache = seeded()
            cache.replace('amf_gnbs', [dict(gnb_id=100, gnb_id_bits=28, plmn='00101', ng=dict(sctp=dict(peer='[2001:db8::1]:38412')))], 15)
            ours = cache.gnbs[0]
            for key in ('ue_info_available', 'observed_cells', 'observed_cells_source'):
                self.assertEqual(ours[key], row[key], key)
            for k in ('supi', 'cm_state', 'gnb', 'pdu_sessions_count'):
                self.assertEqual(ours['associated_ues'][0][k], row['associated_ues'][0][k])
        finally:
            await runner.cleanup()


if __name__ == '__main__':
    unittest.main()
