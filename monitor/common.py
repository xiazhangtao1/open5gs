import json
import os
from pathlib import Path

DEFAULTS = dict(enabled=True, trace_enabled=False, events_interval=2, rates_interval=2, info_interval=10,
                gnbs_interval=15, metrics_interval=15, pods_interval=30)


def config():
    """Mounted ConfigMap updates are picked up without restarting core processes."""
    result = DEFAULTS.copy()
    path = os.getenv("MONITOR_CONFIG", "/etc/xcn-monitor/config.json")
    try:
        data = json.loads(Path(path).read_text())
        if not isinstance(data, dict):
            raise ValueError("configuration must be an object")
        for flag in ("enabled", "trace_enabled"):
            if type(data.get(flag)) is bool:
                result[flag] = data[flag]
        for key, minimum in DEFAULTS.items():
            if key not in ("enabled", "trace_enabled") and type(data.get(key)) in (int, float):
                result[key] = min(300, max(minimum, data[key]))
    except FileNotFoundError:
        pass
    except (OSError, ValueError, TypeError):
        result["enabled"] = False  # Bad configuration stops collection safely.
    return result


def cpu_list(value):
    cpus = set()
    for part in value.split(","):
        if not part.strip():
            continue
        ends = [int(x) for x in part.split("-")]
        if len(ends) not in (1, 2) or min(ends) < 0 or max(ends) > 65535:
            raise ValueError("invalid CPU set")
        cpus.update(range(ends[0], ends[-1] + 1))
    return cpus


def siblings(cpus):
    result = set(cpus)
    for cpu in cpus:
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        result.update(cpu_list(path.read_text().strip()))
    return result


def pin_cpu(reserved=()):
    requested = cpu_list(os.getenv("MONITOR_CPUSET", ""))
    required = os.getenv("MONITOR_REQUIRE_AFFINITY", "1") == "1"
    if not requested and required:
        raise ValueError("MONITOR_CPUSET 必须指定独立 CPU（避开 UPF/VPP 及其 SMT 兄弟）")
    if requested:
        if requested & siblings(set(reserved)):
            raise ValueError("监控 CPU 与 UPF/VPP 或其 SMT 兄弟重叠，已停止采集")
        if not requested <= os.sched_getaffinity(0):
            raise ValueError("监控 CPU 不在容器允许的 CPU 集合中")
        os.sched_setaffinity(0, requested)
    return sorted(os.sched_getaffinity(0))


async def read_limited(response, maximum, budget=None):
    response.raise_for_status()
    data = bytearray()
    if budget and response.content_length:
        budget.before_json(min(response.content_length, maximum))
    async for chunk in response.content.iter_chunked(65536):
        size = len(data) + len(chunk)
        if size > maximum:
            raise ValueError("source response exceeds byte budget")
        if budget:
            budget.before_json(size)
        data.extend(chunk)
    return bytes(data)
