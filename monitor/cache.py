"""Bounded, process-local monitoring state. Readers never contact a network function."""
import copy
import hashlib
import ipaddress
import json
import math
import sys
import time
import uuid
from collections import OrderedDict
from memory import MemoryBudget, MemoryPressure, container_bytes, MIB
from trends import Curve


def encode(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode()


def compatible(a, b):
    if a.get("dnn") is not None and b.get("dnn") is not None and a["dnn"] != b["dnn"]:
        return False
    x, y = a.get("snssai") or {}, b.get("snssai") or {}
    if x.get("sst") is not None and y.get("sst") is not None and x["sst"] != y["sst"]:
        return False
    return not (isinstance(x.get("sd"), str) and isinstance(y.get("sd"), str)
                and x["sd"].lower() != y["sd"].lower())


def gnb_identity(gnb):
    if not isinstance(gnb, dict):
        return None
    ident, bits = gnb.get("gnb_id"), gnb.get("gnb_id_bits")
    if (type(ident) is int and type(bits) is int and 22 <= bits <= 32
            and 0 <= ident < 1 << bits):
        return ident, bits
    return None


def gnb_key(gnb):
    identity = gnb_identity(gnb)
    plmn = gnb.get("plmn") if isinstance(gnb, dict) else None
    return (plmn, *identity) if identity and isinstance(plmn, str) and plmn not in ("", "-") else None


def normalize_location(ue):
    """Mirror xcnctl's 36-bit NCI split; never assume a 22-bit gNB ID."""
    loc = ue.get("location") or {}
    stamp = loc.get("timestamp")
    # cJSON may use scientific notation for integral microsecond timestamps.
    # Accept only exact integers in the safe JSON numeric range.
    if type(stamp) is float and 0 < stamp <= 9007199254740991 and stamp.is_integer():
        loc["timestamp"] = stamp = int(stamp)
    if type(stamp) is not int or not 0 < stamp <= 9007199254740991:
        ue["location"] = dict(timestamp=None, status="unknown", gnb=None, nr_cgi=None, nr_tai=None)
        return
    loc["status"] = {"connected": "reported", "idle": "last-known"}.get(ue.get("cm_state"), "unknown")
    gnb = loc.get("gnb") or {}
    cgi = loc.get("nr_cgi")
    bits = gnb.get("gnb_id_bits", (cgi or {}).get("gnb_id_bits"))
    known = gnb_identity(gnb)
    if not known:
        loc["gnb"] = None
    if isinstance(cgi, dict):
        nci = cgi.get("nci")
        valid = type(nci) is int and 0 <= nci < 1 << 36
        decoded = valid and type(bits) is int and 22 <= bits <= 32
        cgi.update(nci=nci if valid else None, nci_hex=f"0x{nci:09x}" if valid else None,
                   gnb_id_bits=bits if type(bits) is int and 22 <= bits <= 32 else None,
                   gnb_id=nci >> (36 - bits) if decoded else None,
                   cell_id=nci & ((1 << (36 - bits)) - 1) if decoded else None,
                   identity_consistent=(nci >> (36 - bits) == known[0]) if known and decoded else None)
    ue["location"] = loc


class Cache:
    trend_retention = 86400
    max_trend_bytes = 64 * MIB
    max_events = 20000
    max_event_bytes = 16 * 1024 * 1024
    max_curves = 512

    def __init__(self, memory=None):
        self.started_at = time.time()
        self.version = 0
        self.sources = {}
        self.ues, self.gnbs, self.sessions = [], [], []
        self.events = OrderedDict()
        self.anonymous = {}
        self.event_bytes = 0
        self.event_evictions = 0
        self.curves = OrderedDict()
        self.curve_evictions = 0
        self.curve_rejections = 0
        self.trend_epoch = uuid.uuid4().hex
        self.trend_stamp = 0
        self.trend_boot = None
        self.trend_interval = 2
        self.memo = OrderedDict()
        self.memo_bytes = 0
        self.request_count = 0
        self.enabled = True
        self.affinity = {}
        self.event_range = None
        self.memory = memory or MemoryBudget()
        self.view_bytes = 0
        self.last_reclaim = 0
        self.signals = None

    def relieve_memory(self):
        if self.memory.sample(force=True) < self.memory.high or time.monotonic() - self.last_reclaim < 5:
            return
        self.last_reclaim = time.monotonic()
        if self.signals:
            self.signals.trim(pressure=True)
        # Preserve latest NF/session state; history is expendable under pressure.
        self.evict_events(1000, self.max_event_bytes)
        for key in list(self.curves):
            if key != "global":
                del self.curves[key]
                self.curve_evictions += 1
        self.memo.clear()
        self.memo_bytes = 0
        self.memory.reclaims += 1
        self.memory.trim_allocator()
        self.memory.sample(force=True)
        self.changed()

    def admit(self, reserve=0, operation="collection"):
        try:
            self.memory.check(reserve, operation)
        except MemoryPressure:
            self.relieve_memory()
            raise

    def event_coverage(self):
        if self.event_range is None:
            self.event_range = dict(oldest_at=min((e[2] for e in self.events.values()), default=None),
                                    newest_at=max((e[2] for e in self.events.values()), default=None),
                                    limit=self.max_events, byte_limit=self.max_event_bytes,
                                    retention_seconds=None)
        return self.event_range.copy()

    @staticmethod
    def trace_key(event):
        trace = event.get("trace_id")
        return (event.get("nf"), event.get("boot_id"), str(trace)) if trace not in (None, "", "0", 0) else None

    def forget_event(self, key):
        event, size, _ = self.events.pop(key)
        self.event_range = None
        self.event_bytes -= size
        trace = self.trace_key(event)
        group = self.anonymous.get(trace)
        if group:
            group.discard(key)
            if not group:
                del self.anonymous[trace]

    def evict_events(self, count, maximum_bytes):
        if len(self.events) <= count and self.event_bytes <= maximum_bytes:
            return
        # A delayed page from another NF may contain older timestamps than the
        # arrival order. Sort once per batch; never rescan the whole ring per row.
        for key in sorted(self.events, key=lambda k: self.events[k][2]):
            if len(self.events) <= count and self.event_bytes <= maximum_bytes:
                break
            self.forget_event(key)
            self.event_evictions += 1

    def changed(self):
        self.version += 1
        # Trend entries have their own bounded curve/time versions. A signal or
        # session-detail update must not discard the shared historical response.
        self.memo = OrderedDict((k, v) for k, v in self.memo.items() if k[0] == 'trend')
        self.memo_bytes = sum(len(v[0]) for v in self.memo.values())

    def source(self, name):
        return self.sources.setdefault(name, dict(data=[], updated_at=None, attempted_at=None,
                                                  errors=0, requests=0, error=None, interval=15))

    def replace(self, name, data, interval):
        self.admit(max(16 * MIB, self.view_bytes + container_bytes(data) * 4 + 8 * MIB), "保留新快照与派生视图")
        if name in ("amf_ues", "smf_pdus", "amf_gnbs"):
            if not isinstance(data, list) or len(data) > 4096:
                raise ValueError("invalid source rows")
            for row in data:
                if not isinstance(row, dict):
                    raise ValueError("invalid source record")
                if name in ("amf_ues", "smf_pdus"):
                    if row.get("supi") is not None and not isinstance(row["supi"], str):
                        raise ValueError("invalid SUPI")
                    pdus = row.get("pdu_sessions" if name == "amf_ues" else "pdu", [])
                    if not isinstance(pdus, list) or len(pdus) > 16:
                        raise ValueError("invalid PDU list")
                    for p in pdus:
                        if (not isinstance(p, dict) or (p.get("snssai") is not None
                                and not isinstance(p["snssai"], dict))):
                            raise ValueError("invalid PDU record")
                        if p.get("qos_flows") is not None and (not isinstance(p["qos_flows"], list)
                                or len(p["qos_flows"]) > 63 or not all(isinstance(q, dict) for q in p["qos_flows"])):
                            raise ValueError("invalid QoS flows")
                        if p.get("session_control") is not None and not isinstance(p["session_control"], dict):
                            raise ValueError("invalid session control snapshot")
        source = self.source(name)
        old = source.copy()
        source.update(data=data, updated_at=time.time(), error=None, errors=0, interval=interval)
        try:
            self.rebuild()
        except (ValueError, TypeError, KeyError, AttributeError, MemoryError) as exc:
            source.clear()
            source.update(old)
            if isinstance(exc, MemoryError):
                raise
            raise ValueError("invalid source shape; retained last complete snapshot")
        self.view_bytes = container_bytes([self.ues, self.gnbs, self.sessions]) * 2
        self.changed()

    def fail(self, name, error):
        source = self.source(name)
        source.update(error=str(error)[:200], errors=source["errors"] + 1)
        self.changed()

    def health(self):
        now = time.time()
        sources = {}
        for name, s in self.sources.items():
            age = now - s["updated_at"] if s["updated_at"] else None
            sources[name] = {k: v for k, v in s.items() if k != "data"}
            sources[name].update(age_seconds=age, stale=age is None or age > max(6, s["interval"] * 3))
        return dict(started_at=self.started_at, enabled=self.enabled, sources=sources,
                    memory=self.memory.status(), signals=self.signals.status() if self.signals else {},
                    trend_retention_seconds=self.trend_retention, event_coverage=self.event_coverage(),
                    events=len(self.events), event_bytes=self.event_bytes,
                    event_evictions=self.event_evictions, curves=len(self.curves),
                    curve_evictions=self.curve_evictions, curve_rejections=self.curve_rejections,
                    trend_bytes=self.trend_bytes(), trend_byte_limit=self.max_trend_bytes,
                    requests=self.request_count, affinity=self.affinity,
                    coverage="信令按容量保留；速率保留最近 30 分钟原始采样及 24 小时分钟汇总，最多 512 条曲线、64 MiB；缓存不持久化，内存压力下可能提前淘汰")

    def rebuild(self):
        ues = copy.deepcopy(self.source("amf_ues")["data"])
        smf = self.source("smf_pdus")["data"]
        by_supi = {}
        for ue in smf:
            by_supi.setdefault(ue.get("supi"), []).append(ue)
        for ue in ues:
            normalize_location(ue)
            if ue.get("gnb") and not gnb_identity(ue["gnb"]):
                ue["gnb"].update(gnb_id=None, gnb_id_bits=None, cell_id=None)
            candidates = by_supi.get(ue.get("supi"), [])
            pdu_list = candidates[0].get("pdu", []) if len(candidates) == 1 else []
            for p in ue.get("pdu_sessions", []):
                matches = [s for s in pdu_list if type(p.get("psi")) is int and p["psi"] > 0
                           and s.get("psi") == p["psi"]]
                match = matches[0] if len(matches) == 1 and compatible(p, matches[0]) else {}
                for k in ("ipv4", "ipv6", "pdu_state", "upf_seid", "qos_flows", "n3", "session_control"):
                    p[k] = copy.deepcopy(match.get(k))
                p["smf_info_status"] = "available" if match else "not-matched" if self.source("smf_pdus")["updated_at"] else "unavailable"
        ue_map = {}
        for u in ues:
            if u.get("supi"):
                ue_map[u["supi"]] = u if u["supi"] not in ue_map else {}
        sessions = copy.deepcopy(self.source("upf_rates")["data"])
        for s in sessions:
            ue = ue_map.get(s.get("supi"), {})
            s["cm_state"] = ue.get("cm_state", "unknown")
            loc = ue.get("location") or {}
            s["location"] = copy.deepcopy(loc)
            s["gnb_peer"] = ((ue.get("gnb") or {}).get("peer")
                             if s["cm_state"] == "connected" else None) or (loc.get("gnb") or {}).get("peer")
            candidates = by_supi.get(s.get("supi"), [])
            pdus = candidates[0].get("pdu", []) if len(candidates) == 1 else []
            def same_ip(p):
                try:
                    ip = ipaddress.ip_address(s.get("ue_ip", ""))
                    return any(ip == ipaddress.ip_address(p[k]) for k in ("ipv4", "ipv6") if p.get(k))
                except ValueError:
                    return False
            matches = [p for p in pdus if same_ip(p)]
            keyed = [p for p in matches if p.get("upf_seid") is not None]
            if keyed:
                matches = [p for p in keyed if str(p["upf_seid"]) == str(s["seid"])]
            p = matches[0] if len(matches) == 1 and compatible(s, matches[0]) else {}
            s.update(session_control=copy.deepcopy(p.get("session_control")),
                     qos_flows=copy.deepcopy(p.get("qos_flows", [])), psi=p.get("psi"), snssai=p.get("snssai"), n3=p.get("n3"),
                     active_in_amf=bool(p.get("psi") and any(
                         a.get("psi") == p["psi"] and compatible(a, p) for a in ue.get("pdu_sessions", []))))
            totals = {f"{d}_{k}": 0 for d in ("ul", "dl") for k in ("bps", "pps", "bytes", "packets")}
            bearers = {}
            for r in s.get("rules", []):
                # PFCP interface values: ACCESS=0, CORE=1, SGI_N6_LAN=2, CP=3.
                # Missing metadata is unknown, never infer an internal rule from QFI/rate=0.
                src, dst = r.get("src_if"), r.get("dst_if")
                r["rule_kind"], r["rule_purpose"] = "unknown", "unclassified"
                if src == 3 or dst == 3:
                    r["rule_kind"] = "internal"
                    r["rule_purpose"] = ("cp-to-ue" if src == 3 and dst == 0 else
                                         "ue-to-cp" if src == 0 and dst == 3 else "cp-forwarding")
                elif (src == 0 and dst in (0, 1, 2)) or (src in (1, 2) and dst == 0):
                    r["rule_kind"], r["rule_purpose"] = "business", "user-data"
                r["5qi"] = next((q.get("5qi") for q in p.get("qos_flows", [])
                                 if q.get("qfi") == r.get("qfi")), None)
                d = "ul" if r["direction"] == "UL" else "dl"
                for k in ("bps", "pps", "bytes", "packets"):
                    totals[f"{d}_{k}"] += int(r.get(k, 0))
                if r.get("qfi"):
                    b = bearers.setdefault(r["qfi"], dict(qfi=r["qfi"], **{"5qi": r["5qi"]},
                                                         **{key: 0 for key in totals}))
                    for k in ("bps", "pps", "bytes", "packets"):
                        b[f"{d}_{k}"] += int(r.get(k, 0))
            s.update({k: str(v) for k, v in totals.items()})
            s["bearers"] = [{k: str(v) if k not in ("qfi", "5qi") else v for k, v in b.items()}
                            for b in bearers.values()]
        rates = {}
        for s in sessions:
            rate = rates.setdefault(s.get("supi"), [0, 0])
            rate[0] += int(s["ul_bps"])
            rate[1] += int(s["dl_bps"])
        for ue in ues:
            ue["ul_bps"], ue["dl_bps"] = map(str, rates.get(ue.get("supi"), [0, 0]))
        gnbs = copy.deepcopy(self.source("amf_gnbs")["data"])
        for g in gnbs:
            g["associated_ues"], cells = [], {}
            g["ue_info_available"] = self.source("amf_ues")["updated_at"] is not None
            g["observed_cells_source"] = "ue-location-reports"
            peer = ((g.get("ng") or {}).get("sctp") or {}).get("peer")
            for u in ues:
                loc = u.get("location") or {}
                current = u.get("gnb") or {}
                if gnb_key(g) is None:
                    continue
                if (u.get("cm_state") == "connected" and gnb_key(g) == gnb_key(current)
                        and (not peer or not current.get("peer") or peer == current["peer"])
                        and current.get("status", "associated") == "associated"):
                    g["associated_ues"].append(dict(supi=u.get("supi"), cm_state=u.get("cm_state"), gnb=current,
                        pdu_sessions=u.get("pdu_sessions"), pdu_sessions_count=len(u.get("pdu_sessions", [])), location=loc))
                if gnb_key(g) != gnb_key(loc.get("gnb")):
                    continue
                cgi = loc.get("nr_cgi") or {}
                if cgi.get("nci") is not None and cgi.get("plmn") and loc.get("timestamp"):
                    key = (cgi["plmn"], cgi["nci"])
                    cell = cells.setdefault(key, dict(ue_count=0, connected_ue_count=0, idle_ue_count=0,
                                                      unknown_ue_count=0, timestamp=0))
                    cell["ue_count"] += 1
                    state = u.get("cm_state") if u.get("cm_state") in ("connected", "idle") else "unknown"
                    cell[state + "_ue_count"] += 1
                    if loc["timestamp"] >= cell["timestamp"]:
                        cell.update({k: copy.deepcopy(loc.get(k)) for k in ("gnb", "nr_cgi", "nr_tai", "timestamp", "status")})
            g["observed_cells"] = list(cells.values())
            if not g["ue_info_available"]:
                g["associated_ues"] = g["observed_cells"] = None
        self.ues, self.sessions, self.gnbs = ues, sessions, gnbs

    def trend_bytes(self):
        return sys.getsizeof(self.curves) + 8192 + sum(c.budget_bytes(k) for k, c in self.curves.items())

    def record_rates(self, stamp, boot_id=None, interval=2):
        self.admit(16 * MIB, "记录有界速率曲线")
        if not math.isfinite(stamp) or stamp <= 0:
            raise ValueError('invalid trend timestamp')
        if stamp == self.trend_stamp and boot_id == self.trend_boot:
            return
        restarted = self.trend_boot is not None and boot_id != self.trend_boot
        if stamp < self.trend_stamp:
            # Wall-clock discontinuity: never mix overlapping time axes.
            self.curves.clear()
            self.trend_epoch = uuid.uuid4().hex
        self.trend_stamp, self.trend_boot, self.trend_interval = stamp, boot_id, interval
        points = {"global": [0, 0]}
        identities = {}
        for s in self.sessions:
            ul, dl = float(s["ul_bps"]), float(s["dl_bps"])
            points[f"session:{s['seid']}"] = [ul, dl]
            identities[f"session:{s['seid']}"] = (
                (boot_id, s.get('supi'), s.get('dnn'), s.get('ue_ip')), s.get('uptime_seconds'))
            u = points.setdefault(f"user:{s.get('supi', '')}", [0, 0])
            u[0] += ul
            u[1] += dl
            points["global"][0] += ul
            points["global"][1] += dl
        for key in list(self.curves):
            if self.curves[key].last < stamp - self.trend_retention:
                del self.curves[key]
            elif key not in points:
                self.curves[key].active = False
        used = self.trend_bytes()
        for key, (ul, dl) in points.items():
            old = self.curves.get(key)
            identity, uptime = identities.get(key, (None, None))
            if old and key.startswith('session:') and (not old.active or old.identity != identity or
                    (isinstance(uptime, (int, float)) and isinstance(old.uptime, (int, float)) and uptime < old.uptime)):
                del self.curves[key]
                used = self.trend_bytes()
            if key not in self.curves:
                while len(self.curves) >= self.max_curves or used + Curve.allocation_bytes > self.max_trend_bytes:
                    victim = next((k for k, c in self.curves.items() if k != 'global' and k not in points), None)
                    if victim is None:
                        break
                    del self.curves[victim]
                    self.curve_evictions += 1
                    used = self.trend_bytes()
                if len(self.curves) >= self.max_curves or used + Curve.allocation_bytes > self.max_trend_bytes:
                    self.curve_rejections += 1
                    continue
                self.admit(Curve.allocation_bytes + MIB, "分配速率曲线")
                self.curves[key] = Curve()
                used = self.trend_bytes()
            curve = self.curves[key]
            curve.append(stamp, ul, dl, discontinuity=restarted or not curve.active, interval=interval)
            curve.active, curve.identity, curve.uptime = True, identity, uptime
            self.curves.move_to_end(key)
        self.changed()

    def add_events(self, nf, payload):
        self.admit(max(8 * MIB, container_bytes(payload) * 3), "新增事件批次")
        boot = payload["boot_id"]
        for event in payload["items"]:
            event = copy.deepcopy(event)
            event.update(nf=nf, boot_id=boot)
            ident = f"{nf}/{boot}/{event['seq']}"
            if ident in self.events and not event.get("supi"):
                event["supi"] = self.events[ident][0].get("supi", "")
            if event.get("name") == "IdentityResolved" and event.get("supi"):
                trace = self.trace_key(event)
                for key in self.anonymous.pop(trace, ()):
                    entry = self.events.get(key)
                    if entry is None:
                        continue
                    old = entry[0]
                    if not old.get("supi"):
                        self.event_bytes -= entry[1]
                        old["supi"] = event["supi"]
                        entry[1] = len(encode(old))
                        self.event_bytes += entry[1]
            size = len(encode(event))
            if ident in self.events:
                self.forget_event(ident)
            stamp = min(time.time(), int(event.get("timestamp_us", int(time.time() * 1e6))) / 1e6)
            self.events[ident] = [event, size, stamp]
            self.event_range = None
            self.event_bytes += size
            trace = self.trace_key(event)
            if trace and not event.get("supi"):
                self.anonymous.setdefault(trace, set()).add(ident)
        self.evict_events(self.max_events, self.max_event_bytes)
        self.changed()

    def response(self, endpoint, query):
        self.request_count += 1
        # Include a 2s clock bucket so source age/staleness advances during outages.
        key = (self.version, int(time.time() / 2), endpoint, tuple(sorted(query.items())))
        if endpoint == 'trends':
            minute = min(86400, max(60, int(query.get('seconds', 1800)))) > 1800
            curve = self.curves.get(query.get('key', 'global'))
            key = ('trend', curve.epoch if curve else self.trend_epoch,
                   (curve.minute_version if minute else curve.last) if curve else 0,
                   int(time.time() / (60 if minute else 2)), tuple(sorted(query.items())))
        if key in self.memo:
            self.memo.move_to_end(key)
            return self.memo[key]
        if endpoint not in ("health", "overview", "system"):
            self.admit(max(16 * MIB, self.view_bytes), "生成查询结果")
        result = self.query(endpoint, query)
        # Trend payloads contain only curve data and aligned bounds. Unrelated
        # signals must not change their ETag or force retransmission.
        if endpoint != 'trends':
            result.update(version=str(self.version), generated_at=time.time())
        body = encode(result)
        etag = '"' + hashlib.sha256(body).hexdigest()[:24] + '"'
        if len(body) <= 1024 * 1024:
            while self.memo and (len(self.memo) >= 128 or self.memo_bytes + len(body) > 8 * 1024 * 1024):
                self.memo_bytes -= len(self.memo.popitem(last=False)[1][0])
            self.memo[key] = (body, etag)
            self.memo_bytes += len(body)
        return body, etag

    def query_trends(self, query):
        key = query.get('key', 'global')
        window = min(86400, max(60, int(query.get('seconds', 1800))))
        minute = window > 1800
        now = time.time()
        end = math.floor(now / (60 if minute else 2)) * (60 if minute else 2)
        # Raw samples newer than the aligned clock bucket must remain visible.
        if not minute:
            end = max(end, min(now, self.trend_stamp))
        start = end - window
        curve = self.curves.get(key)
        epoch = curve.epoch if curve else self.trend_epoch
        points, buckets = [], []
        if curve:
            if minute:
                points, buckets = curve.minute_points(start, end)
            else:
                points = [p for p in curve.points(start) if p[0] <= end]
        valid = [p[0] for p in points if p[1] is not None]
        reset, replace_from = True, start
        cursor = query.get('cursor')
        if cursor:
            try:
                old_epoch, position = cursor.split('/')
                position = float(position)
                if not math.isfinite(position):
                    raise ValueError('invalid cursor')
            except (ValueError, AttributeError):
                raise ValueError('invalid trend cursor')
            if old_epoch == epoch and start <= position <= end:
                reset = False
                # Re-send the boundary: a just-completed minute can receive its
                # last valid interval on the next upload. The client replaces,
                # never adds, overlapping buckets.
                replace_from = max(start, position - (120 if minute else 0))
        position = end if minute or not valid else valid[-1]
        return dict(key=key, points=[p for p in points if p[0] >= replace_from],
                    buckets=[b for b in buckets if b[0] >= replace_from],
                    resolution_seconds=60 if minute else self.trend_interval,
                    aggregation='minute' if minute else 'raw',
                    available_from=valid[0] if valid else None,
                    available_to=(valid[-1] + 60 if minute else valid[-1]) if valid else None,
                    window_start=start, window_end=end, replace_from=replace_from,
                    cursor=epoch + '/' + str(position), epoch=epoch, reset=reset,
                    started_at=self.started_at, window_seconds=window)

    def query(self, endpoint, query):
        if endpoint == "health":
            return self.health()
        if endpoint == "overview":
            states = {s: sum(u.get("cm_state", "unknown") == s for u in self.ues)
                      for s in ("connected", "idle", "unknown")}
            events = self.query("events", {"limit": "6", "protocol": query.get("protocol", "NGAP")})
            return dict(ue_count=len(self.ues), cm_states=states, gnb_count=len(self.gnbs),
                        session_count=len(self.sessions),
                        ul_bps=str(sum(int(s["ul_bps"]) for s in self.sessions)),
                        dl_bps=str(sum(int(s["dl_bps"]) for s in self.sessions)),
                        events=events["items"], protocols=events["protocols"], health=self.health())
        if endpoint == "trends":
            return self.query_trends(query)
        if endpoint == "system":
            return dict(health=self.health(), pods=self.source("pods")["data"])
        rates = self.sessions
        if endpoint == "rates" and query.get("level") in ("bearer", "rule"):
            field = "bearers" if query["level"] == "bearer" else "rules"
            rates = [{**{k: v for k, v in s.items() if k not in ("bearers", "rules")}, **r}
                     for s in self.sessions for r in s.get(field, [])]
            if field == "rules":
                scope = query.get("rule_scope", "all")
                if scope not in ("business", "internal", "all"):
                    raise ValueError("invalid rule scope")
                if scope == "business":
                    rates = [r for r in rates if r["rule_kind"] != "internal"]
                elif scope == "internal":
                    rates = [r for r in rates if r["rule_kind"] == "internal"]
        elif endpoint == "rates" and query.get("level") == "user":
            users = {}
            for s in self.sessions:
                u = users.setdefault(s.get("supi"), dict(supi=s.get("supi"), cm_state=s.get("cm_state"),
                                                         dnns=set(), session_count=0,
                                                         **{k: 0 for k in ("ul_bps", "dl_bps", "ul_bytes", "dl_bytes")}))
                if s.get("dnn"):
                    u["dnns"].add(s["dnn"])
                u["session_count"] += 1
                for k in ("ul_bps", "dl_bps", "ul_bytes", "dl_bytes"):
                    u[k] += int(s[k])
            rates = []
            for u in users.values():
                u["dnns"] = sorted(u["dnns"])
                u["dnn"] = ", ".join(u["dnns"])
                rates.append({k: str(v) if k in ("ul_bps", "dl_bps", "ul_bytes", "dl_bytes") else v
                              for k, v in u.items()})
        rows = {"ues": self.ues, "gnbs": self.gnbs, "sessions": self.sessions, "rates": rates}.get(endpoint)
        if endpoint == "events":
            rows = sorted((v[0] for v in self.events.values()),
                          key=lambda e: (int(e.get("order_timestamp_us", e.get("timestamp_us", 0))), e.get("nf", ""),
                                         int(e.get("seq", 0))), reverse=True)
            protocols = sorted({r["protocol"] for r in rows if isinstance(r.get("protocol"), str) and r["protocol"]})
        if rows is None:
            raise KeyError(endpoint)
        for name in ("supi", "cm_state", "seid", "nf", "psi", "result", "protocol", "gnb_id", "gnb_id_bits", "plmn"):
            value = query.get(name)
            if value:
                rows = [r for r in rows if str(r.get(name, (r.get("gnb") or
                            (r.get("location") or {}).get("gnb") or {}).get(name, ""))) == value]
        if query.get("active_only") == "1":
            rows = [r for r in rows if r.get("active_in_amf")]
        if query.get("q"):
            term = query["q"].lower()
            rows = [r for r in rows if term in json.dumps(r, ensure_ascii=False).lower()]
        limit = min(100, max(1, int(query.get("limit", 50))))
        page = max(0, min(100000, int(query.get("page", 0))))
        result = dict(items=rows[page * limit:(page + 1) * limit], total=len(rows), page=page, limit=limit,
                      sources=self.health()["sources"])
        if endpoint == "events":
            result["coverage"] = self.event_coverage()
            result["protocols"] = protocols
            if self.signals:
                result["items"] = [dict(r, detail_available=r.get("event_id") in self.signals.raw or r.get("event_id") in self.signals.decoded) for r in result["items"]]
        return result
