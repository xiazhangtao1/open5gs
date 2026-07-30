#!/usr/bin/env python3
"""Render a NUMA-aware two-VF VPP configuration and supervise VPP affinity."""

import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


CPUSET_FILES = (
    Path("/sys/fs/cgroup/cpuset.cpus.effective"),
    Path("/sys/fs/cgroup/cpuset/cpuset.cpus"),
)
PCI_RE = re.compile(r"^(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$")
TOKEN_RE = re.compile(r"{{[A-Z0-9_]+}}")
TOPOLOGY_ROOT = Path("/sys/devices/system/cpu")


class ConfigError(RuntimeError):
    pass


def parse_cpuset(value):
    cpus = set()
    for item in value.strip().split(","):
        if not item:
            continue
        if "-" in item:
            first, last = (int(part) for part in item.split("-", 1))
            if first > last:
                raise ConfigError(f"invalid CPU range: {item}")
            cpus.update(range(first, last + 1))
        else:
            cpus.add(int(item))
    if not cpus:
        raise ConfigError("empty cpuset")
    return sorted(cpus)


def read_allowed_cpus():
    for path in CPUSET_FILES:
        try:
            value = path.read_text(encoding="ascii").strip()
        except OSError:
            continue
        if value:
            return parse_cpuset(value), str(path)
    for line in Path("/proc/self/status").read_text(encoding="ascii").splitlines():
        if line.startswith("Cpus_allowed_list:"):
            return parse_cpuset(line.split(":", 1)[1]), "/proc/self/status"
    raise ConfigError("cannot find container cpuset")


def allocated_cpus(limit):
    cpus, source = read_allowed_cpus()
    if len(cpus) == limit:
        print(f"CPU allocation ready from {source}: {cpus}", flush=True)
        return cpus

    isocpu = os.environ.get("ISOCPU", "").strip()
    if not isocpu.startswith("cpu="):
        raise ConfigError(
            f"expected {limit} CPUs, {source} exposes {len(cpus)} and ISOCPU is missing"
        )
    assigned = parse_cpuset(isocpu.removeprefix("cpu="))
    if len(assigned) != limit:
        raise ConfigError(
            f"expected {limit} CPUs, ISOCPU assigns {len(assigned)}: {assigned}"
        )
    if not set(assigned).issubset(cpus):
        raise ConfigError(f"ISOCPU {assigned} is outside allowed CPUs from {source}: {cpus}")
    print(
        f"CPU cgroup exposes {len(cpus)} CPUs; using webhook ISOCPU allocation: {assigned}",
        flush=True,
    )
    return assigned


def parse_pcis(value):
    devices = sorted(item.strip().lower() for item in (value or "").split(",") if item.strip())
    if len(devices) != 2 or len(set(devices)) != 2:
        raise ConfigError("exactly two distinct external_network PCI devices are required")
    normalized = []
    for device in devices:
        if not PCI_RE.fullmatch(device):
            raise ConfigError(f"invalid PCI address: {device}")
        normalized.append(device if device.count(":") == 2 else "0000:" + device)
    return normalized


def required(name):
    value = os.environ.get(name, "").strip()
    if not value:
        raise ConfigError(f"{name} is required")
    return value


def positive_integer(name, minimum=1):
    value = required(name)
    if not value.isdigit() or int(value) < minimum:
        raise ConfigError(f"{name} must be an integer of at least {minimum}")
    return int(value)


def cpu_core_groups(cpus, topology_root=TOPOLOGY_ROOT):
    groups = {}
    for cpu in cpus:
        topology = topology_root / f"cpu{cpu}" / "topology"
        try:
            package = int((topology / "physical_package_id").read_text(
                encoding="ascii").strip())
            core = int((topology / "core_id").read_text(
                encoding="ascii").strip())
        except (OSError, ValueError) as exc:
            raise ConfigError(f"cannot read topology for CPU {cpu}: {exc}") from exc
        groups.setdefault((package, core), []).append(cpu)
    return [
        sorted(group) for _, group in sorted(
            groups.items(), key=lambda item: min(item[1]))
    ]


def affinity_layouts(cpus, worker_count):
    thread_count = worker_count + 1
    groups = cpu_core_groups(cpus)
    if len(groups) < thread_count:
        raise ConfigError(
            f"{thread_count} VPP busy threads require distinct physical cores, "
            f"but CPU allocation {cpus} contains only {len(groups)} cores")

    isolated = [group[0] for group in groups[:thread_count]]
    dense = [cpu for group in groups for cpu in group][:thread_count]
    if len(dense) != thread_count:
        raise ConfigError(
            f"CPU allocation {cpus} cannot place {thread_count} VPP busy threads")
    return groups, isolated, dense


def atomic_write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(value)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def render(source, destination, replacements):
    text = source.read_text(encoding="utf-8")
    for token, value in replacements.items():
        marker = "{{" + token + "}}"
        if marker not in text:
            raise ConfigError(f"{source.name}: missing {marker}")
        text = text.replace(marker, value)
    unresolved = TOKEN_RE.findall(text)
    if unresolved:
        raise ConfigError(f"{source.name}: unresolved tokens: {unresolved}")

    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=destination.name + ".", dir=destination.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def vpp_tasks(pid):
    tasks = {}
    task_root = Path(f"/proc/{pid}/task")
    try:
        task_paths = list(task_root.iterdir())
    except OSError:
        return tasks
    for task_path in task_paths:
        try:
            name = (task_path / "comm").read_text(
                encoding="ascii").strip()
        except OSError:
            continue
        tasks[name] = int(task_path.name)
    return tasks


def apply_affinity(pid, mode, layouts, worker_count, status_path):
    tasks = vpp_tasks(pid)
    names = ["vpp_main"] + [f"vpp_wk_{index}" for index in range(worker_count)]
    missing = [name for name in names if name not in tasks]
    if missing:
        return False

    changed = False
    assignments = {}
    for name, cpu in zip(names, layouts[mode]):
        tid = tasks[name]
        current = os.sched_getaffinity(tid)
        if current != {cpu}:
            os.sched_setaffinity(tid, {cpu})
            changed = True
        assignments[name] = {"tid": tid, "cpu": cpu}

    status = "mode={} ".format(mode) + " ".join(
        f"{name}={item['tid']}:{item['cpu']}"
        for name, item in assignments.items()) + "\n"
    try:
        previous_status = status_path.read_text(encoding="ascii")
    except OSError:
        previous_status = ""
    if previous_status != status:
        atomic_write(status_path, status)
    if changed:
        print(f"VPP affinity applied: {status.strip()}", flush=True)
    return True


def supervise_vpp(command, layouts, worker_count, mode_path, status_path,
                  poll_interval):
    process = subprocess.Popen(command)

    def forward_signal(signum, _frame):
        if process.poll() is None:
            process.send_signal(signum)

    signal.signal(signal.SIGTERM, forward_signal)
    signal.signal(signal.SIGINT, forward_signal)

    while process.poll() is None:
        try:
            mode = mode_path.read_text(encoding="ascii").strip()
        except OSError:
            mode = "isolated"
        if mode not in layouts:
            print(
                f"ignoring invalid VPP affinity mode {mode!r}; "
                "expected dense or isolated",
                file=sys.stderr, flush=True)
            mode = "isolated"
            atomic_write(mode_path, mode + "\n")
        try:
            apply_affinity(
                process.pid, mode, layouts, worker_count, status_path)
        except (OSError, ValueError) as exc:
            print(f"cannot apply VPP affinity: {exc}", file=sys.stderr, flush=True)
        time.sleep(poll_interval)

    return process.wait()


def main():
    try:
        cpu_limit = positive_integer("VPP_CPU_LIMIT", 2)
        worker_count = positive_integer("VPP_CPU_WORKERS")
        if worker_count >= cpu_limit:
            raise ConfigError(
                f"VPP_CPU_WORKERS {worker_count} must be less than "
                f"VPP_CPU_LIMIT {cpu_limit}")
        cpus = allocated_cpus(cpu_limit)
        groups, isolated, dense = affinity_layouts(cpus, worker_count)
        pci_device_env = os.environ.get(
            "VPP_PCI_DEVICE_ENV", "PCIDEVICE_INTEL_COM_EXTERNAL_NETWORK")
        pci_n3, pci_n6 = parse_pcis(required(pci_device_env))
        cpu_config = (
            f"    main-core {isolated[0]}\n"
            f"    corelist-workers {','.join(map(str, isolated[1:]))}")

        replacements = {
            "CPU_CONFIG": cpu_config,
            "PCI_N3": pci_n3,
            "PCI_N6": pci_n6,
            "N3_INTERFACE_ADDRESS": required("VPP_N3_INTERFACE_ADDRESS"),
            "N3_DEFAULT_GATEWAY": required("VPP_N3_DEFAULT_GATEWAY"),
            "N3_UPF_ADDRESS": required("VPP_N3_UPF_ADDRESS"),
            "N6_INTERFACE_ADDRESS": required("VPP_N6_INTERFACE_ADDRESS"),
            "N6_DEFAULT_GATEWAY": required("VPP_N6_DEFAULT_GATEWAY"),
        }
        template_dir = Path(os.environ.get("VPP_TEMPLATE_DIR", "/etc/vpp/templates"))
        output_dir = Path(os.environ.get("VPP_CONFIG_DIR", "/run/vpp/config"))
        render(template_dir / "startup.conf.template", output_dir / "startup.conf",
               {key: replacements[key] for key in ("CPU_CONFIG", "PCI_N3", "PCI_N6")})
        render(template_dir / "cli-commands.conf.template", output_dir / "cli-commands.conf",
               {key: replacements[key] for key in replacements if key != "CPU_CONFIG" and not key.startswith("PCI_")})
        render(template_dir / "vcl.conf.template", output_dir / "vcl.conf", {})

        mode_path = Path(os.environ.get(
            "VPP_AFFINITY_MODE_FILE", "/run/vpp/affinity-mode"))
        status_path = Path(os.environ.get(
            "VPP_AFFINITY_STATUS_FILE", "/run/vpp/affinity-status"))
        plan_path = Path(os.environ.get(
            "VPP_AFFINITY_PLAN_FILE", "/run/vpp/affinity-plan.json"))
        poll_ms = positive_integer("VPP_AFFINITY_POLL_MS", 50)
        layouts = {"isolated": isolated, "dense": dense}
        atomic_write(mode_path, "isolated\n")
        atomic_write(plan_path, json.dumps({
            "cpuset": cpus,
            "physical_core_groups": groups,
            "worker_count": worker_count,
            "layouts": layouts,
        }, sort_keys=True) + "\n")

        print(
            f"starting VPP with N3 {pci_n3}, N6 {pci_n6}, cpuset {cpus}, "
            f"workers {worker_count}, isolated CPUs {isolated}",
            flush=True)
        return supervise_vpp(
            ("vpp", "-c", str(output_dir / "startup.conf")),
            layouts, worker_count, mode_path, status_path, poll_ms / 1000)
    except (ConfigError, OSError, ValueError) as exc:
        print(f"vpp-entrypoint-dual: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
