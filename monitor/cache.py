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


def sctp_peer(gnb):
    if not isinstance(gnb, dict):
        return None
    peer = gnb.get("peer") if "peer" in gnb else ((gnb.get("ng") or {}).get("sctp") or {}).get("peer")
    return peer if isinstance(peer, str) and 0 < len(peer) <= 128 and peer.strip() and peer not in ("-", "unknown") else None


def gnb_connection_key(gnb):
    identity, peer = gnb_key(gnb), sctp_peer(gnb)
    return (*identity, peer) if identity and peer else None


def current_gnb(ue):
    gnb = ue.get("gnb") or {}
    return gnb if isinstance(gnb, dict) and ue.get("cm_state") == "connected" and gnb.get("status") == "associated" else None


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
    max_trend_bytes = 128 * MIB
    max_events = 20000
    max_event_bytes = 16 * 1024 * 1024
    max_curves = 1024
    max_bearer_curves = 128
    max_bearer_bytes = 16 * MIB
    max_rule_curves = 256
    max_rule_bytes = 32 * MIB

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
        self.bearer_rejections = 0
        self.rule_rejections = 0
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

    def relieve_memory(self, reserve=0):
        if self.memory.sample(force=True) + reserve < self.memory.high or time.monotonic() - self.last_reclaim < 5:
            return
        self.last_reclaim = time.monotonic()
        self.memo.clear()
        self.memo_bytes = 0
        self.memory.reclaims += 1
        # Latest state survives. Reclaim low-priority histories before signals
        # or the higher-priority user/session histories, including for reserves.
        for prefixes in (('rule:',), ('bearer:',)):
            for key in list(self.curves):
                if key.startswith(prefixes):
                    del self.curves[key]
                    self.curve_evictions += 1
            self.memory.trim_allocator()
            used = self.memory.sample(force=True)
            if used + reserve < self.memory.high and not self.memory.paused:
                self.changed()
                return
        if self.signals:
            self.signals.trim(pressure=True)
        self.evict_events(1000, self.max_event_bytes)
        self.memory.trim_allocator()
        if self.memory.sample(force=True) + reserve >= self.memory.high or self.memory.paused:
            for key in list(self.curves):
                if key != 'global':
                    del self.curves[key]
                    self.curve_evictions += 1
            self.memory.trim_allocator()
            self.memory.sample(force=True)
        self.changed()

    def admit(self, reserve=0, operation="collection"):
        try:
            self.memory.check(reserve, operation)
        except MemoryPressure:
            self.relieve_memory(reserve)
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
                    bearer_curves=sum(k.startswith('bearer:') for k in self.curves),
                    bearer_bytes=self.bearer_bytes(), bearer_curve_limit=self.max_bearer_curves,
                    bearer_byte_limit=self.max_bearer_bytes, bearer_rejections=self.bearer_rejections,
                    rule_curves=sum(k.startswith('rule:') for k in self.curves),
                    rule_bytes=self.rule_bytes(), rule_curve_limit=self.max_rule_curves,
                    rule_byte_limit=self.max_rule_bytes, rule_rejections=self.rule_rejections,
                    requests=self.request_count, affinity=self.affinity,
                    coverage="信令按容量保留；速率保留最近 30 分钟原始采样及 24 小时分钟汇总，最多 1024 条曲线、128 MiB；缓存不持久化，内存压力下可能提前淘汰")

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
            connection = gnb_connection_key(g)
            for u in ues:
                loc = u.get("location") or {}
                current = current_gnb(u)
                if connection is None:
                    continue
                if connection == gnb_connection_key(current):
                    g["associated_ues"].append(dict(supi=u.get("supi"), cm_state=u.get("cm_state"), gnb=current,
                        pdu_sessions=u.get("pdu_sessions"), pdu_sessions_count=len(u.get("pdu_sessions", [])), location=loc))
                if connection != gnb_connection_key(loc.get("gnb")):
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

    def bearer_bytes(self):
        # Include a conservative per-entry OrderedDict allowance in the sub-budget.
        return sum(c.budget_bytes(k) + 256 for k, c in self.curves.items() if k.startswith('bearer:'))

    @staticmethod
    def bearer_identity(session, qfi, boot):
        """Bounded rule fingerprint; never retain rules in historical curves.

        QoS values deliberately excluded: unchanged statistics slots continue
        across QoS edits. Missing generation metadata cannot establish identity.
        """
        if not boot or type(qfi) is not int or not 1 <= qfi <= 63:
            return None
        rows = []
        for r in session.get('rules', []):
            if r.get('qfi') != qfi:
                continue
            if any(type(r.get(k)) is not int or not 0 <= r[k] < limit for k, limit in
                   (('pdr_id', 1 << 16), ('qer_id', 1 << 32), ('generation', 1 << 32))):
                return None
            if r.get('direction') not in ('UL', 'DL'):
                return None
            rows.append((r['pdr_id'], r['qer_id'], r['generation'], r['direction']))
        if not rows:
            return None
        return hashlib.sha256(encode([boot, session.get('supi'), session.get('dnn'),
                                      session.get('ue_ip'), sorted(rows)])).digest()

    def rule_bytes(self):
        return sum(c.budget_bytes(k) + 256 for k, c in self.curves.items() if k.startswith('rule:'))

    @staticmethod
    def rule_identity(session, rule, boot):
        if not boot or rule.get('rule_kind') != 'business' or rule.get('direction') not in ('UL', 'DL'):
            return None
        if any(type(rule.get(k)) is not int or not 0 <= rule[k] < limit for k, limit in
               (('pdr_id', 1 << 16), ('qer_id', 1 << 32), ('generation', 1 << 32), ('qfi', 64))):
            return None
        if rule['pdr_id'] == 0:
            return None
        return hashlib.sha256(encode([boot, session.get('supi'), session.get('dnn'), session.get('ue_ip'),
            rule['pdr_id'], rule['qer_id'], rule['generation'], rule['qfi'], rule['direction']])).digest()

    @staticmethod
    def summary(session, item=None):
        fields = ('seid', 'supi', 'psi', 'dnn', 'ue_ip', 'ipv4', 'ipv6', 'cm_state',
                  'uptime_seconds', 'ul_bps', 'dl_bps', 'session_count')
        result = {k: session[k] for k in fields if k in session}
        if item:
            result.update({k: item[k] for k in ('qfi', '5qi', 'pdr_id', 'qer_id', 'direction',
                           'bps', 'ul_bps', 'dl_bps', 'rule_kind') if k in item})
        return result

    def locate_trend(self, key):
        kind, _, ident = key.partition(':')
        if kind == 'user':
            sessions = [s for s in self.sessions if s.get('supi') == ident]
            if not sessions:
                return None, None, None, None
            row = dict(supi=ident, session_count=len(sessions), cm_state=sessions[0].get('cm_state'),
                       dnn=', '.join(sorted({s.get('dnn', '') for s in sessions}))[:256],
                       ul_bps=str(sum(int(s['ul_bps']) for s in sessions)),
                       dl_bps=str(sum(int(s['dl_bps']) for s in sessions)))
            return row, None, None, None
        parts = ident.split(':')
        session = next((s for s in self.sessions if str(s['seid']) == parts[0]), None)
        if not session:
            return None, None, None, None
        uptime = session.get('uptime_seconds')
        if kind == 'session':
            return self.summary(session), (self.trend_boot, session.get('supi'), session.get('dnn'), session.get('ue_ip')), uptime, None
        if len(parts) != 2:
            return None, None, None, None
        field, name = ('bearers', 'qfi') if kind == 'bearer' else ('rules', 'pdr_id')
        item = next((r for r in session.get(field, []) if str(r.get(name)) == parts[1]), None)
        if not item:
            return None, None, None, None
        reason = None
        if kind == 'rule' and item.get('rule_kind') != 'business':
            reason = 'internal' if item.get('rule_kind') == 'internal' else 'unclassified'
        identity = (self.bearer_identity(session, item['qfi'], self.trend_boot) if kind == 'bearer'
                    else self.rule_identity(session, item, self.trend_boot))
        return self.summary(session, item), identity, uptime, reason

    def trend_metadata(self, key):
        row, identity, uptime, reason = self.locate_trend(key)
        curve = self.curves.get(key)
        valid = bool(row is not None and curve and curve.active and not reason and
                     (key.startswith('user:') or (identity is not None and curve.identity == identity)) and
                     not (isinstance(uptime, (int, float)) and isinstance(curve.uptime, (int, float)) and uptime < curve.uptime))
        return dict(trend_key=key, trend_epoch=curve.epoch if valid else None,
                    history_state='recording' if valid else 'released' if row is None and curve else 'unavailable',
                    history_reason=None if valid or row is None and curve else reason or
                    ('absent' if row is None else 'identity' if not key.startswith('user:') and identity is None else
                     'memory' if self.memory.paused else 'capacity'))

    def bearer_history(self, session, bearer):
        return self.trend_metadata(f"bearer:{session['seid']}:{bearer['qfi']}")

    def bearer_status(self, key):
        # Keep existing API/ETag for global, user and session histories compatible.
        return self.trend_metadata(key) if key.startswith(('bearer:', 'rule:')) else {}

    @staticmethod
    def validate_trend_key(key):
        if not isinstance(key, str) or len(key) > 128:
            raise ValueError('invalid trend key')
        if key.startswith(('bearer:', 'rule:')):
            kind, *parts = key.split(':')
            maximum = 63 if kind == 'bearer' else 65535
            if (len(parts) != 2 or not parts[0].isdigit() or len(parts[0]) > 20 or
                    not 0 <= int(parts[0]) < 1 << 64 or not parts[1].isdigit() or
                    not 1 <= int(parts[1]) <= maximum):
                raise ValueError('invalid rule/bearer trend key')

    def record_rates(self, stamp, boot_id=None, interval=2):
        self.admit(16 * MIB, "记录有界速率曲线")
        if not math.isfinite(stamp) or stamp <= 0:
            raise ValueError('invalid trend timestamp')
        if stamp == self.trend_stamp and boot_id == self.trend_boot:
            return
        restarted = self.trend_boot is not None and boot_id != self.trend_boot
        if stamp < self.trend_stamp:
            self.curves.clear()
            self.trend_epoch = uuid.uuid4().hex
        self.trend_stamp, self.trend_boot, self.trend_interval = stamp, boot_id, interval
        points, identities = {"global": [0, 0]}, {}
        present = set()
        for session in self.sessions:
            ul, dl = float(session['ul_bps']), float(session['dl_bps'])
            key = f"session:{session['seid']}"
            points[key] = [ul, dl]
            identities[key] = ((boot_id, session.get('supi'), session.get('dnn'), session.get('ue_ip')), session.get('uptime_seconds'))
            user = points.setdefault(f"user:{session.get('supi', '')}", [0, 0])
            user[0] += ul; user[1] += dl
            points['global'][0] += ul; points['global'][1] += dl
            for prefix, field, name in [('bearer', 'bearers', 'qfi'), ('rule', 'rules', 'pdr_id')]:
                for item in session.get(field, []):
                    key = f"{prefix}:{session['seid']}:{item.get(name)}"
                    if key in self.curves and (prefix != 'rule' or item.get('rule_kind') == 'business'):
                        present.add(key)
                    elif key in self.curves:
                        del self.curves[key]
        for key in list(self.curves):
            curve = self.curves[key]
            if curve.last < stamp - self.trend_retention:
                del self.curves[key]
            elif key not in points and key not in present:
                curve.active = False

        def oldest(prefix, inactive=True):
            candidates = (k for k, c in self.curves.items() if k.startswith(prefix) and (not inactive or not c.active))
            return min(candidates, key=lambda k: self.curves[k].last, default=None)

        def record(key, ul, dl, identity=None, uptime=None, tier=0):
            old = self.curves.get(key)
            if old and (tier or key.startswith('session:')) and (not old.active or old.identity != identity or
                    (isinstance(uptime, (int, float)) and isinstance(old.uptime, (int, float)) and uptime < old.uptime)):
                del self.curves[key]
            if key not in self.curves:
                prefix = 'rule:' if tier == 2 else 'bearer:'
                def sub_fits():
                    if not tier: return True
                    count = sum(k.startswith(prefix) for k in self.curves)
                    used = self.rule_bytes() if tier == 2 else self.bearer_bytes()
                    return count < (self.max_rule_curves if tier == 2 else self.max_bearer_curves) and used + Curve.allocation_bytes + 256 <= (self.max_rule_bytes if tier == 2 else self.max_bearer_bytes)
                def total_fits():
                    return len(self.curves) < self.max_curves and self.trend_bytes() + Curve.allocation_bytes + max(1024, sys.getsizeof(self.curves)) <= self.max_trend_bytes
                while not sub_fits() or not total_fits():
                    victim = None
                    if not sub_fits():
                        victim = oldest(prefix)
                    else:
                        candidates = [('rule:', tier < 2), ('bearer:', tier < 1)] if tier < 2 else [('rule:', False)]
                        for target, allow_active in candidates:
                            victim = oldest(target)
                            if victim is None and allow_active:
                                victim = oldest(target, inactive=False)
                            if victim is not None: break
                        if victim is None and tier == 0:
                            victim = min((k for k in self.curves if k != 'global' and k not in points), key=lambda k: self.curves[k].last, default=None)
                    if victim is None:
                        self.curve_rejections += 1
                        self.bearer_rejections += int(tier == 1)
                        self.rule_rejections += int(tier == 2)
                        return
                    del self.curves[victim]
                    self.curve_evictions += 1
                self.admit(Curve.allocation_bytes + MIB, "分配速率曲线")
                self.curves[key] = Curve()
            curve = self.curves[key]
            curve.append(stamp, ul, dl, discontinuity=restarted or not curve.active, interval=interval)
            curve.active, curve.identity, curve.uptime = True, identity, uptime

        for key, (ul, dl) in points.items():
            record(key, ul, dl, *identities.get(key, (None, None)))
        for tier in (1, 2):
            for session in self.sessions:
                field = 'bearers' if tier == 1 else 'rules'
                for item in session.get(field, []):
                    if tier == 2 and item.get('rule_kind') != 'business': continue
                    key = (f"bearer:{session['seid']}:{item['qfi']}" if tier == 1 else f"rule:{session['seid']}:{item.get('pdr_id')}")
                    identity = self.bearer_identity(session, item['qfi'], boot_id) if tier == 1 else self.rule_identity(session, item, boot_id)
                    if identity is None:
                        if key in self.curves: del self.curves[key]
                        continue
                    record(key, float(item['ul_bps']) if tier == 1 else float(item['bps']) if item['direction'] == 'UL' else 0,
                           float(item['dl_bps']) if tier == 1 else float(item['bps']) if item['direction'] == 'DL' else 0,
                           identity, session.get('uptime_seconds'), tier)
        self.changed()

    def trends_batch(self, query):
        self.admit(32 * MIB, '生成批量历史响应')
        entries = json.loads(query.get('curves', '[]'))
        if not isinstance(entries, list) or not 1 <= len(entries) <= 8:
            raise ValueError('select 1 to 8 curves')
        kinds, seen = set(), set()
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) - {'key', 'cursor', 'supi', 'status_only'}:
                raise ValueError('invalid batch entry')
            key = entry.get('key')
            self.validate_trend_key(key)
            kind, _, ident = key.partition(':')
            if kind not in ('user', 'session', 'bearer', 'rule') or not ident or key in seen:
                raise ValueError('invalid or duplicate batch key')
            if any(k in entry and (not isinstance(entry[k], str) or len(entry[k]) > 128) for k in ('supi', 'cursor')):
                raise ValueError('invalid batch identity/cursor')
            if 'status_only' in entry and type(entry['status_only']) is not bool:
                raise ValueError('invalid status flag')
            kinds.add(kind); seen.add(key)
        if len(kinds) != 1: raise ValueError('mixed trend levels')
        seconds = str(min(86400, max(60, int(query.get('seconds', 1800)))))
        self._batch_now = time.time()
        try:
            chunks, size = [], 16
            for entry in entries:
                key = entry['key']
                row, _, _, _ = self.locate_trend(key)
                metadata = self.trend_metadata(key)
                if row and entry.get('supi') and entry['supi'] != row.get('supi'):
                    doc = dict(key=key, points=[], reset=True, epoch='replaced', **metadata)
                    doc.update(history_state='unavailable', history_reason='instance', object=None)
                    body = encode(doc)
                elif entry.get('status_only'):
                    body = encode(dict(key=key, object=row, **metadata))
                else:
                    params = dict(key=key, seconds=seconds)
                    if entry.get('cursor'): params['cursor'] = entry['cursor']
                    body, _ = self.response('trends', params)
                    extra = dict(object=row)
                    if not key.startswith(('bearer:', 'rule:')):
                        extra.update(metadata)
                    body = body[:-1] + b',' + encode(extra)[1:]
                size += len(body) + 1
                if size > 4 * MIB: raise ValueError('batch response exceeds budget')
                chunks.append(body)
            body = b'{"items":[' + b','.join(chunks) + b']}'
            return body, '"' + hashlib.sha256(body).hexdigest()[:24] + '"'
        finally:
            self._batch_now = None

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
            history = self.bearer_status(query.get('key', 'global'))
            key = ('trend', tuple(sorted(history.items())), curve.epoch if curve else self.trend_epoch,
                   (curve.minute_version if minute else curve.last) if curve else 0,
                   curve.active if curve else None, self.memory.paused,
                   int((getattr(self, "_batch_now", None) or time.time()) / (60 if minute else 2)), tuple(sorted(query.items())))
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
        self.validate_trend_key(key)
        window = min(86400, max(60, int(query.get('seconds', 1800))))
        minute = window > 1800
        now = getattr(self, "_batch_now", None) or time.time()
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
        history = self.bearer_status(key)
        position = end if minute or not valid else valid[-1]
        return dict(key=key, points=[p for p in points if p[0] >= replace_from],
                    buckets=[b for b in buckets if b[0] >= replace_from],
                    resolution_seconds=60 if minute else self.trend_interval,
                    aggregation='minute' if minute else 'raw',
                    available_from=valid[0] if valid else None,
                    available_to=(valid[-1] + 60 if minute else valid[-1]) if valid else None,
                    window_start=start, window_end=end, replace_from=replace_from,
                    cursor=epoch + '/' + str(position), epoch=epoch, reset=reset,
                    started_at=self.started_at, window_seconds=window, **history)

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
            rates = [{**{k: v for k, v in s.items() if k not in ("bearers", "rules", "qos_flows")}, **r,
                      **(self.bearer_history(s, r) if field == "bearers" else self.trend_metadata(f"rule:{s['seid']}:{r.get('pdr_id')}"))}
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
        if query.get("sctp_peer") and endpoint in ("ues", "gnbs"):
            rows = [r for r in rows if sctp_peer(current_gnb(r) if endpoint == "ues" else r) == query["sctp_peer"]]
        if query.get("active_only") == "1":
            rows = [r for r in rows if r.get("active_in_amf")]
        if query.get("q"):
            term = query["q"].lower()
            rows = [r for r in rows if term in json.dumps(r, ensure_ascii=False).lower()]
        limit = min(100, max(1, int(query.get("limit", 50))))
        page = max(0, min(100000, int(query.get("page", 0))))
        result = dict(items=rows[page * limit:(page + 1) * limit], total=len(rows), page=page, limit=limit,
                      sources=self.health()["sources"])
        if endpoint == "sessions":
            result['items'] = [dict(s, bearers=[dict(b, **self.bearer_history(s, b))
                              for b in s.get('bearers', [])]) for s in result['items']]
        if endpoint == 'rates' and query.get('level', 'session') in ('user', 'session'):
            kind = query.get('level', 'session')
            result['items'] = [dict(row, **self.trend_metadata(f"{kind}:{row['supi'] if kind == 'user' else row['seid']}")) for row in result['items']]
        if endpoint == 'rates' and query.get('purpose') == 'selection':
            result['items'] = [{k: v for k, v in row.items() if k in ('supi', 'seid', 'psi', 'dnn', 'ue_ip',
                'qfi', '5qi', 'pdr_id', 'qer_id', 'direction', 'rule_kind', 'trend_key', 'trend_epoch',
                'history_state', 'history_reason')} for row in result['items']]
        if endpoint == "events":
            result["coverage"] = self.event_coverage()
            result["protocols"] = protocols
            if self.signals:
                result["items"] = [dict(r, detail_available=r.get("event_id") in self.signals.raw or r.get("event_id") in self.signals.decoded) for r in result["items"]]
        return result
