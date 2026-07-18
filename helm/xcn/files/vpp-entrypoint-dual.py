#!/usr/bin/env python3
"""Render a NUMA-aware two-VF VPP configuration and start VPP."""

import os
import re
import sys
import tempfile
from pathlib import Path


CPUSET_FILES = (
    Path("/sys/fs/cgroup/cpuset.cpus.effective"),
    Path("/sys/fs/cgroup/cpuset/cpuset.cpus"),
)
PCI_RE = re.compile(r"^(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$")
TOKEN_RE = re.compile(r"{{[A-Z0-9_]+}}")


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


def main():
    try:
        limit_text = required("VPP_CPU_LIMIT")
        if not limit_text.isdigit() or int(limit_text) < 2:
            raise ConfigError("VPP_CPU_LIMIT must be an integer of at least 2")
        cpus = allocated_cpus(int(limit_text))
        pci_n3, pci_n6 = parse_pcis(required("PCIDEVICE_INTEL_COM_EXTERNAL_NETWORK"))
        cpu_config = f"    main-core {cpus[0]}\n    corelist-workers {','.join(map(str, cpus[1:]))}"

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

        print(f"starting VPP with N3 {pci_n3}, N6 {pci_n6}, CPUs {cpus}", flush=True)
        os.execvp("vpp", ("vpp", "-c", str(output_dir / "startup.conf")))
    except (ConfigError, OSError, ValueError) as exc:
        print(f"vpp-entrypoint-dual: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
