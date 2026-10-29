#!/usr/bin/env python3
"""Benchmark a live VPP's memif, without UPF, GTP, sessions, or IP forwarding.

Creates one temporary IP-mode master memif and a CPU-isolated VPP slave peer.
RX packets terminate at ip4-not-enabled: neither side gets IP addresses/routes.
Sizes are whole IPv4 packets, not UDP payloads or Ethernet frames. TX includes
the DUT's packet-generator overhead; compare the corresponding PG-only baseline.
Requires local hostNetwork Kubernetes VPP, Docker, and a matching cached image.
Raw statistics stay in --work-dir. Optional quiesce disables specified idle
interfaces during isolated measurements; cleanup restores their admin states.
"""

import argparse
import json
import pathlib
import re
import socket
import struct
import subprocess
import time

from iperf_core import cpus, run, write_json
from iperf_core_diag import process_paths, snapshot


class Bench:
    def __init__(self, args):
        self.args = args
        self.path = pathlib.Path(args.work_dir).resolve()
        self.state_path = self.path / "state.json"
        self.state = json.loads(self.state_path.read_text()) if self.state_path.exists() else {}
        if self.state:
            self.args = argparse.Namespace(**(vars(args) | self.state["fixture"]))
        self.name = f"xcn-memif-only-{self.args.socket_id}"
        self.interface = f"memif{self.args.socket_id}/0"
        self.pg = f"pg{self.args.socket_id}"

    def cli(self, role, command, check=True):
        if role == "dut":
            cmd = ["kubectl", "-n", self.args.namespace, "exec", self.args.pod,
                   "-c", "vpp", "--", "vppctl", command]
        else:
            cmd = ["docker", "exec", self.name, "vppctl", command]
        output = run(cmd, check=check)
        if check and (re.search(r"^.*(?:unknown input|error:|parse error)", output, re.I | re.M)
                      or (not command.startswith("show ") and re.search(r"failed|not found|invalid", output, re.I))):
            raise RuntimeError(f"{role}: {command}\n{output}")
        return output

    def setup(self):
        if self.state:
            raise RuntimeError("Fixture already exists; use run or cleanup")
        args = self.args
        pods = json.loads(run(["kubectl", "-n", args.namespace, "get", "pods",
                               "-l", "app=xcn-5gc", "-o", "json"]))["items"]
        if not args.pod:
            if len(pods) != 1:
                raise RuntimeError("Specify --pod when there is not exactly one core Pod")
            args.pod = pods[0]["metadata"]["name"]
        pod = json.loads(run(["kubectl", "-n", args.namespace, "get", "pod", args.pod, "-o", "json"]))
        if not pod["spec"].get("hostNetwork"):
            raise RuntimeError("The DUT must be a local hostNetwork Pod")
        if run(["docker", "ps", "-aq", "--filter", f"name=^/{self.name}$"]).strip():
            raise RuntimeError(f"{self.name} already exists")
        existing = self.cli("dut", "show memif")
        if re.search(rf"(?m)^\s*{args.socket_id}\s+", existing) or self.interface in existing:
            raise RuntimeError("Temporary memif socket/interface ID is already in use")
        if self.pg in self.cli("dut", "show interface"):
            raise RuntimeError("Temporary PG interface ID is already in use")
        if len(re.findall(r"\bworkers\b", self.cli("dut", "show threads"))) != 1:
            raise RuntimeError("This harness measures a DUT with exactly one VPP Worker")
        workers = sorted(cpus(args.peer_workers))
        if len(workers) != args.queues:
            raise RuntimeError("Use one peer Worker per memif queue")
        test_cpus = [args.peer_main, *workers]
        if len(set(test_cpus)) != len(test_cpus):
            raise RuntimeError("Peer main and workers must have different CPUs")
        allocated = set()
        all_pods = json.loads(run(["kubectl", "get", "pods", "-A", "-o", "json"]))["items"]
        for item in all_pods:
            if item["spec"].get("nodeName") != pod["spec"]["nodeName"]:
                continue
            annotation = item["metadata"].get("annotations", {}).get("xnet.com/used-core", "{}")
            for entry in json.loads(annotation).values():
                allocated.update(cpus(entry["cpu"]))
        for cpu in test_cpus:
            siblings = cpus(pathlib.Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip())
            if siblings & allocated or siblings & (set(test_cpus) - {cpu}):
                raise RuntimeError(f"Peer CPU {cpu} shares an allocated physical core")
        self.path.mkdir(parents=True, exist_ok=True)
        shared = (f"/var/lib/kubelet/pods/{pod['metadata']['uid']}/"
                  "volumes/kubernetes.io~empty-dir/vpp-run")
        fixture_keys = ("namespace", "pod", "socket_id", "queues", "ring_size", "buffer_size",
                        "peer_main", "peer_workers", "image")
        self.state = {"fixture": {key: getattr(args, key) for key in fixture_keys},
                      "pod_uid": pod["metadata"]["uid"], "shared": shared,
                      "dut_interface_created": False, "dut_pg_created": False,
                      "dut_socket_created": False}
        write_json(self.state_path, self.state)
        startup = f"""unix {{ nodaemon cli-listen /run/vpp/cli.sock log /dev/stderr }}
cpu {{ main-core {args.peer_main} corelist-workers {args.peer_workers} }}
memory {{ main-heap-size 1G }}
buffers {{ buffers-per-numa 131072 default data-size {args.buffer_size} page-size 4K }}
plugins {{ plugin dpdk_plugin.so {{ disable }} }}
"""
        (self.path / "peer-startup.conf").write_text(startup)
        run(["docker", "run", "-d", "--name", self.name, "--privileged", "--network", "none",
             "--shm-size", "1g", "--entrypoint", "/usr/bin/vpp", "-v", f"{self.path}:/config",
             "-v", f"{shared}:/dut-vpp", args.image, "-c", "/config/peer-startup.conf"])
        for attempt in range(50):
            try:
                self.cli("peer", "show threads")
                break
            except RuntimeError:
                if attempt == 49:
                    raise
                time.sleep(0.2)
        for role, socket_path in (("dut", "/run/vpp"), ("peer", "/dut-vpp")):
            self.cli(role, f"create memif socket id {args.socket_id} filename {socket_path}/memif-only-{args.socket_id}.sock")
            if role == "dut":
                self.state["dut_socket_created"] = True
                write_json(self.state_path, self.state)
            role_flag = "master" if role == "dut" else "slave"
            self.cli(role, f"create interface memif id 0 socket-id {args.socket_id} {role_flag} mode ip "
                     f"rx-queues {args.queues} tx-queues {args.queues} ring-size {args.ring_size} buffer-size {args.buffer_size}")
            if role == "dut":
                self.state["dut_interface_created"] = True
                write_json(self.state_path, self.state)
            self.cli(role, f"set interface state {self.interface} up")
            self.cli(role, f"create packet-generator interface {self.pg}")
            if role == "dut":
                self.state["dut_pg_created"] = True
                write_json(self.state_path, self.state)
            self.cli(role, f"set interface state {self.pg} up")
        for _ in range(80):
            if "connected" in self.cli("dut", f"show memif {self.interface}"):
                break
            time.sleep(0.25)
        else:
            raise RuntimeError("memif did not connect")
        for q in range(args.queues):
            self.cli("dut", f"set interface rx-placement {self.interface} queue {q} worker 0")
            self.cli("peer", f"set interface rx-placement {self.interface} queue {q} worker {q}")
            # Exclude main from all peer TX queues: no shared queue spinlocks.
            self.cli("peer", f"set interface tx-queue {self.interface} queue {q} threads {q + 1}")
        # Read this VPP release's stat client rather than guessing its shared-memory ABI.
        module = run(["kubectl", "-n", args.namespace, "exec", args.pod, "-c", "vpp", "--", "cat",
                      "/usr/lib/python3/dist-packages/vpp_papi/vpp_stats.py"])
        (self.path / "vpp_stats.py").write_text(module)
        write_json(self.path / "environment.json", {"pod": pod, "peer_inspect": json.loads(run(["docker", "inspect", self.name]))})
        self.capture("setup")
        print(json.dumps({"fixture": "ready", "interface": self.interface, "queues": args.queues}))

    def quiesce(self):
        if not self.state or not self.args.quiet_interface:
            raise RuntimeError("Set up first and specify --quiet-interface NAME")
        pod = json.loads(run(["kubectl", "-n", self.args.namespace, "get", "pod", self.args.pod, "-o", "json"]))
        if pod["metadata"]["uid"] != self.state["pod_uid"]:
            raise RuntimeError("DUT Pod changed; refusing to disable interfaces")
        for name in self.args.quiet_interface:
            if name == self.interface:
                raise RuntimeError("Cannot disable the measured memif")
            output = self.cli("dut", f"show interface {name}")
            match = re.search(rf"(?m)^{re.escape(name)}\s+\d+\s+(up|down)\b", output)
            if not match:
                raise RuntimeError(f"Cannot read admin state of {name}")
            self.state.setdefault("quiet_interfaces", {}).setdefault(name, match.group(1))
            write_json(self.state_path, self.state)
            self.cli("dut", f"set interface state {name} down")
        self.capture("quiesced")
        print(json.dumps({"quiesced": self.state["quiet_interfaces"]}))

    def clients(self):
        # Kubelet's parent directory is root-only. Read both mapped stat segments
        # inside the peer; this does not interrupt either VPP worker with CLI calls.
        code = '''import importlib.util, json, sys, time
spec = importlib.util.spec_from_file_location("bench_stats", "/config/vpp_stats.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
clients = {"dut": module.VPPStats("/dut-vpp/stats.sock"),
           "peer": module.VPPStats("/run/vpp/stats.sock")}
for client in clients.values():
    client.connect()
for request in sys.stdin:
    result = {}
    for role, client in clients.items():
        begin = time.monotonic_ns()
        names = list(client.get_counter("/if/names"))
        counters = {}
        for direction in ("rx", "tx"):
            values = client.get_counter("/if/" + direction)
            for name in sys.argv[1:]:
                index = names.index(name)
                counters[name + "/" + direction] = {
                    key: sum(row[index][key] for row in values if index < len(row))
                    for key in ("packets", "bytes")}
        result[role] = {"monotonic_ns": (begin + time.monotonic_ns()) // 2,
                        "counters": counters}
    print(json.dumps(result), flush=True)
for client in clients.values():
    client.disconnect()
'''
        return subprocess.Popen(["docker", "exec", "-i", self.name, "python3", "-u", "-c", code,
                                 self.pg, self.interface], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, text=True)

    def counters(self, clients):
        clients.stdin.write("sample\n")
        clients.stdin.flush()
        line = clients.stdout.readline()
        if not line:
            raise RuntimeError("VPP statistics reader exited")
        return json.loads(line)

    def capture(self, label):
        data = {}
        for role in ("dut", "peer"):
            data[role] = {cmd: self.cli(role, cmd) for cmd in (
                "show threads", "show memif", "show interface", "show hardware-interfaces",
                "show interface rx-placement", "show runtime", "show errors", "show packet-generator")}
        write_json(self.path / f"{label}-cli.json", data)

    def stream_names(self, role):
        return [f"mo{self.args.socket_id}-{role}-{q}" for q in range(self.args.queues if role == "peer" else 1)]

    def stop(self, role):
        existing = self.cli(role, "show packet-generator", check=False)
        for name in self.stream_names(role):
            if name in existing:
                self.cli(role, f"packet-generator disable {name}", check=False)
                self.cli(role, f"packet-generator delete {name}", check=False)

    def streams(self, role, baseline=False):
        args = self.args
        # Valid, fixed IPv4/UDP packet. Both IP interfaces are deliberately disabled.
        size = args.size
        header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, size, 1, 0, 64, 17, 0,
                             socket.inet_aton("198.18.254.1"), socket.inet_aton("198.18.254.2"))
        words = struct.unpack("!10H", header)
        checksum = sum(words)
        checksum = (checksum & 65535) + (checksum >> 16)
        checksum = (checksum & 65535) + (checksum >> 16)
        header = header[:10] + struct.pack("!H", ~checksum & 65535) + header[12:]
        packet = header + struct.pack("!HHHH", 9000, 9001, size - 20, 0) + bytes(size - 28)
        names = self.stream_names(role)
        for q, name in enumerate(names):
            node = "error-drop" if baseline else self.interface
            rate = args.rate_mpps * 1000000 / len(names)
            self.cli(role, f"packet-generator new {{ name {name} source {self.pg} worker {q} rate {rate} maxframe {args.maxframe} "
                     f"size {size}-{size} interface {self.pg} node {node} data {{ hex 0x{packet.hex()} }} }}")
        return names

    def measure(self):
        args = self.args
        if not self.state:
            raise RuntimeError("Run setup first")
        stem = self.path / args.label
        if pathlib.Path(str(stem) + "-summary.json").exists():
            raise RuntimeError("Choose an unused label")
        pod = json.loads(run(["kubectl", "-n", args.namespace, "get", "pod", args.pod, "-o", "json"]))
        if pod["metadata"]["uid"] != self.state["pod_uid"]:
            raise RuntimeError("DUT Pod was replaced; do not reuse the fixture")
        paths = process_paths(pod)
        clients = self.clients()
        for role in ("dut", "peer"):
            self.stop(role)
        roles = {"rx": ["peer"], "tx": ["dut"], "duplex": ["dut", "peer"],
                 "baseline-dut": ["dut"], "baseline-peer": ["peer"]}[args.direction]
        baseline = args.direction.startswith("baseline")
        created = {}
        samples = []
        try:
            for role in roles:
                created[role] = self.streams(role, baseline)
            initial = self.counters(clients)
            self.capture(args.label + "-before")
            health_before = snapshot(paths)
            for role, names in created.items():
                for name in names:
                    self.cli(role, f"packet-generator enable {name}")
            time.sleep(args.warmup)
            start = self.counters(clients)
            samples.append(start)
            deadline = time.monotonic() + args.duration
            while time.monotonic() < deadline:
                time.sleep(min(1, max(0, deadline - time.monotonic())))
                samples.append(self.counters(clients))
            end = samples[-1]
            health_after = snapshot(paths)
        finally:
            for role, names in created.items():
                for name in names:
                    self.cli(role, f"packet-generator disable {name}", check=False)
        time.sleep(0.5)
        final = self.counters(clients)
        self.capture(args.label + "-after")
        streams = {}
        for sender in roles:
            receiver = "dut" if sender == "peer" else "peer"
            source = f"{self.pg}/rx"
            target = source if baseline else f"{self.interface}/rx"
            sink = sender if baseline else receiver
            elapsed = (end[sink]["monotonic_ns"] - start[sink]["monotonic_ns"]) / 1e9
            observed = {key: end[sink]["counters"][target][key] - start[sink]["counters"][target][key]
                        for key in ("packets", "bytes")}
            # PG RX counters include filled FIFO buffers not yet submitted.
            # Once stopped, stream Count is the exact number actually submitted.
            pg_output = self.cli(sender, "show packet-generator")
            total_generated = 0
            for name in created[sender]:
                count = re.search(rf"(?m)^{re.escape(name)}\s+No\s+(\d+)", pg_output)
                if not count:
                    raise RuntimeError("Missing stopped PG stream count")
                total_generated += int(count.group(1))
            total_received = final[sink]["counters"][target]["packets"] - initial[sink]["counters"][target]["packets"]
            if baseline:
                total_received = total_generated
            sender_elapsed = (end[sender]["monotonic_ns"] - start[sender]["monotonic_ns"]) / 1e9
            sender_mpps = (end[sender]["counters"][source]["packets"] - start[sender]["counters"][source]["packets"]) / sender_elapsed / 1e6
            if observed["packets"] <= 0 or observed["bytes"] != observed["packets"] * args.size:
                raise RuntimeError(f"Packet length or receive count mismatch: {observed}")
            streams[sender] = {"receiver": sink, "seconds": elapsed, **observed,
                "mpps": observed["packets"] / elapsed / 1e6, "gbps": observed["bytes"] * 8 / elapsed / 1e9,
                "sender_mpps": sender_mpps,
                "total_generated": total_generated, "total_received": total_received,
                "total_lost": total_generated - total_received,
                "total_lost_percent": 100 * (total_generated - total_received) / total_generated}
        result = {"label": args.label, "size": args.size, "direction": args.direction,
                  "target_mpps": args.rate_mpps, "warmup": args.warmup, "duration": args.duration,
                  "maxframe": args.maxframe,
                  "streams": streams}
        write_json(pathlib.Path(str(stem) + "-raw.json"), {"initial": initial, "start": start, "end": end,
            "final": final, "samples": samples, "health_before": health_before, "health_after": health_after})
        write_json(pathlib.Path(str(stem) + "-summary.json"), result)
        for role in roles:
            self.stop(role)
        clients.stdin.close()
        clients.wait(timeout=10)
        print(json.dumps(result), flush=True)

    def cleanup(self):
        if not self.state:
            return
        pod = json.loads(run(["kubectl", "-n", self.args.namespace, "get", "pod", self.args.pod, "-o", "json"]))
        if pod["metadata"]["uid"] != self.state["pod_uid"]:
            raise RuntimeError("DUT Pod changed; refusing to delete interfaces from a different VPP")
        for role in ("dut", "peer"):
            self.stop(role)
        run(["docker", "rm", "-f", self.name], check=False)
        if self.state.get("dut_pg_created"):
            self.cli("dut", f"delete packet-generator interface {self.pg}")
        if self.state.get("dut_auto_pg_created"):
            self.cli("dut", "delete packet-generator interface pg-1")
        if self.state.get("dut_interface_created"):
            self.cli("dut", f"delete interface memif {self.interface}")
        if self.state.get("dut_socket_created"):
            self.cli("dut", f"delete memif socket id {self.args.socket_id}")
        for name, admin_state in self.state.get("quiet_interfaces", {}).items():
            self.cli("dut", f"set interface state {name} {admin_state}")
        self.state_path.rename(self.path / "cleaned-state.json")
        print(json.dumps({"fixture": "removed"}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("setup", "quiesce", "run", "cleanup"))
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--namespace", default="xcn")
    parser.add_argument("--pod")
    parser.add_argument("--image", default="vpp:26.06")
    parser.add_argument("--socket-id", type=int, default=178)
    parser.add_argument("--queues", type=int, default=4)
    parser.add_argument("--ring-size", type=int, default=8192)
    parser.add_argument("--buffer-size", type=int, default=2048)
    parser.add_argument("--peer-main", type=int, default=62)
    parser.add_argument("--peer-workers", default="64-67")
    parser.add_argument("--quiet-interface", action="append", default=[],
                        help="quiesce: temporarily disable this idle interface; cleanup restores it")
    parser.add_argument("--label", default="smoke")
    parser.add_argument("--size", type=int, choices=(64, 1400), default=1400)
    parser.add_argument("--direction", choices=("rx", "tx", "duplex", "baseline-dut", "baseline-peer"), default="rx")
    parser.add_argument("--rate-mpps", type=float, default=0, help="Total offered Mpps per direction; 0 = unpaced")
    parser.add_argument("--maxframe", type=int, default=256, help="PG batch size, 1..256; use with rate 0 to avoid timed pacing")
    parser.add_argument("--duration", type=float, default=30)
    parser.add_argument("--warmup", type=float, default=3)
    args = parser.parse_args()
    if args.rate_mpps < 0 or args.duration <= 0 or args.warmup < 0:
        parser.error("Invalid rate or duration")
    if not 1 <= args.maxframe <= 256:
        parser.error("maxframe must be in 1..256")
    if args.queues <= 0 or args.socket_id <= 2:
        parser.error("Use a positive queue count and a non-production socket ID")
    bench = Bench(args)
    if args.action == "setup":
        bench.setup()
    elif args.action == "quiesce":
        bench.quiesce()
    elif args.action == "run":
        bench.measure()
    else:
        bench.cleanup()


if __name__ == "__main__":
    main()
