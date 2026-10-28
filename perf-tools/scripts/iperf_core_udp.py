#!/usr/bin/env python3
"""Measure UDP using the VCL endpoints created by iperf_core.py setup.

Single UE / PDU session; multiple iperf processes/flows are permitted. Rates
are application payload rates, and -b is divided across processes and streams.
VCL must choose its source address from the sending app namespace: explicitly
binding a UDP client is incompatible with this VCL release. The returned
addresses are checked against the real PDU and DN addresses before accepting data.
"""

import argparse
import json
import pathlib
import subprocess
import time

from iperf_core import Bench, DN_IP, RAN, run, write_json


def measure(b):
    b.session()
    label = b.args.label
    if (b.path / f"{label}-summary.json").exists():
        raise RuntimeError("Choose a new label")
    # Initiate from the actual sender in both directions. A UDP server's
    # bound/listening socket has substantially higher VCL overhead with -R.
    source, destination = (("ue", "dn") if b.args.direction == "ul" else ("dn", "ue"))
    address = {"ue": b.state["pdu"]["ipv4"], "dn": DN_IP}
    first_cpu = {"ue": b.args.ue_cpu, "dn": b.args.dn_cpu}
    width = b.state.get("args", {}).get("endpoint_cpu_count", 1)
    # The fixture owns these endpoints; remove idle servers on the sending
    # CPUs before starting receivers. Idle VCL select loops consume a core.
    for name in {"iperf3", pathlib.Path(b.args.client_binary).name,
                 pathlib.Path(b.args.server_binary).name}:
        b.docker(RAN, "pkill", "-x", name, check=False)
    time.sleep(0.5)
    for i in range(b.args.processes):
        cmd = b.endpoint(destination, first_cpu[destination] + i * width,
                         b.args.server_binary, "-s", "-1", "-B", address[destination],
                         "-p", str(5201 + i), "-J", "--logfile",
                         f"/config/{label}-server-{i}.json")
        cmd.insert(2, "-d")
        run(cmd)
    before = b.snapshot(label + "-before")
    jobs = []
    for i in range(b.args.processes):
        cmd = b.endpoint(source, first_cpu[source] + i * width, b.args.client_binary,
                         "-c", address[destination],
                         "-p", str(5201 + i), "-u",
                         "-b", str(int(b.args.mbps * 1000000 /
                                       b.args.processes / b.args.streams)),
                         "-l", str(b.args.payload), "-t", str(b.args.seconds),
                         "-O", str(b.args.omit), "--pacing-timer", "100",
                         "-P", str(b.args.streams), "-J")
        # Bound the in-container process too; killing docker exec alone would
        # leave its iperf child running and contaminate later measurements.
        cmd[3:3] = ["timeout", "-k", "2", str(b.args.seconds + b.args.omit + 60)]
        jobs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True))
        # Serialize VCL attachment/UDP listener creation in the test VPP.
        time.sleep(0.1)
    values, errors = [], []
    for i, process in enumerate(jobs):
        try:
            # VCL may take about 20s to detach after finishing the measurement.
            stdout, stderr = process.communicate(timeout=b.args.seconds + b.args.omit + 65)
        except subprocess.TimeoutExpired:
            process.kill()
            stdout, stderr = process.communicate()
            errors.append(f"process {i}: timed out")
        (b.path / f"{label}-{i}.json").write_text(stdout)
        (b.path / f"{label}-{i}.stderr").write_text(stderr)
        try:
            value = json.loads(stdout)
            if process.returncode or "error" in value:
                raise ValueError(value.get("error", stderr))
            if value["start"]["test_start"]["protocol"] != "UDP":
                raise ValueError("Unexpected transport")
            if len(value["start"]["connected"]) != b.args.streams:
                raise ValueError("Unexpected number of connected UDP streams")
            if any(c["local_host"] != address[source] or c["remote_host"] != address[destination]
                   for c in value["start"]["connected"]):
                raise ValueError("Source address does not match the PDU")
            values.append(value)
        except (ValueError, KeyError) as error:
            errors.append(f"process {i}: {error}")
    after = b.snapshot(label + "-after")
    b.session()
    if errors:
        raise RuntimeError("; ".join(errors))
    rx = [v["end"]["sum_received"] for v in values]
    tx = [v["end"]["sum_sent"] for v in values]
    packets = sum(v["packets"] for v in rx)
    lost = sum(v["lost_packets"] for v in rx)
    if any(not v["packets"] or not v["bytes"] for v in rx):
        raise RuntimeError("No UDP packets received")
    if not 0 <= lost <= packets:
        raise RuntimeError("Invalid UDP loss counters")
    result = {"label": label, "mode": b.state["mode"], "protocol": "UDP",
              "args": vars(b.args), "payload_bytes": b.args.payload,
              "offered_mbps": b.args.mbps,
              "sender_mbps": sum(v["bits_per_second"] for v in tx) / 1000000,
              "receiver_mbps": sum(v["bits_per_second"] for v in rx) / 1000000,
              "lost_packets": lost, "packets": packets,
              "lost_percent": lost * 100 / packets,
              "jitter_ms_per_process": [v["jitter_ms"] for v in rx],
              "iperf_cpu": [v["end"]["cpu_utilization_percent"] for v in values],
              "session_before": before["rates"], "session_after": after["rates"]}
    write_json(b.path / f"{label}-summary.json", result)
    print(json.dumps(result), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--work-dir", required=True)
    p.add_argument("--namespace", default="xcn")
    p.add_argument("--pod")
    p.add_argument("--smf-url", default="http://127.0.0.1:9092/pdu-info")
    p.add_argument("--label", default="udp")
    p.add_argument("--client-binary", default="iperf3")
    p.add_argument("--server-binary", default="iperf3")
    p.add_argument("--direction", choices=("ul", "dl"), default="ul")
    p.add_argument("--mbps", type=float, default=1000)
    p.add_argument("--payload", type=int, choices=range(64, 1401), default=1400)
    p.add_argument("--seconds", type=int, default=30)
    p.add_argument("--omit", type=int, default=3)
    p.add_argument("--processes", type=int, choices=range(1, 17), default=4)
    p.add_argument("--streams", type=int, default=1)
    p.add_argument("--ue-cpu", type=int, default=64)
    p.add_argument("--dn-cpu", type=int, default=68)
    args = p.parse_args()
    if args.mbps <= 0 or args.seconds < 1 or args.omit < 0 or args.streams < 1:
        p.error("Invalid offered rate, duration or stream count")
    b = Bench(args)
    if not b.state:
        p.error("Create the VPP fixture with iperf_core.py setup first")
    if args.processes > b.state.get("args", {}).get("endpoint_count", 4):
        p.error("More processes requested than servers created at setup")
    measure(b)


if __name__ == "__main__":
    main()
