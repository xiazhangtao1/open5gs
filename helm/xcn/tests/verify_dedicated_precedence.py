#!/usr/bin/env python3
"""Live test: requires an online, idle test UE with no dedicated bearers.

Run with --url http://NODE:30777 --supi imsi-... --psi 2.
Creates QoS flows and deletes only the application IDs created by this test.
"""
import argparse
import concurrent.futures
import json
import subprocess
import time
import urllib.parse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--supi", required=True)
    parser.add_argument("--psi", required=True, type=int)
    args = parser.parse_args()
    base = args.url.rstrip("/") + "/xcn-dedicated-bearer/v1/bearers"
    created = []

    def request(method, url, body=None):
        cmd = ["curl", "--http2-prior-knowledge", "--max-time", "15",
               "-sS", "-i", "-X", method, url]
        if body is not None:
            cmd += ["-H", "Content-Type: application/json", "-d", json.dumps(body)]
        output = subprocess.check_output(cmd, text=True)
        headers, payload = output.split("\n\n", 1)
        status = int(headers.splitlines()[0].split()[1])
        if method == "POST" and status == 201:
            location = next(line.split(":", 1)[1].strip()
                            for line in headers.splitlines()
                            if line.lower().startswith("location:"))
            created.append(location.rstrip("/").rsplit("/", 1)[1])
        return status, json.loads(payload) if payload.strip() else None

    def snapshot():
        status, body = request("GET", base + "?" + urllib.parse.urlencode(
            {"supi": args.supi, "pduSessionId": args.psi}))
        assert status == 200, (status, body)
        return {b["appSessionId"]: b["pccRules"][0]["precedence"]
                for b in body["bearers"]}

    def bearer(app_id):
        status, data = request("GET", base + "?" + urllib.parse.urlencode(
            {"supi": args.supi, "pduSessionId": args.psi}))
        assert status == 200, (status, data)
        return next(b for b in data["bearers"] if b["appSessionId"] == app_id)

    def body(address, precedence=None):
        qos = {"5qi": 3, "arp": {"priorityLevel": 8},
               "maxbrDl": "10 Mbps", "maxbrUl": "10 Mbps",
               "gbrDl": "5 Mbps", "gbrUl": "5 Mbps"}
        if precedence is not None:
            qos["precedence"] = precedence
        return {"supi": args.supi, "pduSessionId": args.psi, "qos": qos,
                "flowDescriptions": [f"permit out ip from {address}/32 to assigned",
                                     f"permit in ip from assigned to {address}/32"]}

    def create(address, precedence=None):
        status, response = request("POST", base, body(address, precedence))
        assert status == 201, (status, response)

    def wait_absent(app_id):
        for _ in range(50):
            if app_id not in snapshot():
                return
            time.sleep(0.2)
        raise AssertionError(f"Application {app_id} was not deleted")

    assert not snapshot(), "Use a test session without existing dedicated bearers"
    try:
        create("10.45.0.1")
        first = snapshot()
        app_id = next(iter(first))
        qos_only = body("10.45.0.1")
        qos_only["qos"]["5qi"] = 4
        status, result = request("PATCH", base + "/" + app_id, qos_only)
        assert status == 200 and result["pccRules"][0]["qos"]["5qi"] == 4
        time.sleep(0.1)

        arp_only = body("10.45.0.1")
        arp_only["qos"]["5qi"] = 4
        arp_only["qos"]["arp"]["priorityLevel"] = 9
        status, result = request("PATCH", base + "/" + app_id, arp_only)
        assert status == 200 and result["pccRules"][0]["qos"]["arp"]["priorityLevel"] == 9
        time.sleep(0.1)

        updated = body("192.0.2.10")
        updated["qos"]["5qi"] = 4
        status, result = request("PATCH", base + "/" + app_id, updated)
        assert status == 200 and result["appSessionId"] == app_id, (status, result)
        time.sleep(0.1)

        updated["qos"]["maxbrDl"] = "12 Mbps"
        status, result = request("PATCH", base + "/" + app_id, updated)
        assert status == 200 and result["appSessionId"] == app_id, (status, result)
        time.sleep(0.1)

        rule = bearer(app_id)["pccRules"][0]
        assert rule["qos"]["5qi"] == 4 and rule["qos"]["mbrDl"] == 12000000, rule
        assert "192.0.2.10" in rule["flows"][0]["description"], rule
        assert snapshot() == first, "PATCH changed the application or precedence"

        broken_update = body("192.0.2.11")
        broken_update["flowDescriptions"] = ["invalid flow"]
        status, _ = request("PATCH", base + "/" + app_id, broken_update)
        assert status == 400 and bearer(app_id)["pccRules"][0] == rule, status
        print("PASS in-place update and failed-update rollback", flush=True)

        create("10.2.0.119")
        second = snapshot()
        assert len(second) == 2 and len(set(second.values())) == 2, second
        assert all(second[k] == v for k, v in first.items()), second
        print("PASS sequential create and existing precedence retention", second, flush=True)

        other_precedence = next(value for key, value in second.items() if key != app_id)
        status, _ = request("PATCH", base + "/" + app_id,
                            body("192.0.2.11", other_precedence))
        assert status == 409 and snapshot() == second, status
        status, _ = request("PATCH", base + "/" + app_id,
                            body("192.0.2.11", 150))
        assert status == 409 and snapshot() == second, status
        print("PASS installed precedence is immutable", flush=True)

        occupied = second[app_id]
        status, _ = request("POST", base, body("192.0.2.1", occupied))
        assert status == 409 and snapshot() == second, status
        for invalid in (-1, 255, 256, 100.5, "100", True):
            status, _ = request("POST", base, body("192.0.2.1", invalid))
            assert status == 400 and snapshot() == second, (invalid, status)
        print("PASS conflict and invalid input rejection", flush=True)

        broken = body("192.0.2.1", 200)
        broken["flowDescriptions"] = ["invalid flow"]
        status, _ = request("POST", base, broken)
        assert status >= 400 and snapshot() == second, status
        create("192.0.2.1", 200)
        assert 200 in snapshot().values()
        print("PASS failed creation releases precedence", flush=True)

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            list(pool.map(create, ["192.0.2.2", "192.0.2.3"]))
        current = snapshot()
        assert len(current) == 5 and len(set(current.values())) == 5, current
        print("PASS concurrent create", current, flush=True)

        app_id = next(iter(first))
        status, _ = request("DELETE", base + "/" + app_id)
        assert status == 204, status
        wait_absent(app_id)
        created.remove(app_id)
        create("192.0.2.4", occupied)
        assert occupied in snapshot().values()
        print("PASS reuse after deletion", snapshot(), flush=True)
    finally:
        for app_id in list(created):
            status, response = request("DELETE", base + "/" + app_id)
            assert status in (204, 404), (status, response)
            wait_absent(app_id)
        print("Cleanup complete", snapshot(), flush=True)


if __name__ == "__main__":
    main()
