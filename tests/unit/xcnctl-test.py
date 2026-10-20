#!/usr/bin/env python3
"""Run the real CLI against isolated HTTP and UPF control socket fixtures."""
import copy
import http.server
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse

BINARY = os.path.abspath(sys.argv.pop(1))
SUPI = "imsi-001010000000001"


def ue(bits=28, ident=100, state="connected"):
    gnb = {"gnb_id": ident, "gnb_id_bits": bits, "plmn": "00101",
           "peer": "[2001:db8::1]:38412"}
    return {"supi": SUPI, "cm_state": state, "suci": "suci-test", "pei": "imei-test", "guti": "guti-test",
            "gnb": dict(gnb, amf_ue_ngap_id=9, ran_ue_ngap_id=7, status="associated", ostream_id=1),
            "security": {"valid": 1, "enc": "nea0", "int": "nia2"},
            "ambr": {"uplink": 1000000000, "downlink": 1000000000},
            "requested_slices": [], "allowed_slices": [{"sst": 1, "sd": "010101"}],
            "location": {"timestamp": 1790000000000000, "gnb": copy.deepcopy(gnb),
                         "nr_cgi": {"plmn": "00101", "nci": (ident << (36 - bits)) | 3},
                         "nr_tai": {"plmn": "00101", "tac": 1}},
            "pdu_sessions": [{"psi": 2, "dnn": "internet",
                              "snssai": {"sst": 1, "sd": "010101"}}]}


def pdu():
    return {"supi": SUPI, "pdu": [{"psi": 2, "upf_seid": "811",
            "ipv4": "10.45.0.3", "ipv6": "2001:db8:45::3", "dnn": "internet",
            "snssai": {"sst": 1, "sd": "010101"}, "pdu_state": "active",
            "qos_flows": [{"qfi": 1, "5qi": 9}],
            "n3": {"upf": {"addr": "[2001:db8:45::1]:2152", "teid": 0x01020304, "pdr_id": 2},
                   "gnb": {"addr": "[192.0.2.1]:2152", "teid": 0x05060708}}}]}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        page = int(urllib.parse.parse_qs(parsed.query).get("page", [0])[0])
        self.server.requests.append((parsed.path, page))
        data = self.server.data.get(parsed.path)
        status = 200
        if isinstance(data, tuple):
            status, body = data
        elif callable(data):
            body = json.dumps(data(page)).encode()
        elif isinstance(data, list):
            start = page * self.server.page_size
            items = data[start:start + self.server.page_size]
            pager = {"page": page, "page_size": self.server.page_size, "count": len(items)}
            if start + len(items) < len(data):
                pager["next"] = f"{parsed.path}?page={page + 1}"
            body = json.dumps({"items": items, "pager": pager}).encode()
        else:
            status, body = 503, b"unavailable"
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


RATE_TABLES = {
    "user": "SUPI UPTIME UL-Mbps DL-Mbps UL-pps DL-pps UL-bytes DL-bytes",
    "session": "SUPI UE-IP SEID DNN WORKER UPTIME UL-Mbps DL-Mbps UL-pps DL-pps UL-bytes DL-bytes",
    "bearer": "SUPI UE-IP SEID QFI UPTIME UL-Mbps DL-Mbps UL-pps DL-pps UL-bytes DL-bytes UL-packets DL-packets",
    "rule": "SUPI UE-IP SEID QFI PDR QER DIR UPTIME Mbps pps bytes packets",
}


class InspectionTest(unittest.TestCase):
    def setUp(self):
        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.http.data = {"/ue-info": [ue()], "/pdu-info": [pdu()],
                          "/gnb-info": [{"gnb_id": 100, "gnb_id_bits": 28,
                            "plmn": "00101", "ng": {"setup_success": True,
                            "sctp": {"peer": "[2001:db8::1]:38412"}},
                            "num_connected_ues": 1, "supported_ta_list": []}]}
        self.http.page_size = 100
        self.http.requests = []
        self.http_thread = threading.Thread(target=self.http.serve_forever,
                                            kwargs={"poll_interval": 0.02})
        self.http_thread.start()
        self.base = f"http://127.0.0.1:{self.http.server_port}"
        self.directory = tempfile.TemporaryDirectory()
        self.path = self.directory.name + "/upf.sock"
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.bind(self.path)
        self.sock.listen()
        self.sock.settimeout(0.05)
        self.stopping = threading.Event()
        self.control_thread = threading.Thread(target=self.control)
        self.control_thread.start()

    def tearDown(self):
        self.stopping.set()
        self.control_thread.join()
        self.sock.close()
        self.directory.cleanup()
        self.http.shutdown()
        self.http.server_close()
        self.http_thread.join()

    def control(self):
        while not self.stopping.is_set():
            try:
                connection, _ = self.sock.accept()
            except socket.timeout:
                continue
            with connection:
                request = b""
                while True:
                    data = connection.recv(512)
                    if not data:
                        break
                    request += data
                options = dict(token.split("=", 1) for token in request.decode().split())
                level = options.get("level", "user")
                if options.get("json") == "1":
                    row = {"supi": SUPI, "ue_ip": "10.45.0.3", "seid": 811,
                           "qfi": 1, "ul_bps": 1234, "uptime_seconds": 60}
                    response = json.dumps({"level": level, "rows": [row]}) + "\n"
                else:
                    values = {"SUPI": SUPI, "UE-IP": "10.45.0.3", "SEID": "811",
                              "DNN": "internet", "QFI": "1", "DIR": "UL"}
                    header = RATE_TABLES[level]
                    response = header + "\n" + " ".join(values.get(c, "1") for c in header.split()) + "\n"
                connection.sendall(response.encode())

    def argv(self, kind="ue", *options):
        args = [BINARY, "show", kind, "--amf-ue-info", self.base + "/ue-info",
                "--amf-gnb-info", self.base + "/gnb-info",
                "--smf-pdu-info", self.base + "/pdu-info"]
        if kind == "rate":
            args += ["--socket", self.path]
        return args + list(options)

    def run_cli(self, kind="ue", *options, expected=0):
        result = subprocess.run(self.argv(kind, *options), text=True,
                                capture_output=True, timeout=8,
                                env=dict(os.environ, NO_PROXY="*", no_proxy="*"))
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def item(self):
        return json.loads(self.run_cli("ue", "--json").stdout)["items"][0]

    def assert_detail_fields(self, output, fields):
        for label, value in fields.items():
            self.assertRegex(output, r"(?m)^\s*" + re.escape(label) +
                             r"\s*:\s*" + re.escape(value) + r"\s*$")

    def test_ue_without_upf_session(self):
        self.http.data["/ue-info"][0]["pdu_sessions"] = []
        result = self.run_cli()
        self.assertIn(SUPI, result.stdout)
        header, row = result.stdout.splitlines()
        columns = dict(zip(header.split(), row.split()))
        self.assertEqual(columns["GNB-ID"], "100")
        self.assertEqual(columns["GNB-LEN"], "28")
        self.assertEqual(columns["CELL-ID"], "3")
        self.assertNotIn(("/pdu-info", 0), self.http.requests)

    def test_detail_and_session_join(self):
        result = self.run_cli("ue", "--supi", SUPI, "--detail")
        self.assert_detail_fields(result.stdout, {
            "gNB ID (reported)": "100", "gNB ID (from NCI)": "100",
            "gNB ID length (bits)": "28", "NCI": "25603 (0x000006403)",
            "SUCI": "suci-test", "PEI": "imei-test", "GUTI": "guti-test",
            "AMF-UE-NGAP-ID": "9", "RAN-UE-NGAP-ID": "7", "SCTP stream ID": "1",
            "Security context valid": "yes", "NAS encryption": "nea0", "NAS integrity": "nia2",
            "UE-AMBR UL": "1000.000 Mbps (1000000000 bps)", "UPF-SEID": "811", "SMF data": "available"})
        for text in ("00101-0x000006403", "10.45.0.3",
                     "2001:db8:45::3", "internet",
                     "2001:db8::1", "active"):
            self.assertIn(text, result.stdout)
        self.assertRegex(result.stdout, r"Location updated\s*:")
        self.assertRegex(result.stdout, r"QFI\s+1\s+5QI\s+9")
        self.assertIn("16909060 (0x01020304)", result.stdout)
        self.assertIn("84281096 (0x05060708)", result.stdout)
        session = self.item()["pdu_sessions"][0]
        self.assertEqual(session["psi"], 2)
        self.assertEqual(session["qos_flows"], pdu()["pdu"][0]["qos_flows"])
        self.assertEqual(session["n3"], pdu()["pdu"][0]["n3"])

    def test_large_upf_seid_preserved(self):
        seid = str((1 << 64) - 1)
        self.http.data["/pdu-info"][0]["pdu"][0]["upf_seid"] = seid
        self.assertEqual(self.item()["pdu_sessions"][0]["upf_seid"], seid)
        self.assert_detail_fields(self.run_cli("ue", "--detail").stdout, {"UPF-SEID": seid})

    def test_widths_and_maximum_nci(self):
        for width in (22, 23, 24, 28, 32):
            with self.subTest(width=width):
                ident = 0xffffffff if width == 32 else 100
                value = ue(width, ident)
                value["location"]["nr_cgi"]["nci"] = (ident << (36 - width)) | ((1 << (36 - width)) - 1)
                self.http.data["/ue-info"] = [value]
                cgi = self.item()["location"]["nr_cgi"]
                self.assertTrue(cgi["identity_consistent"])
                self.assertEqual(cgi["cell_id"], (1 << (36 - width)) - 1)
                self.assertEqual(cgi["gnb_id"], ident)
                header, row = self.run_cli().stdout.splitlines()
                columns = dict(zip(header.split(), row.split()))
                self.assertEqual(columns["GNB-LEN"], str(width))
                self.assertEqual(columns["CELL-ID"], str(cgi["cell_id"]))
                value["location"]["nr_cgi"]["nci"] ^= 1 << (36 - width)
                cgi = self.item()["location"]["nr_cgi"]
                self.assertFalse(cgi["identity_consistent"])
                self.assertEqual(cgi["gnb_id"], ident ^ 1)
                self.assertEqual(cgi["cell_id"], (1 << (36 - width)) - 1)

    def test_zero_identity_is_valid(self):
        self.http.data["/ue-info"] = [ue(22, 0)]
        self.assertEqual(self.item()["gnb"]["gnb_id"], 0)
        result = self.run_cli("ue", "--gnb-id", "0", "--plmn", "00101")
        self.assertIn(SUPI, result.stdout)

    def test_identity_mismatch(self):
        self.http.data["/ue-info"] = [ue(28, 3584)]
        self.http.data["/ue-info"][0]["location"]["nr_cgi"]["nci"] = 0xe014e
        item = self.item()
        self.assertEqual(item["gnb"]["gnb_id"], 3584)
        self.assertEqual(item["location"]["gnb"]["gnb_id"], 3584)
        self.assertEqual(item["location"]["nr_cgi"]["nci"], 0xe014e)
        self.assertEqual(item["location"]["nr_cgi"]["gnb_id"], 3585)
        self.assertFalse(item["location"]["nr_cgi"]["identity_consistent"])
        self.assertEqual(item["location"]["nr_cgi"]["cell_id"], 78)
        header, row = self.run_cli().stdout.splitlines()
        columns = dict(zip(header.split(), row.split()))
        self.assertEqual(columns["GNB-ID"], "3584")
        self.assertEqual(columns["GNB-LEN"], "28")
        self.assertEqual(columns["CELL-ID"], "78")
        self.assertEqual(columns["LOCATION"], "identity-mismatch")
        for level in RATE_TABLES:
            row = self.run_cli("rate", "--level", level, "--with-location").stdout.splitlines()[-1]
            self.assertEqual([c.strip() for c in row.split("|")][-5:],
                             ["3584", "28", "78", "[2001:db8::1]:38412", "identity-mismatch"])
        detail = self.run_cli("ue", "--detail").stdout
        self.assert_detail_fields(detail, {"gNB ID (reported)": "3584", "gNB ID (from NCI)": "3585",
                                 "NCI": "917838 (0x0000e014e)", "Cell local ID": "78",
                                 "Identity": "identity-mismatch"})

    def test_cell_id_only_needs_valid_nci_and_length(self):
        for missing in ("gnb_id", "snapshot"):
            with self.subTest(missing=missing):
                value = ue()
                if missing == "gnb_id":
                    value["location"]["gnb"].pop("gnb_id")
                else:
                    value["location"].pop("gnb")
                    value["location"]["nr_cgi"]["gnb_id_bits"] = 28
                self.http.data["/ue-info"] = [value]
                cgi = self.item()["location"]["nr_cgi"]
                self.assertEqual(cgi["cell_id"], 3)
                self.assertEqual(cgi["gnb_id"], 100)
                self.assertIsNone(cgi["identity_consistent"])

    def test_invalid_nci_or_length_hides_derived_values(self):
        for key, invalid in (("nci", (None, -1, 1.5, 1 << 36, "25603")),
                             ("gnb_id_bits", (None, 0, 21, 33, -1, 28.5, "28"))):
            for value in invalid:
                with self.subTest(key=key, value=value):
                    data = ue()
                    target = data["location"]["nr_cgi" if key == "nci" else "gnb"]
                    target[key] = value
                    self.http.data["/ue-info"] = [data]
                    cgi = self.item()["location"]["nr_cgi"]
                    self.assertIsNone(cgi["cell_id"])
                    self.assertIsNone(cgi["gnb_id"])
                    self.assertIsNone(cgi["identity_consistent"])
                    header, row = self.run_cli().stdout.splitlines()
                    self.assertEqual(dict(zip(header.split(), row.split()))["CELL-ID"], "-")

    def test_last_known_after_gnb_removal(self):
        item = self.http.data["/ue-info"][0]
        item["cm_state"] = "idle"
        item["gnb"] = {"status": "not-connected"}
        self.assertEqual(self.item()["location"]["status"], "last-known")
        self.assertIn("last-known", self.run_cli("ue", "--gnb-id", "100").stdout)

    def test_missing_and_unreported_location(self):
        for timestamp in (None, 0):
            item = self.http.data["/ue-info"][0]
            item["location"]["timestamp"] = timestamp
            result = self.item()
            self.assertEqual(result["location"]["status"], "unknown")
            self.assertIsNone(result["location"]["nr_cgi"])

    def test_legacy_amf_never_trusts_fixed_width_id(self):
        item = self.http.data["/ue-info"][0]
        item["gnb"] = {"gnb_id": 56, "cell_id": 334}
        item["location"].pop("gnb")
        item["location"]["nr_cgi"].update(gnb_id=56, cell_id=334)
        result = self.item()
        self.assertIsNone(result["gnb"]["gnb_id"])
        self.assertIsNone(result["location"]["nr_cgi"]["cell_id"])
        self.assertEqual(result["location"]["nr_cgi"]["nci"], 25603)

    def test_filters_and_pagination(self):
        second = ue()
        second["supi"] += "2"
        self.http.data["/ue-info"].append(second)
        self.http.page_size = 1
        result = self.run_cli("ue", "--json", "--supi", second["supi"], "--gnb-id", "0x64")
        self.assertEqual(len(json.loads(result.stdout)["items"]), 1)
        self.assertIn(("/ue-info", 1), self.http.requests)
        self.assertEqual(json.loads(self.run_cli("ue", "--json", "--supi", "absent").stdout)["items"], [])

    def test_gnb_detail_and_plmn_filter(self):
        second = copy.deepcopy(self.http.data["/gnb-info"][0])
        second["plmn"] = "00102"
        self.http.data["/gnb-info"].append(second)
        data = json.loads(self.run_cli("gnb", "--json", "--gnb-id", "100").stdout)
        self.assertEqual(len(data["items"]), 2)
        by_plmn = {g["plmn"]: g for g in data["items"]}
        self.assertEqual(len(by_plmn["00101"]["associated_ues"]), 1)
        self.assertEqual(by_plmn["00102"]["associated_ues"], [])
        data = json.loads(self.run_cli("gnb", "--json", "--plmn", "00102").stdout)
        self.assertEqual(len(data["items"]), 1)
        self.assertIn("Supported TA/PLMN/slices", self.run_cli("gnb", "--detail").stdout)
        self.assertIn("ready", self.run_cli("gnb").stdout)

    def test_gnb_observed_cells_aggregate_connected_and_idle(self):
        idle = ue(state="idle")
        idle["supi"] += "2"
        idle["gnb"] = {"status": "not-connected"}
        idle["location"]["timestamp"] -= 1000000
        self.http.data["/ue-info"].append(idle)
        gnb = json.loads(self.run_cli("gnb", "--json").stdout)["items"][0]
        self.assertTrue(gnb["ue_info_available"])
        self.assertEqual(gnb["observed_cells_source"], "ue-location-reports")
        self.assertEqual(len(gnb["associated_ues"]), 1)
        self.assertEqual(gnb["associated_ues"][0]["gnb"]["amf_ue_ngap_id"], 9)
        self.assertEqual(len(gnb["observed_cells"]), 1)
        cell = gnb["observed_cells"][0]
        self.assertEqual((cell["ue_count"], cell["connected_ue_count"], cell["idle_ue_count"]), (2, 1, 1))
        self.assertEqual(cell["timestamp"], self.http.data["/ue-info"][0]["location"]["timestamp"])
        self.assertEqual(cell["nr_cgi"]["nci"], 25603)
        detail = self.run_cli("gnb", "--detail").stdout
        self.assertIn("Associated UEs", detail)
        self.assertIn("Observed cells (UE location reports", detail)
        self.assertIn("UE reports: total 2, connected 1, idle 1", detail)

    def test_gnb_mismatch_stays_with_reported_node(self):
        self.http.data["/ue-info"][0]["location"]["nr_cgi"]["nci"] = (101 << 8) | 78
        second = copy.deepcopy(self.http.data["/gnb-info"][0])
        second["gnb_id"] = 101
        self.http.data["/gnb-info"].append(second)
        nodes = {g["gnb_id"]: g for g in json.loads(self.run_cli("gnb", "--json").stdout)["items"]}
        self.assertEqual(len(nodes[100]["associated_ues"]), 1)
        cell = nodes[100]["observed_cells"][0]
        self.assertEqual(cell["nr_cgi"]["gnb_id"], 101)
        self.assertEqual(cell["nr_cgi"]["cell_id"], 78)
        self.assertFalse(cell["nr_cgi"]["identity_consistent"])
        self.assertEqual(nodes[101]["associated_ues"], [])
        self.assertEqual(nodes[101]["observed_cells"], [])

    def test_gnb_optional_ue_failure_preserves_base(self):
        for data in ((503, b"unavailable"), [{"pdu_sessions": {}}]):
            self.http.data["/ue-info"] = data
            result = self.run_cli("gnb", "--json")
            self.assertIn("warning", result.stderr)
            gnb = json.loads(result.stdout)["items"][0]
            self.assertFalse(gnb["ue_info_available"])
            self.assertIsNone(gnb["associated_ues"])
            self.assertIsNone(gnb["observed_cells"])
            self.assertEqual(gnb["gnb_id"], 100)
        self.http.requests.clear()
        self.run_cli("gnb")
        self.assertNotIn(("/ue-info", 0), self.http.requests)

    def test_gnb_current_association_uses_sctp_peer(self):
        second = copy.deepcopy(self.http.data["/gnb-info"][0])
        second["ng"]["sctp"]["peer"] = "[2001:db8::1]:38413"
        self.http.data["/gnb-info"].append(second)
        nodes = {g["ng"]["sctp"]["peer"]: g for g in
                 json.loads(self.run_cli("gnb", "--json").stdout)["items"]}
        self.assertEqual(len(nodes["[2001:db8::1]:38412"]["associated_ues"]), 1)
        self.assertEqual(nodes["[2001:db8::1]:38413"]["associated_ues"], [])

    def test_gnb_no_valid_cell_is_known_empty(self):
        self.http.data["/ue-info"][0]["location"]["timestamp"] = 0
        gnb = json.loads(self.run_cli("gnb", "--json").stdout)["items"][0]
        self.assertTrue(gnb["ue_info_available"])
        self.assertEqual(len(gnb["associated_ues"]), 1)
        self.assertEqual(gnb["observed_cells"], [])

    def test_smf_unavailable_is_optional(self):
        self.http.data["/pdu-info"] = (503, b"unavailable")
        result = self.run_cli("ue", "--json")
        self.assertIn("warning", result.stderr)
        self.assertIsNone(json.loads(result.stdout)["items"][0]["pdu_sessions"][0]["ipv4"])
        session = json.loads(result.stdout)["items"][0]["pdu_sessions"][0]
        self.assertIsNone(session["qos_flows"])
        self.assertIsNone(session["n3"])
        self.assertEqual(session["smf_info_status"], "unavailable")

    def test_ambiguous_session_or_supi_not_merged(self):
        self.http.data["/pdu-info"][0]["pdu"].append(copy.deepcopy(pdu()["pdu"][0]))
        self.assertIsNone(self.item()["pdu_sessions"][0]["ipv4"])
        self.http.data["/pdu-info"] = [pdu(), pdu()]
        self.assertIsNone(self.item()["pdu_sessions"][0]["ipv4"])
        malformed = pdu()
        malformed["pdu"] = {"session": malformed["pdu"][0]}
        self.http.data["/pdu-info"] = [malformed]
        self.assertIsNone(self.item()["pdu_sessions"][0]["n3"])
        self.http.data["/ue-info"][0].pop("supi")
        missing = pdu()
        missing.pop("supi")
        self.http.data["/pdu-info"] = [missing]
        self.assertIsNone(self.item()["pdu_sessions"][0]["n3"])

    def test_amf_unavailable_is_failure(self):
        self.http.data["/ue-info"] = (503, b"unavailable")
        self.assertIn("cannot load", self.run_cli(expected=1).stderr)

    def test_conflicting_session_not_merged(self):
        self.http.data["/pdu-info"][0]["pdu"][0]["dnn"] = "other"
        session = self.item()["pdu_sessions"][0]
        self.assertIsNone(session["ipv4"])
        self.assertIsNone(session["n3"])
        self.assertIsNone(session["qos_flows"])
        self.assertIsNone(session["upf_seid"])
        self.assertEqual(session["smf_info_status"], "not-matched")

    def test_invalid_or_partial_amf_pages_fail(self):
        invalid = [{}, {"items": {}, "pager": {}},
                   {"items": [], "pager": {"page": 0, "count": 1}},
                   {"items": [], "pager": {"page": 0, "count": 0, "truncated": True}},
                   {"items": [], "pager": {"page": 0, "count": 0, "next": 2}},
                   {"items": [None], "pager": {"page": 0, "count": 1}}]
        for data in invalid:
            with self.subTest(data=data):
                self.http.data["/ue-info"] = (200, json.dumps(data).encode())
                self.run_cli(expected=1)
        self.http.data["/ue-info"] = (200, b'{"items": []}garbage')
        self.run_cli(expected=1)
        for body in (json.dumps({"items": [], "pager": {"page": 0, "count": 0}}).encode() + b'\0garbage',
                     json.dumps({"items": [{"pdu_sessions": {}}],
                                 "pager": {"page": 0, "count": 1}}).encode(),
                     json.dumps({"items": [{"location": []}],
                                 "pager": {"page": 0, "count": 1}}).encode()):
            self.http.data["/ue-info"] = (200, body)
            self.run_cli(expected=1)

    def test_repeated_page_not_silently_accepted(self):
        self.http.data["/ue-info"] = lambda page: {"items": [ue()],
            "pager": {"page": 0, "count": 1, "next": "/ue-info?page=1"}}
        self.run_cli(expected=1)

    def test_nonfinite_numbers_are_unknown(self):
        data = {"items": [ue()], "pager": {"page": 0, "count": 1}}
        body = json.dumps(data).replace('"nci": 25603', '"nci": 1e999').encode()
        self.http.data["/ue-info"] = (200, body)
        self.assertIsNone(self.item()["location"]["nr_cgi"]["nci"])

    def test_bad_options(self):
        for option in (("--interval", "nan"), ("--interval", "inf"),
                       ("--gnb-id", "4294967296"), ("--gnb-id", "-1"),
                       ("--plmn", "abcde"), ("--with-location",), ("--level", "user")):
            with self.subTest(option=option):
                self.run_cli("ue", *option, expected=2)
        self.run_cli("gnb", "--supi", SUPI, expected=2)
        self.run_cli("rate", "--detail", expected=2)

    def test_rate_defaults_and_location_at_all_levels(self):
        for level in RATE_TABLES:
            with self.subTest(level=level):
                result = self.run_cli("rate", "--level", level, "--json")
                row = json.loads(result.stdout)["rows"][0]
                self.assertNotIn("location", row)
                self.assertEqual(row["cm_state"], "connected")
                result = self.run_cli("rate", "--level", level, "--with-location", "--json")
                row = json.loads(result.stdout)["rows"][0]
                self.assertEqual(row["ul_bps"], 1234)
                self.assertEqual(row["location"]["gnb"]["gnb_id"], 100)
                self.assertEqual(row["location"]["gnb"]["gnb_id_bits"], 28)
                self.assertEqual(row["location"]["gnb"]["peer"], "[2001:db8::1]:38412")
                self.assertEqual(row["location"]["nr_cgi"]["nci"], 25603)
                self.assertEqual(row["location"]["nr_cgi"]["cell_id"], 3)
                if level != "user":
                    self.assertEqual(row["psi"], 2)
                result = self.run_cli("rate", "--level", level, "--with-location")
                header, separator, row = result.stdout.strip().splitlines()
                self.assertIn("-+-", separator)
                self.assertEqual(len(header.split("|")), len(row.split("|")))
                self.assertEqual([c.strip() for c in header.split("|")][-5:],
                                 ["GNB-ID", "GNB-LEN", "CELL-ID", "GNB-PEER", "LOCATION"])
                self.assertEqual([c.strip() for c in row.split("|")][-5:],
                                 ["100", "28", "3", "[2001:db8::1]:38412", "reported"])
                widths = lambda line: [len(c) for c in line.split("|")]
                self.assertEqual(widths(header), widths(row))
                self.assertEqual(widths(header), [len(c) for c in separator.split("+")])

    def test_rate_optional_location_failure_and_active_only(self):
        self.http.data["/ue-info"] = (503, b"unavailable")
        result = self.run_cli("rate", "--with-location", "--json")
        self.assertIn("warning", result.stderr)
        self.assertEqual(json.loads(result.stdout)["rows"][0]["location"]["status"], "unknown")
        row = self.run_cli("rate", "--with-location").stdout.splitlines()[-1]
        self.assertEqual([c.strip() for c in row.split("|")][-5:],
                         ["-", "-", "-", "-", "unknown"])
        self.run_cli("rate", "--level", "session", "--active-only", expected=1)

    def test_watch_emits_multiple_json_documents(self):
        process = subprocess.Popen(self.argv("ue", "--json", "--watch", "--interval", "0.1"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 3
            while len([p for p in self.http.requests if p[0] == "/ue-info"]) < 2:
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
        finally:
            process.terminate()
            stdout, _ = process.communicate(timeout=3)
        self.assertGreaterEqual(len(stdout.splitlines()), 2)
        for line in stdout.splitlines():
            self.assertIn("items", json.loads(line))


if __name__ == "__main__":
    unittest.main()
