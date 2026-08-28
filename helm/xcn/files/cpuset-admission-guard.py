#!/usr/bin/env python3
"""Recreate a 5GC Pod when CPUSet admission mutation was skipped."""

import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


TOKEN_FILE = Path("/var/run/secrets/xcn-guard/token")
CA_FILE = Path("/var/run/secrets/xcn-guard/ca.crt")


class GuardError(RuntimeError):
    pass


def non_negative_integer(name, minimum=0):
    value = os.environ.get(name, "").strip()
    if not value.isdigit() or int(value) < minimum:
        raise GuardError(f"{name} must be an integer of at least {minimum}")
    return int(value)


def parse_cpuset(value):
    cpus = set()
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            first, last = (int(part) for part in item.split("-", 1))
            if first > last:
                raise ValueError(f"invalid CPU range: {item}")
            cpus.update(range(first, last + 1))
        else:
            cpus.add(int(item))
    if not cpus:
        raise ValueError("empty CPU set")
    return cpus


def allocation_ready(value, required_containers):
    try:
        allocation = json.loads(value)
        if not isinstance(allocation, dict):
            return False
        for container in required_containers:
            entry = allocation.get(container)
            if not isinstance(entry, dict):
                return False
            parse_cpuset(entry.get("cpu", ""))
    except (TypeError, ValueError, json.JSONDecodeError):
        return False
    return True


def pod_url(host, port, namespace, name):
    address = f"[{host}]" if ":" in host else host
    namespace = urllib.parse.quote(namespace, safe="")
    name = urllib.parse.quote(name, safe="")
    return f"https://{address}:{port}/api/v1/namespaces/{namespace}/pods/{name}"


def delete_pod(url, token, ca_file, timeout):
    request = urllib.request.Request(
        url,
        data=json.dumps({
            "apiVersion": "v1",
            "kind": "DeleteOptions",
            "gracePeriodSeconds": 0,
        }).encode("ascii"),
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/json",
        },
        method="DELETE",
    )
    context = ssl.create_default_context(cafile=str(ca_file))
    try:
        with urllib.request.urlopen(
                request, context=context, timeout=timeout) as response:
            if response.status not in (200, 202):
                raise GuardError(
                    f"Kubernetes API returned HTTP {response.status}")
    except urllib.error.HTTPError as exc:
        if exc.code != 404:
            raise GuardError(
                f"Kubernetes API returned HTTP {exc.code}") from exc


def required(name):
    value = os.environ.get(name, "").strip()
    if not value:
        raise GuardError(f"{name} is required")
    return value


def main():
    try:
        allocation = os.environ.get("CPUSET_ALLOCATION", "").strip()
        required_containers = [
            item.strip() for item in required(
                "CPUSET_REQUIRED_CONTAINERS").split(",") if item.strip()
        ]
        if not required_containers:
            raise GuardError("CPUSET_REQUIRED_CONTAINERS must not be empty")
        if allocation_ready(allocation, required_containers):
            print(
                "CPUSet admission allocation is present for "
                + ", ".join(required_containers), flush=True)
            return 0

        delay = non_negative_integer("CPUSET_GUARD_DELETE_DELAY_SECONDS")
        timeout = non_negative_integer(
            "CPUSET_GUARD_API_TIMEOUT_SECONDS", minimum=1)
        pod_name = required("POD_NAME")
        namespace = required("POD_NAMESPACE")
        host = required("KUBERNETES_SERVICE_HOST")
        port = required("KUBERNETES_SERVICE_PORT_HTTPS")
        print(
            f"CPUSet admission allocation is missing or invalid; "
            f"deleting Pod {namespace}/{pod_name} after {delay}s",
            file=sys.stderr, flush=True)
        time.sleep(delay)
        token = TOKEN_FILE.read_text(encoding="ascii").strip()
        delete_pod(
            pod_url(host, port, namespace, pod_name),
            token, CA_FILE, timeout)
        print(
            f"Pod deletion accepted for {namespace}/{pod_name}; "
            "waiting for termination", flush=True)
        while True:
            time.sleep(3600)
    except (GuardError, OSError, ValueError, urllib.error.URLError) as exc:
        print(f"cpuset-admission-guard: {exc}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
