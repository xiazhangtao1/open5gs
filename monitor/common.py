import json
import logging
import os
from pathlib import Path
import re
import time

DEFAULTS = dict(enabled=True, trace_enabled=False, events_interval=2, rates_interval=2, info_interval=10,
                gnbs_interval=15, metrics_interval=15, pods_interval=30)


def trace_redact_keys():
    value = os.getenv("TRACE_REDACT_KEYS", "true").lower()
    if value not in ("true", "false"):
        raise ValueError("TRACE_REDACT_KEYS must be true or false")
    return value == "true"


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
    if not value.strip():
        return cpus
    for part in value.split(","):
        if not re.fullmatch(r"\d+(?:-\d+)?", part.strip()):
            raise ValueError("invalid CPU set")
        ends = [int(x) for x in part.split("-")]
        if ends[0] > ends[-1] or ends[-1] > 65535:
            raise ValueError("invalid CPU set")
        cpus.update(range(ends[0], ends[-1] + 1))
    return cpus


def siblings(cpus):
    result = set(cpus)
    for cpu in cpus:
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        result.update(cpu_list(path.read_text().strip()))
    return result


class AllocationPending(ValueError):
    pass


def container_cpus():
    """Read the kernel's current allocation, including whole-tree cgroup mounts."""
    memberships = {}
    for line in Path('/proc/self/cgroup').read_text().splitlines():
        _, controllers, member = line.split(':', 2)
        if '..' in Path(member).parts:
            raise ValueError('invalid cgroup membership')
        for controller in controllers.split(','):
            memberships[controller] = member.lstrip('/')
    for root, controller, filenames in (
            (Path('/sys/fs/cgroup'), '', ('cpuset.cpus.effective',)),
            (Path('/sys/fs/cgroup/cpuset'), 'cpuset', ('cpuset.effective_cpus', 'cpuset.cpus'))):
        member = memberships.get(controller)
        # Prefer our own leaf when the runtime exposes the complete cgroup tree.
        bases = [root / member, root] if member else [root]
        for base in bases:
            for filename in filenames:
                try:
                    cpus = cpu_list((base / filename).read_text().strip())
                    if cpus:
                        return cpus
                except FileNotFoundError:
                    pass
    raise AllocationPending('无法读取容器实际 cpuset，等待 CPU Manager 分配')


def allocated_cpus():
    raw = os.getenv('MONITOR_CPU_LIMIT_MILLICORES', '')
    if not re.fullmatch(r'[1-9][0-9]{0,7}', raw) or int(raw) > 65536000:
        raise ValueError('MONITOR_CPU_LIMIT_MILLICORES must be a positive CPU limit')
    milli = int(raw)
    actual = container_cpus()
    if milli % 1000 == 0:
        count = milli // 1000
        if len(actual) != count:
            raise AllocationPending(f'等待 CPU Manager 分配 {count} 个逻辑 CPU，当前 cpuset 为 {len(actual)} 个')
        hint = os.getenv('ISOCPU', '').strip()
        if hint:
            if not hint.startswith('cpu=') or not cpu_list(hint[4:]):
                raise ValueError('invalid CPU Manager ISOCPU allocation')
            if actual != cpu_list(hint[4:]):
                raise AllocationPending('容器 cpuset 尚未与 CPU Manager 的 ISOCPU 分配一致')
    else:
        online = cpu_list(Path('/sys/devices/system/cpu/online').read_text().strip())
        if not online or not actual <= online:
            raise AllocationPending('容器 cpuset 尚未与在线 CPU 集合一致')
        if actual == online:
            raise AllocationPending('共享容器仍可使用全部 CPU，等待 CPU Manager 的共享集合')
    return actual


def pin_cpu(reserved=()):
    requested = cpu_list(os.getenv("MONITOR_CPUSET", ""))
    required = os.getenv("MONITOR_REQUIRE_AFFINITY", "1") == "1"
    if not requested and required:
        actual = allocated_cpus()
        if actual & siblings(set(reserved)):
            raise AllocationPending('容器实际 CPU 与核心网工作核或其 SMT 兄弟重叠，等待 CPU Manager')
        # Do not pin to a snapshot of the shared pool: the manager can change it.
        # The kernel applies the current container cpuset to every thread/child.
        return sorted(actual)
    if requested:
        if requested & siblings(set(reserved)):
            raise ValueError("监控 CPU 与 UPF/VPP 或其 SMT 兄弟重叠，已停止采集")
        if not requested <= os.sched_getaffinity(0):
            raise ValueError("监控 CPU 不在容器允许的 CPU 集合中")
        os.sched_setaffinity(0, requested)
    return sorted(os.sched_getaffinity(0))


def wait_for_cpu():
    raw = os.getenv('MONITOR_CPUSET_WAIT_SECONDS', '120')
    if not re.fullmatch(r'\d{1,3}', raw) or int(raw) > 600:
        raise ValueError('MONITOR_CPUSET_WAIT_SECONDS must be between 0 and 600')
    deadline, next_log = time.monotonic() + int(raw), 0
    while True:
        try:
            return pin_cpu()
        except AllocationPending as exc:
            now = time.monotonic()
            if now >= deadline:
                raise ValueError(f'CPU 分配等待超时（{raw} 秒）：{exc}') from exc
            if now >= next_log:
                logging.warning('%s', exc)
                next_log = now + 5
            time.sleep(min(.25, deadline - now))


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
