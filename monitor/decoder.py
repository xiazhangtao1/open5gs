"""Isolated, bounded TShark decoding with an explicit deployment disclosure policy."""
import asyncio
import contextlib
import hmac
import io
import json
import os
import re
import struct
import time

import aiohttp
from aiohttp import web
from defusedxml import ElementTree as ET
from common import pin_cpu, read_limited, trace_redact_keys, wait_for_cpu

VERSION = "tshark-4.6.9/xcn-tree-3"
MAX_OUTPUT = 4 * 1024 * 1024
SECRET = re.compile(r"(?:securitykey|nexthopnh|kgnb|kamf|knas|kasme)", re.I)


def upper_pdu(data, kind):
    # Wireshark exported-PDU option 12 = registered dissector name; network byte order.
    name = (b"ngap\0" if kind == 1 else b"nas-5gs\0")
    name += b"\0" * (-len(name) % 4)
    prefix = struct.pack("!HH", 12, len(name)) + name + b"\0" * 4
    frame = prefix + data
    # Classic PCAP, microseconds, DLT_WIRESHARK_UPPER_PDU = 252. No fake network headers.
    pcap = struct.pack("<IHHIIII", 0xa1b2c3d4, 2, 4, 0, 0, 65536, 252)
    pcap += struct.pack("<IIII", 0, 0, len(frame), len(frame)) + frame
    return pcap, len(prefix)


def decoder_version(redact_keys):
    return VERSION + ("/masked" if redact_keys else "/full")


def protocol_tree(xml, data, prefix, redact_keys=True):
    stack, roots, secrets, count = [], [], [], 0
    malformed = False
    uncertain = False
    for action, element in ET.iterparse(io.BytesIO(xml), events=("start", "end"), forbid_dtd=True):
        if action == "start":
            count += 1
            if count > 10000 or len(stack) >= 64:
                raise ValueError("tree_limit")
            a = element.attrib
            name = a.get("name", "")
            if name == "_ws.malformed" or name.startswith("_ws.malformed."):
                malformed = True
            if name == "_ws.expert.message" and re.search(r"unknown|not dissected|undecoded|malformed", a.get("show", ""), re.I):
                uncertain = True
            try:
                start, size = int(a.get("pos", "0")) - prefix, int(a.get("size", "0"))
            except ValueError:
                raise ValueError("invalid_field_range")
            valid = start >= 0 and size >= 0 and start + size <= len(data)
            node = dict(name=name, label=a.get("showname", a.get("show", name))[:4096],
                        value=a.get("show", "")[:4096], children=[])
            if valid and size:
                node.update(offset=start, length=size)
            if SECRET.search(name):
                if not valid or size == 0:
                    uncertain = True
                elif redact_keys:
                    secrets.append((start, start + size))
                if redact_keys:
                    node.update(label=name, value="已遮蔽", redacted=True)
            stack.append((element.tag, node))
        else:
            tag, node = stack.pop()
            if tag == "proto" and node["name"] in ("ngap", "nas-5gs", "nas_5gs"):
                roots.append(node)
            elif tag in ("proto", "field") and stack:
                stack[-1][1]["children"].append(node)
            element.clear()
    if malformed or not roots:
        return dict(state="error", reason="malformed", decoder_version=decoder_version(redact_keys), redact_keys=redact_keys)

    def protected_names(node):
        if node.get("redacted"):
            return [node["name"].split('.')[-1]]
        names = []
        for child in node.get("children", []):
            names.extend(protected_names(child))
        return list(dict.fromkeys(names))[:3]

    def sanitize(node):
        # A parent can repeat secret bytes without a usable offset of its own.
        child_secret = False
        for child in node["children"]:
            child_secret = sanitize(child) or child_secret
        start, size = node.get("offset"), node.get("length", 0)
        overlap = start is not None and any(start < end and start + size > begin for begin, end in secrets)
        if node.get("redacted"):
            node.update(label=node["name"], value="已遮蔽")
        elif child_secret or overlap:
            # Reconstruct a static label rather than echoing any ancestor's raw data.
            label = "NG Application Protocol" if node["name"] == "ngap" else node["name"].split('.')[-1].removesuffix('_element')
            if not label:
                names = protected_names(node)
                label = "协议项（" + "、".join(names) + "）" if names else "协议数据项"
            node.update(label=label, value="", contains_redacted=True)
        if not node["children"]:
            node.pop("children")
        return bool(node.get("redacted") or node.get("contains_redacted"))
    for node in roots:
        sanitize(node)
    octets = [f"{b:02x}" for b in data]
    for begin, end in secrets:
        octets[begin:end] = ["**"] * (end - begin)
    return dict(state="ready", tree=roots, hex=" ".join(octets) if not uncertain else None,
                hex_unavailable="incomplete_decode" if uncertain else None,
                redacted=bool(secrets), redact_keys=redact_keys, length=len(data), decoder_version=decoder_version(redact_keys))


async def run_tshark(data, kind, redact_keys=True):
    pcap, prefix = upper_pdu(data, kind)
    proc = await asyncio.create_subprocess_exec(os.getenv("TSHARK", "tshark"),
            "-n", "-r", "-", "-T", "pdml", "--disable-heuristic", "nas_5gs_udp",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
            env={**os.environ, "HOME": "/nonexistent", "WIRESHARK_CONFIG_DIR": "/nonexistent"})
    try:
        proc.stdin.write(pcap)
        await proc.stdin.drain()
        proc.stdin.close()
        result = bytearray()
        while True:
            chunk = await proc.stdout.read(65536)
            if not chunk:
                break
            if len(result) + len(chunk) > MAX_OUTPUT:
                raise ValueError("output_limit")
            result.extend(chunk)
        if await proc.wait():
            raise ValueError("decode_failed")
        return protocol_tree(result, data, prefix, redact_keys=redact_keys)
    finally:
        if proc.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                proc.kill()
            await proc.wait()


def application():
    token = os.getenv("MONITOR_INTERNAL_TOKEN", "")
    if len(token) < 32:
        raise ValueError("internal token required")
    redact_keys = trace_redact_keys()
    version = decoder_version(redact_keys)
    app = web.Application(client_max_size=32768)
    busy = False
    ready = os.getenv("MONITOR_REQUIRE_AFFINITY", "1") == "0"

    async def guard(app):
        async def check():
            nonlocal ready
            pin_cpu()
            if ready:
                return
            async with aiohttp.ClientSession(trust_env=False,
                    timeout=aiohttp.ClientTimeout(total=1)) as http:
                while True:
                    try:
                        async with http.get(os.environ["MONITOR_URL"].rstrip("/") + "/internal/trace-control",
                                headers={"Authorization": "Bearer " + token}) as response:
                            control = json.loads(await read_limited(response, 16384))
                        reserved = control.get("affinity", {}).get("reserved")
                        ready = bool(control.get("enabled") and reserved)
                        if ready:
                            pin_cpu(reserved)
                    except (aiohttp.ClientError, OSError, ValueError, KeyError, TimeoutError):
                        ready = False
                    await asyncio.sleep(2)
        task = asyncio.create_task(check())
        try:
            yield
        finally:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)

    async def decode(request):
        nonlocal busy
        if not hmac.compare_digest(request.headers.get("Authorization", ""), "Bearer " + token):
            raise web.HTTPUnauthorized()
        if busy or not ready:
            raise web.HTTPServiceUnavailable()
        if request.headers.get("X-Trace-Kind") not in ("1", "2", "3"):
            raise web.HTTPBadRequest()
        busy = True
        try:
            data = await asyncio.wait_for(request.read(), .5)
            if not data or len(data) > 32768:
                raise web.HTTPBadRequest()
            try:
                result = await asyncio.wait_for(run_tshark(data, int(request.headers["X-Trace-Kind"]), redact_keys=redact_keys), 2)
            except TimeoutError:
                result = dict(state="error", reason="decode_timeout", decoder_version=version, redact_keys=redact_keys)
            except (ValueError, ET.ParseError, OSError):
                result = dict(state="error", reason="decode_failed", decoder_version=version, redact_keys=redact_keys)
            body = json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode()
            if len(body) > 1024 * 1024:
                body = json.dumps(dict(state="error", reason="output_limit", decoder_version=version, redact_keys=redact_keys)).encode()
            return web.Response(body=body, content_type="application/json", headers={"Cache-Control": "no-store"})
        finally:
            busy = False

    async def health(request):
        return web.json_response({"ok": True, "ready": ready, "decoder_version": version, "redact_keys": redact_keys})
    app.router.add_get("/healthz", health)
    app.router.add_post("/decode", decode)
    app.cleanup_ctx.append(guard)
    return app


if __name__ == "__main__":
    wait_for_cpu()
    web.run_app(application(), host="0.0.0.0", port=9997, access_log=None)
