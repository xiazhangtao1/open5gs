#!/usr/bin/env python3
"""Sample core threads/cgroups from host /proc while running a benchmark command.

Only reads the current UPF/VPP processes. CPU samples cover warm-up, traffic,
and teardown; use iperf timestamps to select the steady traffic window.
"""

import argparse
import datetime
import json
import pathlib
import subprocess
import threading
import time

from iperf_core import cpus, run, write_json


def process_paths(pod):
    ids = {x["name"]: x["containerID"].split("://")[-1]
           for x in pod["status"]["containerStatuses"] if x["name"] in ("upf", "vpp")}
    found = {}
    for proc in pathlib.Path("/proc").glob("[0-9]*"):
        try:
            name = (proc / "comm").read_text().strip()
            if name not in ("xcn-upfd", "vpp_main", "vpp"):
                continue
            cgroup = (proc / "cgroup").read_text()
            for role, cid in ids.items():
                if cid in cgroup:
                    paths = {ctrl: path for line in cgroup.splitlines()
                             for _, controls, path in [line.split(":", 2)]
                             for ctrl in controls.split(",")}
                    found[role] = {"pid": int(proc.name), "proc": str(proc),
                                   "cpu": "/sys/fs/cgroup/cpu" + paths["cpu"],
                                   "cpuset": "/sys/fs/cgroup/cpuset" + paths["cpuset"],
                                   "memory": "/sys/fs/cgroup/memory" + paths["memory"]}
        except (FileNotFoundError, PermissionError):
            continue
    if set(found) != {"upf", "vpp"}:
        raise RuntimeError(f"Cannot resolve both core processes: {found}")
    return found


def read_text(path):
    try:
        return pathlib.Path(path).read_text().strip()
    except (FileNotFoundError, ProcessLookupError):
        return None


def snapshot(paths):
    data = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "wall_seconds": time.time(), "monotonic_ns": time.monotonic_ns(), "roles": {}}
    for role, entry in paths.items():
        threads = {}
        for task in pathlib.Path(entry["proc"], "task").glob("[0-9]*"):
            try:
                sched = list(map(int, (task / "schedstat").read_text().split()))
                status = dict(line.split(":", 1) for line in (task / "status").read_text().splitlines())
                threads[task.name] = {"comm": (task / "comm").read_text().strip(),
                                     "runtime_ns": sched[0], "runqueue_wait_ns": sched[1],
                                     "slices": sched[2],
                                     "affinity": status["Cpus_allowed_list"].strip(),
                                     "voluntary": int(status["voluntary_ctxt_switches"]),
                                     "involuntary": int(status["nonvoluntary_ctxt_switches"])}
            except (FileNotFoundError, ProcessLookupError):
                continue
        data["roles"][role] = {"threads": threads,
            "cpu_stat": read_text(entry["cpu"] + "/cpu.stat"),
            "cpu_quota_us": read_text(entry["cpu"] + "/cpu.cfs_quota_us"),
            "cpuset": read_text(entry["cpuset"] + "/cpuset.cpus"),
            "memory_bytes": read_text(entry["memory"] + "/memory.usage_in_bytes"),
            "memory_failcnt": read_text(entry["memory"] + "/memory.failcnt")}
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--namespace", default="xcn")
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--profile", action="store_true",
                        help="Record cycles at 99 Hz during seconds 8..25; use a >=30s test")
    parser.add_argument("--observer", default="xcn-iperf-observer",
                        help="Privileged host-PID helper with /perf and a writable /out mount")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("A benchmark command is required after --")
    pods = json.loads(run(["kubectl", "-n", args.namespace, "get", "pods",
                           "-l", "app=xcn-5gc", "-o", "json"]))["items"]
    if len(pods) != 1:
        parser.error("Exactly one core Pod is required")
    paths = process_paths(pods[0])
    directory = pathlib.Path(args.work_dir)
    directory.mkdir(parents=True, exist_ok=True)
    stem = directory / args.label
    metadata = pathlib.Path(str(stem) + "-diag-metadata.json")
    if metadata.exists():
        parser.error("Choose a new label")
    allocated = set()
    for entry in paths.values():
        allocated.update(cpus(read_text(entry["cpuset"] + "/cpuset.cpus")))
    topology = {str(cpu): {"siblings": read_text(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list"),
                           "core_id": read_text(f"/sys/devices/system/cpu/cpu{cpu}/topology/core_id"),
                           "socket": read_text(f"/sys/devices/system/cpu/cpu{cpu}/topology/physical_package_id")}
                for cpu in sorted(allocated)}
    write_json(metadata, {"pod": pods[0]["metadata"]["name"], "uid": pods[0]["metadata"]["uid"],
                          "paths": paths, "topology": topology, "command": command})
    done = threading.Event()
    errors = []

    def collect():
        with pathlib.Path(str(stem) + "-metrics.jsonl").open("x") as output:
            while True:
                try:
                    output.write(json.dumps(snapshot(paths)) + "\n")
                    output.flush()
                except Exception as error:
                    errors.append(str(error))
                    return
                if done.wait(1):
                    return

    worker = threading.Thread(target=collect)
    worker.start()
    profile = None
    profile_log = None
    try:
        if args.profile:
            mounts = json.loads(run(["docker", "inspect", args.observer]))[0]["Mounts"]
            root = pathlib.Path(next(m["Source"] for m in mounts if m["Destination"] == "/out"))
            relative = directory.resolve().relative_to(root.resolve())
            filename = f"/out/{relative}/{args.label}-perf.data"
            tids = [task.name for entry in paths.values()
                    for task in pathlib.Path(entry["proc"], "task").glob("[0-9]*")]
            profile_log = pathlib.Path(str(stem) + "-perf.log").open("x")
            profile = subprocess.Popen(["docker", "exec", args.observer, "/perf", "record",
                                       "-D", "8000", "-e", "cycles:u", "-F", "99",
                                       "-t", ",".join(tids), "-o", filename, "--", "sleep", "25"],
                                      stdout=profile_log, stderr=subprocess.STDOUT)
        with pathlib.Path(str(stem) + "-command.log").open("x") as output:
            result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
        if profile:
            if profile.wait(timeout=30):
                raise RuntimeError("perf record failed; see the perf log")
            for role, entry in paths.items():
                comm = "xcn-upfd" if role == "upf" else "vpp_wk_0"
                report = run(["docker", "exec", args.observer, "/perf", "report", "--stdio",
                              "--no-children", "--percent-limit", "0.5", "--percentage", "relative", "--comms", comm,
                              "--symfs", entry["proc"] + "/root", "-i", filename])
                pathlib.Path(str(stem) + f"-{role}-perf.txt").write_text(report)
    finally:
        done.set()
        worker.join()
        if profile_log:
            profile_log.close()
    if errors:
        raise RuntimeError("; ".join(errors))
    if result.returncode:
        raise RuntimeError(f"Benchmark failed; see {stem}-command.log")
    summary_path = pathlib.Path(str(stem) + "-summary.json")
    if summary_path.exists():
        value = json.loads(summary_path.read_text())
        print(json.dumps({k: value.get(k) for k in ("label", "receiver_mbps", "sender_mbps", "lost_percent")}))


if __name__ == "__main__":
    main()
