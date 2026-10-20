#!/usr/bin/env python3
"""Run the real CLI against isolated HTTP and UPF control socket fixtures."""
import copy
import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import unittest
import urllib.parse

BINARY = os.path.abspath(sys.argv.pop(1))
SUPI = "imsi-001010000000001"


def ue(bits=28, ident=100, state="connected"):
    gnb = {"gnb_id": ident, "gnb_id_bits": bits, "plmn": "00101",
           "peer": "[2001:db8::1]:38412"}
    return {"supi": SUPI, "cm_state": state,
            "gnb": dict(gnb, amf_ue_ngap_id=9, ran_ue_ngap_id=7),
            "location": {"timestamp": 1790000000000000, "gnb": copy.deepcopy(gnb),
                         "nr_cgi": {"plmn": "00101", "nci": (ident << (36 - bits)) | 3},
                         "nr_tai": {"plmn": "00101", "tac": 1}},
            "pdu_sessions": [{"psi": 2, "dnn": "internet",
                              "snssai": {"sst": 1, "sd": "010101"}}]}


def pdu():
    return {"supi": SUPI, "pdu": [{"psi": 2, "upf_seid": "811",
            "ipv4": "10.45.0.3", "ipv6": "2001:db8:45::3", "dnn": "internet",
            "snssai": {"sst": 1, "sd": "010101"}, "pdu_state": "active",
            "qos_flows": [{"qfi": 1, "5qi": 9}]}]}


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

    def test_ue_without_upf_session(self):
        self.http.data["/ue-info"][0]["pdu_sessions"] = []
        result = self.run_cli()
        self.assertIn(SUPI, result.stdout)
        self.assertIn("小区ID(NCI)", result.stdout)
        self.assertIn("0x000006403", result.stdout)
        self.assertNotIn(("/pdu-info", 0), self.http.requests)

    def test_detail_and_session_join(self):
        result = self.run_cli("ue", "--supi", SUPI, "--detail")
        for text in ("100 (28 bits)", "00101-0x000006403", "10.45.0.3",
                     "2001:db8:45::3", "internet", "Location updated:",
                     "2001:db8::1", "active"):
            self.assertIn(text, result.stdout)
        self.assertEqual(self.item()["pdu_sessions"][0]["psi"], 2)

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

    def test_zero_identity_is_valid(self):
        self.http.data["/ue-info"] = [ue(22, 0)]
        self.assertEqual(self.item()["gnb"]["gnb_id"], 0)
        result = self.run_cli("ue", "--gnb-id", "0", "--plmn", "00101")
        self.assertIn(SUPI, result.stdout)

    def test_identity_mismatch(self):
        self.http.data["/ue-info"][0]["location"]["nr_cgi"]["nci"] = 101 << 8
        item = self.item()
        self.assertEqual(item["gnb"]["gnb_id"], 100)
        self.assertEqual(item["location"]["nr_cgi"]["nci"], 101 << 8)
        self.assertFalse(item["location"]["nr_cgi"]["identity_consistent"])
        self.assertIsNone(item["location"]["nr_cgi"]["cell_id"])
        self.assertIn("identity-mismatch", self.run_cli().stdout)

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
        data = json.loads(self.run_cli("gnb", "--json", "--plmn", "00102").stdout)
        self.assertEqual(len(data["items"]), 1)
        self.assertIn("Supported TA/PLMN/slices", self.run_cli("gnb", "--detail").stdout)
        self.assertIn("ready", self.run_cli("gnb").stdout)

    def test_smf_unavailable_is_optional(self):
        self.http.data["/pdu-info"] = (503, b"unavailable")
        result = self.run_cli("ue", "--json")
        self.assertIn("warning", result.stderr)
        self.assertIsNone(json.loads(result.stdout)["items"][0]["pdu_sessions"][0]["ipv4"])

    def test_ambiguous_session_or_supi_not_merged(self):
        self.http.data["/pdu-info"][0]["pdu"].append(copy.deepcopy(pdu()["pdu"][0]))
        self.assertIsNone(self.item()["pdu_sessions"][0]["ipv4"])
        self.http.data["/pdu-info"] = [pdu(), pdu()]
        self.assertIsNone(self.item()["pdu_sessions"][0]["ipv4"])

    def test_amf_unavailable_is_failure(self):
        self.http.data["/ue-info"] = (503, b"unavailable")
        self.assertIn("cannot load", self.run_cli(expected=1).stderr)

    def test_conflicting_session_not_merged(self):
        self.http.data["/pdu-info"][0]["pdu"][0]["dnn"] = "other"
        self.assertIsNone(self.item()["pdu_sessions"][0]["ipv4"])

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
                if level != "user":
                    self.assertEqual(row["psi"], 2)
                result = self.run_cli("rate", "--level", level, "--with-location")
                header, separator, row = result.stdout.strip().splitlines()
                self.assertIn("-+-", separator)
                self.assertEqual(len(header.split("|")), len(row.split("|")))
                self.assertIn("GNB-ID", header)
                self.assertIn("小区ID(NCI)", header)
                self.assertIn("0x000006403", row)
                widths = lambda line: [sum(2 if unicodedata.east_asian_width(c) in "WF" else 1
                                          for c in column) for column in line.split("|")]
                self.assertEqual(widths(header), widths(row))
                self.assertEqual(widths(header), [len(c) for c in separator.split("+")])

    def test_rate_optional_location_failure_and_active_only(self):
        self.http.data["/ue-info"] = (503, b"unavailable")
        result = self.run_cli("rate", "--with-location", "--json")
        self.assertIn("warning", result.stderr)
        self.assertEqual(json.loads(result.stdout)["rows"][0]["location"]["status"], "unknown")
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
