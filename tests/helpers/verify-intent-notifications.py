#!/usr/bin/env python3
"""Check the exact sequence from XCN_INTENT_TEST (including retransmission dedup)."""
import argparse
import ipaddress
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("path")
parser.add_argument("--resource-failure", action="store_true")
args = parser.parse_args()
with open(args.path, encoding="utf-8") as source:
    records = [json.loads(line) for line in source]
expected = [(1, 1, 2, "FAILURE", None), (1, 1, 3, "SUCCESS", None),
            (1, 2, 2, "SUCCESS", 2), (1, 2, 2, "SUCCESS", 2),
            (1, 1, 3, "SUCCESS", 2), (1, 2, 2, "SUCCESS", 2),
            (1, 3, 1, "SUCCESS", 3), (1, 3, 3, "SUCCESS", 3),
            (1, 2, 2, "SUCCESS", 2)]
expected += [(1, 2, 2, "SUCCESS", 2)] * 40  # 39 decrements plus floor no-op
expected += [(0, 1, 3, "SUCCESS", None)] * 2
if args.resource_failure:
    expected.append((1, 2, 2, "FAILURE", None))
assert len(records) == len(expected), (len(records), len(expected))
for record, want in zip(records, expected):
    body, headers = record["body"], record["headers"]
    actual = tuple(body[key] for key in ("commStatus", "aiModel", "gpuLevel", "qosResult")) + (body.get("fiveQi"),)
    assert actual == want, (actual, want)
    assert body["rawPco"] == "%02x%02x%02x" % want[:3]
    assert ("fiveQi" in body) == (want[4] is not None)
    assert 0 <= record["receivedAt"] - body["timestamp"] < 1000
    assert ipaddress.ip_address(body["ueIp"]).is_private
    assert body["supi"].startswith("imsi-")
    assert headers[":method"] == "POST" and headers[":path"] == "/api/v1/intent/notify"
    assert headers["content-type"] == "application/json; charset=utf-8"
    assert "authorization" not in headers
print(f"PASS: {len(records)} notifications, exact order/values, no duplicates")
