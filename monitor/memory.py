"""Admission control for decoded batches, clones and container memory pressure."""
import gc
import os
from pathlib import Path
import sys
import time

MIB = 1024 * 1024
UPF_BODY_LIMIT = 4 * MIB


class MemoryPressure(RuntimeError):
    pass


def container_bytes(value):
    """Upper bound for cloning JSON containers; deepcopy shares immutable values.

    Inputs are acyclic JSON trees. Repeated containers count again deliberately;
    no large visited-object set is allocated while memory is already scarce.
    """
    stack, total = [value], 0
    while stack:
        item = stack.pop()
        if isinstance(item, dict):
            total += sys.getsizeof(item)
            stack.extend(v for v in item.values() if isinstance(v, (dict, list)))
        elif isinstance(item, list):
            total += sys.getsizeof(item)
            stack.extend(v for v in item if isinstance(v, (dict, list)))
    return total


class MemoryBudget:
    def __init__(self, limit=None, usage=None):
        self.limit = limit or int(os.getenv("MONITOR_MEMORY_LIMIT_BYTES", str(512 * MIB)))
        self.current_file = None
        for limit_file, current_file in (("/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory.current"),
                ("/sys/fs/cgroup/memory/memory.limit_in_bytes", "/sys/fs/cgroup/memory/memory.usage_in_bytes")):
            try:
                enforced = int(Path(limit_file).read_text().strip())
                if 0 < enforced < (1 << 60):
                    self.limit = min(self.limit, enforced)
                    self.current_file = Path(current_file)
                    break
            except (OSError, ValueError):
                pass
        if self.limit <= 0:
            raise ValueError("invalid monitor memory limit")
        self.high = self.limit * 70 // 100
        self.low = self.limit * 55 // 100
        self.usage = usage
        self.used = 0
        self.measured_at = 0
        self.paused = False
        self.skipped = 0
        self.reclaims = 0
        self.last_reason = None

    def sample(self, force=False):
        if not force and time.monotonic() - self.measured_at < 1:
            return self.used
        self.measured_at = time.monotonic()
        if self.usage:
            self.used = self.usage()
        else:
            self.used = int(Path('/proc/self/statm').read_text().split()[1]) * os.sysconf('SC_PAGE_SIZE')
            if self.current_file:
                try:
                    self.used = max(self.used, int(self.current_file.read_text().strip()))
                except (OSError, ValueError):
                    pass
        if self.used >= self.high:
            self.paused = True
        elif self.used <= self.low:
            self.paused = False
        return self.used

    def check(self, reserve=0, operation="collection"):
        used = self.sample(force=True)
        if self.paused or used + reserve > self.high:
            self.skipped += 1
            self.last_reason = f"内存保护：{operation}；使用 {used // MIB} MiB，预留 {reserve // MIB} MiB，水位 {self.high // MIB} MiB"
            raise MemoryPressure(self.last_reason)

    def before_json(self, size):
        # JSON objects can be many times larger than their wire representation.
        # Include the bytes buffer, parser temporaries and subsequent derived views.
        self.check(max(8 * MIB, size * 32), "新批次超过可用解析预算")

    def status(self):
        return dict(limit_bytes=self.limit, used_bytes=self.sample(), high_bytes=self.high,
                    resume_bytes=self.low, paused=self.paused, skipped_batches=self.skipped,
                    reclaims=self.reclaims, last_reason=self.last_reason)

    def trim_allocator(self):
        gc.collect()
        # Best effort; correctness never depends on the allocator returning pages.
        try:
            import ctypes
            ctypes.CDLL(None).malloc_trim(0)
        except (OSError, AttributeError):
            pass
