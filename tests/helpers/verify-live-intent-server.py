#!/usr/bin/env python3
"""Exercise a real intent receiver using curl h2c prior knowledge.

Sends state-changing test notifications. Use an explicitly reserved test UE IP.
Records transport, JSON validation and optional monitor snapshots. Does not
exercise image/model inference or infer processing success solely from HTTP 204.
"""
import argparse
import copy
import json
import subprocess
import tempfile
import time
from pathlib import Path


def exchange(method, url, body=None, *, raw_body=None):
    with tempfile.TemporaryDirectory(prefix="intent-http-") as directory:
        payload = Path(directory) / "body"
        cmd = ["curl", "--noproxy", "*", "--http2-prior-knowledge",
               "--connect-timeout", "3", "--max-time", "5", "-sS",
               "-X", method, "-o", str(payload), "-w", "%{json}", url]
        if body is not None or raw_body is not None:
            cmd += ["-H", "Content-Type: application/json; charset=utf-8",
                    "--data-binary", raw_body if raw_body is not None else
                    json.dumps(body, ensure_ascii=False)]
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=7)
            metrics = json.loads(result.stdout) if result.stdout else {}
        except (OSError, subprocess.TimeoutExpired, ValueError) as error:
            return {"curlExit": -1, "stderr": str(error), "httpStatus": None,
                    "httpVersion": None, "totalMs": None, "response": ""}
        return {"curlExit": result.returncode, "stderr": result.stderr,
                "httpStatus": metrics.get("http_code"),
                "httpVersion": metrics.get("http_version"),
                "totalMs": metrics.get("time_total", 0) * 1000,
                "response": payload.read_text() if payload.exists() else ""}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Exact /api/v1/intent/notify URL")
    parser.add_argument("--ue-ip", required=True)
    parser.add_argument("--supi", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--monitor-url")
    args = parser.parse_args()
    records = []
    base = {"ueIp": args.ue_ip, "supi": args.supi, "commStatus": 1,
            "aiModel": 2, "gpuLevel": 2, "qosResult": "SUCCESS"}

    def send(name, body, expected, *, raw_body=None):
        result = exchange("POST", args.url, body, raw_body=raw_body)
        issues = []
        if result["curlExit"] or result["httpVersion"] != "2":
            issues.append("h2c transport failure")
        if result["httpStatus"] != expected:
            issues.append(f"expected HTTP {expected}")
        if expected == 204 and result["response"]:
            issues.append("204 must have no response body")
        if expected == 400:
            try:
                error = json.loads(result["response"])
                if not isinstance(error.get("error"), str):
                    issues.append("400 must contain string error")
            except (ValueError, AttributeError):
                issues.append("400 must contain error JSON")
        if result["totalMs"] is not None and result["totalMs"] > 100:
            issues.append("observed latency exceeded 100 ms (network included)")
        record = {"case": name, "request": body, "expectedStatus": expected,
                  **result, "issues": issues}
        if raw_body is not None:
            record["rawRequest"] = raw_body
        if args.monitor_url and expected == 204:
            time.sleep(0.15)
            monitor = exchange("GET", args.monitor_url)
            try:
                record["monitor"] = json.loads(monitor["response"])
            except ValueError:
                record["monitorResponse"] = monitor
        records.append(record)
        latency = f"{result['totalMs']:.2f} ms" if result["totalMs"] is not None else "unavailable"
        print(f"{name}: HTTP {result['httpStatus']} {latency}"
              + (" ISSUES: " + "; ".join(issues) if issues else " PASS"), flush=True)

    try:
        valid = [("off", 0, 1, 3, "SUCCESS", None)]
        valid += [(f"model{model}-gpu{gpu}", 1, model, gpu, "SUCCESS",
                   model if model in (2, 3) else None)
                  for model in (1, 2, 3) for gpu in (1, 2, 3)]
        valid += [(f"llm-retain-{qi}", 1, 1, 2, "SUCCESS", qi) for qi in (2, 3)]
        valid += [(f"qos-failure-{model}", 1, model, 3, "FAILURE", None)
                  for model in (1, 2, 3)]
        valid += [("off-failure", 0, 3, 1, "FAILURE", None),
                  ("off-face", 0, 2, 2, "SUCCESS", None)]
        for name, on, model, gpu, qos, qi in valid:
            body = dict(base, commStatus=on, aiModel=model, gpuLevel=gpu,
                        qosResult=qos, rawPco=f"{on:02x}{model:02x}{gpu:02x}",
                        timestamp=int(time.time() * 1000))
            if qi is not None:
                body["fiveQi"] = qi
            send(name, body, 204)
        minimal = {key: base[key] for key in
                   ("ueIp", "commStatus", "aiModel", "gpuLevel", "qosResult")}
        send("minimal-required", minimal, 204)
        for key in minimal:
            body = copy.deepcopy(minimal)
            del body[key]
            send("missing-" + key, body, 400)
        for key, value in [("commStatus", 2), ("aiModel", 4), ("gpuLevel", 0),
                           ("qosResult", "UNKNOWN"), ("ueIp", "invalid-ip"),
                           ("commStatus", "1"), ("gpuLevel", True), ("aiModel", 2.0)]:
            send(f"invalid-{key}-{value}", dict(minimal, **{key: value}), 400)
        send("empty-body", None, 400)
        send("malformed-json", None, 400, raw_body="{")
    finally:
        cleanup = dict(base, commStatus=0, aiModel=1, gpuLevel=1,
                       rawPco="000101", timestamp=int(time.time() * 1000))
        send("cleanup-off", cleanup, 204)
        Path(args.output).write_text(json.dumps(records, ensure_ascii=False, indent=2),
                                    encoding="utf-8")
    issues = sum(bool(record.get("issues")) for record in records)
    print(f"Completed {len(records)} requests, {issues} cases with issues; {args.output}")
    raise SystemExit(1 if issues else 0)


if __name__ == "__main__":
    main()
