"""Explicit local-only synthetic source for UI/load tests, never used by deployments.

100 UEs x 2 sessions x 4 QFIs. Run alongside the REAL collector and UPF agent.
"""
import asyncio
from collections import Counter
import copy
import json
import math
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aiohttp import web

BOOT = "monitor-lab-" + str(time.time_ns())
started = time.time()
counts = Counter()
ues, pdus, sessions, gnbs = [], [], [], []
for i in range(3):
    gnbs.append(dict(gnb_id=100 + i, gnb_id_bits=28, plmn="00101", num_connected_ues=25,
                     ng=dict(setup_success=True, sctp=dict(peer=f"192.0.2.{10 + i}:38412", local="192.0.2.1:38412", status="ESTABLISHED")),
                     supported_ta_list=[dict(tac=1, broadcast_plmn_list=[dict(plmn="00101", snssai_list=[dict(sst=1, sd="010101")])])]))
for i in range(100):
    supi = f"imsi-0010100{i:08d}"
    gnb = dict(gnb_id=100 + i % 3, gnb_id_bits=28, plmn="00101", peer=f"192.0.2.{10 + i % 3}:38412")
    u = dict(supi=supi, cm_state="connected" if i < 75 else "idle", identity=dict(suci="suci-lab", pei="imei-lab"),
             security=dict(valid=True, enc="nea0", int="nia2"), gnb=dict(gnb, status="associated") if i < 75 else None,
             location=dict(timestamp=int(started * 1e6), gnb=gnb, nr_cgi=dict(plmn="00101", nci=(gnb["gnb_id"] << 8) | 3),
                           nr_tai=dict(plmn="00101", tac=1)), pdu_sessions=[])
    pd = dict(supi=supi, pdu=[])
    for j in range(2):
        seid = str(9007199254740993 + i * 2 + j)
        p = dict(psi=j + 1, dnn=["internet", "ims"][j], upf_seid=seid, ipv4=f"10.45.{j}.{i + 2}",
                 ipv6=f"2001:db8:{j}::{i + 2:x}", snssai=dict(sst=1, sd="010101"), pdu_state="active",
                 qos_flows=[{"qfi": q, "5qi": 9 if q == 1 else 7} for q in range(1, 5)],
                 n3=dict(upf=dict(addr="192.0.2.1:2152", teid=1000 + i), gnb=dict(addr="192.0.2.10:2152", teid=2000 + i)))
        pd["pdu"].append(p)
        u["pdu_sessions"].append(dict(psi=p["psi"], dnn=p["dnn"], snssai=p["snssai"]))
        s = dict(supi=supi, seid=seid, dnn=p["dnn"], ue_ip=p["ipv4"], ipv4=p["ipv4"], ipv6=p["ipv6"],
                 worker=i % 2, uptime_seconds=600, rules=[])
        for q in range(1, 5):
            for d in ("UL", "DL"):
                s["rules"].append(dict(qfi=q, direction=d, pdr_id=q * 2 - (d == "UL"), qer_id=q, generation=1,
                                       bps="8000", pps="10", bytes="9007199254740993", packets="100000"))
        sessions.append(s)
    ues.append(u)
    pdus.append(pd)


async def source(request):
    nf, kind = request.match_info["nf"], request.match_info["kind"]
    counts[nf + "/" + kind] += 1
    if kind == "metrics":
        return web.Response(text="# TYPE ues_active gauge\nues_active 100\npfcp_sessions_active 200\n")
    if kind == "ue-events":
        latest = 12000 + int((time.time() - started) / 2)
        after = int(request.query.get("after", 0))
        reset = request.query.get("boot_id") not in (None, BOOT)
        if reset:
            after = 0
        seqs = range(max(1, after + 1), min(latest + 1, after + 1001))
        items = [dict(event_id=f"{nf}/{BOOT}/{n}", seq=str(n), trace_id=str(n % 100), timestamp_us=str(int((started - (12000 - n) * .05 if n <= 12000 else started + (n - 12000) * 2) * 1e6)),
                      name="Registration" if n % 4 else "SessionEstablishment", protocol="NAS-5GMM" if nf == "amf" else "PFCP",
                      direction="UE->AMF" if nf == "amf" else "SMF->UPF", action=True,
                      result="rejected" if n % 61 == 0 else "completed", supi=ues[n % 100]["supi"], psi=1,
                      info="实验数据：注册拒绝，检查订阅配置" if n % 61 == 0 else "实验数据：过程完成") for n in seqs]
        cursor = items[-1]["seq"] if items else str(latest)
        return web.json_response(dict(boot_id=BOOT, items=items, next_seq=cursor, latest_seq=str(latest), oldest_seq="1",
                                      has_more=int(cursor) < latest, gap=False, reset=reset, overwritten_total="0"))
    rows = {"ue-info": ues, "gnb-info": gnbs, "pdu-info": pdus}[kind]
    page, limit = int(request.query.get("page", 0)), int(request.query.get("page_size", 100))
    items = rows[page * limit:(page + 1) * limit]
    return web.json_response(dict(items=items, pager=dict(page=page, count=len(items),
        next=f"?page={page + 1}" if (page + 1) * limit < len(rows) else None)))


async def socket(reader, writer):
    try:
        await reader.readline()
        counts["upf/snapshot"] += 1
        now = time.time()
        rows = copy.deepcopy(sessions)
        for i, s in enumerate(rows):
            for r in s["rules"]:
                r["bps"] = str(int((100000 + i * 200) * (1 + .3 * math.sin((now - started) / 15)) * (1 if r["direction"] == "UL" else 2.8)))
        doc = dict(schema=1, ready=True, truncated=False, timestamp_us=str(int(now * 1e6)),
                   monotonic_us=str(int(time.monotonic() * 1e6)), sample_us=1000000,
                   version=str(int(now)), boot_id=BOOT, sessions=rows)
        writer.write(json.dumps(doc, separators=(",", ":")).encode())
        await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        writer.close()
        await writer.wait_closed()


async def main():
    path = Path(os.getenv("UPF_STATS_SOCKET", "/tmp/xcn-monitor-lab/upf.sock"))
    path.parent.mkdir(exist_ok=True)
    path.unlink(missing_ok=True)
    uds = await asyncio.start_unix_server(socket, str(path))
    app = web.Application()
    app.router.add_get('/{nf:amf|smf|upf}/{kind:ue-info|pdu-info|gnb-info|ue-events|metrics}', source)
    async def status(request):
        return web.json_response(dict(counts))
    app.router.add_get('/counts', status)
    runner = web.AppRunner(app, access_log=None)
    await runner.setup()
    await web.TCPSite(runner, '127.0.0.1', 19090).start()
    print('LAB ONLY: 100 UEs / 200 sessions / 800 QFIs; HTTP 127.0.0.1:19090 and ' + str(path), flush=True)
    try:
        await asyncio.Event().wait()
    finally:
        uds.close()
        await uds.wait_closed()
        await runner.cleanup()

if __name__ == '__main__':
    asyncio.run(main())
