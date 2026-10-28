#!/usr/bin/env python3
"""Measure TCP on one real PFCP session with iperf3 and VCL, without the OAI radio data path.

Requires local Docker, Kubernetes access, hostNetwork VPP/memif UPF, and exactly
one established PDU session. See perf-tools/results for a worked measurement.
The script does not redeploy the core or change its CPU/queue/QoS settings.
"""

import argparse
import datetime
import ipaddress
import json
import pathlib
import re
import shlex
import subprocess
import time
import urllib.request


RAN = "xcn-iperf-ran"
DN_IP = "198.19.0.2"


def run(args, *, data=None, check=True, timeout=45):
    result = subprocess.run(
        [str(a) for a in args], input=data, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(f"{shlex.join(map(str, args))}: {result.stderr}{result.stdout}")
    return result.stdout


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def http_json(url):
    # The host's Internet proxy must not intercept the local SMF metrics API.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(url, timeout=5) as response:
        return json.load(response)


def cpus(value):
    result = set()
    for item in value.split(","):
        bounds = [int(part) for part in item.split("-")]
        result.update(range(bounds[0], bounds[-1] + 1))
    return result


class Bench:
    def __init__(self, args):
        self.args = args
        self.path = pathlib.Path(args.work_dir).resolve()
        self.state_file = self.path / "state.json"
        self.state = json.loads(self.state_file.read_text()) if self.state_file.exists() else {}
        if self.state:
            # Setup addresses/CPU placement are part of the test fixture.
            for name in ("namespace", "pod", "smf_url", "ue_cpu", "dn_cpu"):
                if name in self.state.get("args", {}):
                    setattr(args, name, self.state["args"][name])
        if not args.pod:
            pods = json.loads(run(["kubectl", "-n", args.namespace, "get", "pods",
                                   "-l", "app=xcn-5gc", "-o", "json"]))["items"]
            if len(pods) != 1:
                raise RuntimeError("Specify --pod: exactly one core Pod is required")
            args.pod = "pod/" + pods[0]["metadata"]["name"]

    def core(self, container, *args, **kwargs):
        return run(["kubectl", "-n", self.args.namespace, "exec", "-i",
                    self.args.pod, "-c", container, "--", *args], **kwargs)

    def docker(self, name, *args, **kwargs):
        return run(["docker", "exec", "-i", name, *args], **kwargs)

    def endpoint(self, role, cpu, *args):
        # Set LD_PRELOAD after taskset, to attach only iperf to VCL.
        width = self.state.get("args", {}).get("endpoint_cpu_count", 1)
        affinity = ",".join(str(c) for c in range(cpu, cpu + width))
        return ["docker", "exec", RAN, "taskset", "-c", affinity, "env",
                f"VCL_CONFIG=/config/{role}-vcl.conf",
                "LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libvcl_ldpreload.so", *args]

    def cli(self, commands, *, fake=False, check=True):
        execute = (lambda *a, **kw: self.docker(RAN, *a, **kw)) if fake else (
            lambda *a, **kw: self.core("vpp", *a, **kw))
        execute("tee", "/tmp/xcn-iperf.vpp", data="\n".join(commands) + "\n")
        output = execute("vppctl", "exec", "/tmp/xcn-iperf.vpp", check=check)
        if check and re.search(r"unknown input|failed|not found|invalid|error:", output, re.I):
            raise RuntimeError(output)
        return output

    def session(self):
        data = http_json(self.args.smf_url)
        sessions = [(ue["supi"], pdu) for ue in data["items"] for pdu in ue.get("pdu", [])]
        rates = json.loads(self.core("upf", "xcnctl", "show", "rate", "--level", "session", "--json"))
        if len(sessions) != 1 or len(rates["rows"]) != 1:
            raise RuntimeError("Exactly one SMF PDU and one UPF PFCP session are required")
        supi, pdu = sessions[0]
        if pdu.get("qos_flows") != [{"qfi": 1, "is_default": True, "5qi": 9}]:
            raise RuntimeError("This default-session harness requires only the default QFI 1 / 5QI 9")
        for endpoint in pdu["n3"].values():
            if not 0 < endpoint["teid"] <= 0xffffffff:
                raise RuntimeError("Invalid TEID")
        ipaddress.IPv4Address(pdu["ipv4"])
        if self.state and (supi != self.state["supi"] or pdu["ipv4"] != self.state["pdu"]["ipv4"] or
                           pdu["upf_seid"] != self.state["pdu"]["upf_seid"] or
                           pdu["n3"] != self.state["pdu"]["n3"]):
            raise RuntimeError("Session/TEIDs changed; cleanup and setup again")
        return supi, pdu, rates

    def setup(self):
        if self.state:
            raise RuntimeError("State already exists; cleanup first")
        for name in (RAN,):
            if run(["docker", "ps", "-aq", "--filter", f"name=^/{name}$"]).strip():
                raise RuntimeError(f"Container {name} already exists")
        supi, pdu, _ = self.session()
        pod = json.loads(run(["kubectl", "-n", self.args.namespace,
                              "get", self.args.pod, "-o", "json"]))
        if not pod["spec"].get("hostNetwork"):
            raise RuntimeError("This harness requires a local hostNetwork core Pod")
        existing = self.core("vpp", "vppctl", "show", "memif")
        if re.search(r"(?m)^\s*(88|89)\s+", existing) or any(
                name in existing for name in ("interface memif88/0", "interface memif89/0")):
            raise RuntimeError("Test memif IDs 88/89 are already in use")
        workers = cpus(self.args.ran_worker_cpus)
        test_cpus = [self.args.ran_main_cpu, *sorted(workers),
                     *range(self.args.ue_cpu, self.args.ue_cpu + self.args.endpoint_count * self.args.endpoint_cpu_count),
                     *range(self.args.dn_cpu, self.args.dn_cpu + self.args.endpoint_count * self.args.endpoint_cpu_count)]
        if len(workers) < 2 or len(set(test_cpus)) != len(test_cpus):
            raise RuntimeError("Use at least two test VPP Workers and disjoint endpoint CPU sets")
        allocated = set()
        pods = json.loads(run(["kubectl", "get", "pods", "-A", "-o", "json"]))["items"]
        for item in pods:
            if item["spec"].get("nodeName") != pod["spec"]["nodeName"]:
                continue
            annotation = item["metadata"].get("annotations", {}).get("xnet.com/used-core", "{}")
            for entry in json.loads(annotation).values():
                allocated.update(cpus(entry["cpu"]))
        for cpu in test_cpus:
            siblings = cpus(pathlib.Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip())
            if siblings & allocated:
                raise RuntimeError(f"Test CPU {cpu} shares a physical core with an allocated Pod")
        self.path.mkdir(parents=True, exist_ok=True)
        gnb = pdu["n3"]["gnb"]["addr"].split("]")[0].lstrip("[")
        upf = pdu["n3"]["upf"]["addr"].split("]")[0].lstrip("[")
        self.state = {"supi": supi, "pdu": pdu, "pod_uid": pod["metadata"]["uid"],
                      "args": vars(self.args), "mode": "core", "gnb": gnb, "upf": upf}
        write_json(self.state_file, self.state)
        startup = f"""unix {{ nodaemon cli-listen /run/vpp/cli.sock log /dev/stderr }}
cpu {{ main-core {self.args.ran_main_cpu} corelist-workers {self.args.ran_worker_cpus} }}
memory {{ main-heap-size 1G }}
buffers {{ buffers-per-numa 65536 default data-size 2048 page-size 4K }}
plugins {{ plugin dpdk_plugin.so {{ disable }} }}
"""
        (self.path / "fake-startup.conf").write_text(startup)
        shared = (f"/var/lib/kubelet/pods/{pod['metadata']['uid']}/"
                  "volumes/kubernetes.io~empty-dir/vpp-run")
        run(["docker", "run", "-d", "--name", RAN, "--privileged", "--network", "none", "--shm-size", "1g",
             "--entrypoint", "/usr/bin/vpp", "-v", f"{self.path}:/config",
             "-v", f"{shared}:/core-vpp", self.args.vpp_image, "-c", "/config/fake-startup.conf"])
        for _ in range(30):
            try:
                self.docker(RAN, "vppctl", "show", "threads")
                break
            except RuntimeError:
                time.sleep(0.2)
        else:
            raise RuntimeError("Test VPP did not start")
        if self.args.iperf_binary:
            run(["docker", "cp", self.args.iperf_binary, f"{RAN}:/usr/local/bin/iperf3"])
        version = self.docker(RAN, "iperf3", "--version")
        match = re.search(r"iperf (\d+)\.(\d+)", version)
        if not match or tuple(map(int, match.groups())) < (3, 11):
            raise RuntimeError("Use iperf3 >= 3.11; provide --iperf-binary for an existing binary")
        (self.path / "iperf-version.txt").write_text(version)
        # A VPP memif slave uses its native buffer pool without a core-side
        # RX/TX payload copy. Only the added test links change role; the UPF's
        # production N3/N6 memifs retain their original settings.
        core_role = "slave" if self.args.test_core_zero_copy else "master"
        ran_role = "master" if self.args.test_core_zero_copy else "slave"
        self.cli([
            "create memif socket id 88 filename /run/vpp/perf-n3.sock",
            f"create interface memif id 0 socket-id 88 {core_role} mode ip ring-size 8192 buffer-size 2048 rx-queues 1 tx-queues 1",
            "set interface ip table memif88/0 10", "set interface state memif88/0 up",
            "set interface unnumbered memif88/0 use dpdk-n3",
            f"ip route add table 10 {gnb}/32 via memif88/0",
            "create memif socket id 89 filename /run/vpp/perf-n6.sock",
            f"create interface memif id 0 socket-id 89 {core_role} mode ip ring-size 8192 buffer-size 2048 rx-queues 1 tx-queues 1",
            "set interface state memif89/0 up", "set interface unnumbered memif89/0 use memif1/0",
            "ip route add 198.19.0.0/24 via memif89/0",
        ])
        self.cli([
            "ip table add 10", "ip table add 20",
            "create memif socket id 88 filename /core-vpp/perf-n3.sock",
            f"create interface memif id 0 socket-id 88 {ran_role} mode ip ring-size 8192 buffer-size 2048 rx-queues 1 tx-queues 1",
            "set interface ip table memif88/0 10", "set interface state memif88/0 up",
            f"set interface ip address memif88/0 {gnb}/32",
            f"ip route add table 10 {upf}/32 via memif88/0",
            "create memif socket id 89 filename /core-vpp/perf-n6.sock",
            f"create interface memif id 0 socket-id 89 {ran_role} mode ip ring-size 8192 buffer-size 2048 rx-queues 1 tx-queues 1",
            "set interface state memif89/0 up", "set interface ip address memif89/0 198.18.0.2/32",
            "ip route add 10.45.0.0/16 via memif89/0",
            "create loopback interface instance 88", "set interface ip table loop88 20",
            f"set interface ip address loop88 {pdu['ipv4']}/32", "set interface state loop88 up",
            "create loopback interface instance 89", f"set interface ip address loop89 {DN_IP}/32",
            "set interface state loop89 up",
            f"create gtpu tunnel src {gnb} dst {upf} teid {pdu['n3']['gnb']['teid']} "
            f"tteid {pdu['n3']['upf']['teid']} encap-vrf-id 10 decap-next ip4 qfi 1",
            "set interface ip table gtpu_tunnel0 20", "set interface state gtpu_tunnel0 up",
            "set interface unnumbered gtpu_tunnel0 use loop88",
            "ip route add table 20 0.0.0.0/0 via gtpu_tunnel0",
            "session enable",
            "app ns add id ue secret 42 if loop88", "app ns add id dn secret 42 if loop89",
        ], fake=True)
        for role in ("ue", "dn"):
            (self.path / f"{role}-vcl.conf").write_text(f"""vcl {{
 app-socket-api /run/vpp/app_ns_sockets/{role}
 namespace-id {role}
 namespace-secret 42
 app-scope-global
 use-mq-eventfd
 rx-fifo-size 4194304
 tx-fifo-size 4194304
}}
""")
        for _ in range(40):
            status = self.docker(RAN, "vppctl", "show", "memif")
            if len(re.findall(r"(?m)^\s*flags .*\bconnected\b", status)) == 2:
                break
            time.sleep(0.25)
        else:
            raise RuntimeError("The two test memif links did not connect")
        if self.args.test_core_zero_copy:
            status = self.core("vpp", "vppctl", "show", "memif")
            for ident in (88, 89):
                marker = f"interface memif{ident}/0"
                if marker not in status:
                    raise RuntimeError(f"Missing test interface memif{ident}/0")
                section = re.split(r"(?m)^interface ", status.split(marker, 1)[1])[0]
                if "zero-copy" not in section:
                    raise RuntimeError(f"Test memif{ident}/0 did not enter zero-copy mode")
        self.cli(["set interface rx-placement memif88/0 queue 0 worker 0",
                  "set interface rx-placement memif89/0 queue 0 worker 1"], fake=True)
        for i in range(self.args.endpoint_count):
            cmd = self.endpoint("dn", self.args.dn_cpu + i * self.args.endpoint_cpu_count, "iperf3", "-s", "-B", DN_IP,
                                "-p", str(5201 + i), "-J", "--logfile", f"/config/server-{5201 + i}.json")
            cmd.insert(2, "-d")
            run(cmd)
        self.snapshot("setup")

    def snapshot(self, label):
        data = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "pdu": http_json(self.args.smf_url),
                "rates": json.loads(self.core("upf", "xcnctl", "show", "rate", "--level", "session", "--json"))}
        for cmd in ("show interface", "show errors", "show runtime", "show threads", "show memif"):
            data["core_" + cmd.replace(" ", "_")] = self.core("vpp", "vppctl", *cmd.split())
        for cmd in ("show interface", "show errors", "show runtime", "show threads"):
            data["ran_" + cmd.replace(" ", "_")] = self.docker(RAN, "vppctl", *cmd.split())
        task_script = '''
for p in /proc/[0-9]*; do
 test -r "$p/comm" || continue
 name=$(cat "$p/comm")
 case "$name" in xcn-upfd|vpp|vpp_main)
  for t in "$p"/task/*; do
   echo "task=${t##*/} comm=$(cat "$t/comm") schedstat=$(cat "$t/schedstat")"
   sed -n '/Cpus_allowed_list:/p' "$t/status"
  done;; esac
done
for f in /sys/fs/cgroup/cpu.stat /sys/fs/cgroup/cpu/cpu.stat /sys/fs/cgroup/cpu/cpu.cfs_quota_us; do
 test ! -f "$f" || { echo "$f"; cat "$f"; }
done
'''
        for container in ("upf", "vpp"):
            data[container + "_tasks"] = self.core(container, "sh", "-c", task_script)
        data["host_interfaces"] = {iface: {
            field: int(pathlib.Path(f"/sys/class/net/{iface}/statistics/{field}").read_text())
            for field in ("rx_bytes", "tx_bytes", "rx_packets", "tx_packets")}
            for iface in ("ens5f0", "ens5f1") if pathlib.Path(f"/sys/class/net/{iface}").exists()}
        write_json(self.path / f"{label}-snapshot.json", data)
        return data

    def measure(self):
        if (self.path / f"{self.args.label}-summary.json").exists():
            raise RuntimeError("Choose a new --label to preserve existing results")
        self.session()
        before = self.snapshot(self.args.label + "-before")
        processes = []
        for i in range(self.args.processes):
            width = self.state.get("args", {}).get("endpoint_cpu_count", 1)
            cmd = self.endpoint("ue", self.args.ue_cpu + i * width, "iperf3",
                   "-c", DN_IP, "-B", self.state["pdu"]["ipv4"], "-p", str(5201 + i),
                   "-t", str(self.args.seconds), "-O", str(self.args.omit), "-J", "--get-server-output",
                   "-P", str(self.args.streams))
            if self.args.direction == "dl":
                cmd += ["-R"]
            cmd[3:3] = ["timeout", "-k", "2", str(self.args.seconds + self.args.omit + 60)]
            process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append((cmd, process))
        outputs = []
        for i, (cmd, process) in enumerate(processes):
            stdout, stderr = process.communicate(timeout=self.args.seconds + self.args.omit + 65)
            (self.path / f"{self.args.label}-{i}.json").write_text(stdout)
            if process.returncode:
                raise RuntimeError(f"iperf failed: {stderr}{stdout}")
            value = json.loads(stdout)
            if "error" in value:
                raise RuntimeError(value["error"])
            outputs.append(value)
        after = self.snapshot(self.args.label + "-after")
        self.session()
        receivers = [v if self.args.direction == "dl" else v["server_output_json"] for v in outputs]
        senders = [v["server_output_json"] if self.args.direction == "dl" else v for v in outputs]
        # Use the JSON produced by the actual receiver in both directions.
        receiver_sums = [v["end"].get("sum_received", v["end"].get("sum")) for v in receivers]
        sender_sums = [v["end"].get("sum_sent", v["end"].get("sum")) for v in senders]
        bps = sum(v["bits_per_second"] for v in receiver_sums)
        lost = sum(v.get("lost_packets", 0) for v in receiver_sums)
        packets = sum(v.get("packets", 0) for v in receiver_sums)
        result = {"label": self.args.label, "mode": self.state["mode"], "args": vars(self.args),
                  "receiver_mbps": bps / 1000000,
                  "lost_percent": 100 * lost / packets if packets else None,
                  "sender_mbps": sum(v["bits_per_second"] for v in sender_sums) / 1000000,
                  "iperf_cpu": [v["end"]["cpu_utilization_percent"] for v in outputs],
                  "session_before": before["rates"], "session_after": after["rates"]}
        write_json(self.path / f"{self.args.label}-summary.json", result)
        print(json.dumps(result, ensure_ascii=False), flush=True)

    def baseline(self, enabled):
        """Temporarily replace the UPF with VPP GTP-U, retaining both test links."""
        if (self.state["mode"] == "bypass") == enabled:
            return
        pdu, upf, gnb = self.state["pdu"], self.state["upf"], self.state["gnb"]
        if enabled:
            self.cli([
                f"ip route del table 10 {upf}/32 via memif2/0",
                f"ip route add table 10 {upf}/32 via local",
                f"create gtpu tunnel src {upf} dst {gnb} teid {pdu['n3']['upf']['teid']} "
                f"tteid {pdu['n3']['gnb']['teid']} encap-vrf-id 10 decap-next ip4 qfi 1",
                "set interface state gtpu_tunnel0 up",
                "set interface unnumbered gtpu_tunnel0 use memif1/0",
                f"ip route add {pdu['ipv4']}/32 via gtpu_tunnel0",
            ])
        else:
            self.cli([
                f"ip route del {pdu['ipv4']}/32 via gtpu_tunnel0",
                f"create gtpu tunnel src {upf} dst {gnb} teid {pdu['n3']['upf']['teid']} encap-vrf-id 10 del",
                f"ip route del table 10 {upf}/32 via local",
                f"ip route add table 10 {upf}/32 via memif2/0",
            ])
        self.state["mode"] = "bypass" if enabled else "core"
        write_json(self.state_file, self.state)
        print("mode:", self.state["mode"])

    def cleanup(self):
        if not self.state:
            return
        if self.state.get("mode") == "bypass":
            self.baseline(False)
        commands = [
            f"ip route del table 10 {self.state['gnb']}/32 via memif88/0",
            "ip route del 198.19.0.0/24 via memif89/0",
            "delete interface memif memif88/0", "delete interface memif memif89/0",
            "delete memif socket id 88", "delete memif socket id 89",
        ]
        # A partially completed setup may lack one route/interface. VPP exec
        # stops on its first CLI error, so clean each owned object separately.
        for command in commands:
            self.cli([command], check=False)
        run(["docker", "rm", "-f", RAN], check=False)
        self.core("vpp", "rm", "-f", "/run/vpp/perf-n3.sock", "/run/vpp/perf-n6.sock", check=False)
        self.state_file.rename(self.path / "state-cleaned.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("setup", "run", "bypass-on", "bypass-off", "snapshot", "cleanup"))
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--namespace", default="xcn")
    parser.add_argument("--pod")
    parser.add_argument("--smf-url", default="http://127.0.0.1:9092/pdu-info")
    parser.add_argument("--vpp-image", default="vpp:26.06")
    parser.add_argument("--iperf-binary", help="Host path to a compatible iperf3 >= 3.11 binary")
    parser.add_argument("--ran-main-cpu", type=int, default=60)
    parser.add_argument("--ran-worker-cpus", default="61,62,63")
    parser.add_argument("--ue-cpu", type=int, default=64)
    parser.add_argument("--dn-cpu", type=int, default=68)
    parser.add_argument("--endpoint-count", type=int, choices=range(1, 17), default=4)
    parser.add_argument("--endpoint-cpu-count", type=int, choices=range(1, 9), default=1)
    parser.add_argument("--test-core-zero-copy", action="store_true",
                        help="Use slave/zero-copy only on the two added core-side test memifs")
    parser.add_argument("--label", default="measurement")
    parser.add_argument("--direction", choices=("ul", "dl"), default="ul")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--omit", type=int, default=3)
    parser.add_argument("--streams", type=int, default=1)
    parser.add_argument("--processes", type=int, choices=range(1, 17), default=1)
    args = parser.parse_args()
    if args.seconds < 1 or args.omit < 0 or args.streams < 1:
        parser.error("Invalid duration or stream count")
    bench = Bench(args)
    if args.action == "run" and args.processes > bench.state.get("args", {}).get("endpoint_count", 4):
        parser.error("More processes requested than servers created at setup")
    if args.action == "setup":
        bench.setup()
    elif not bench.state:
        parser.error("Run setup first")
    elif args.action == "run":
        bench.measure()
    elif args.action == "snapshot":
        bench.snapshot(args.label)
    elif args.action == "cleanup":
        bench.cleanup()
    else:
        bench.baseline(args.action == "bypass-on")


if __name__ == "__main__":
    main()
