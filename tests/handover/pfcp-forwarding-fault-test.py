#!/usr/bin/env python3
"""Exercise real UPF rollback using standard PFCP/GTP-U in an isolated netns.

Run after starting only the test UPF (sample.yaml); never against a live core:
XCN_FORWARDING_ISOLATED=1 python3 tests/handover/pfcp-forwarding-fault-test.py
"""
import os
import socket
import struct
import time


def ie(kind, value):
    return struct.pack("!HH", kind, len(value)) + value


def integer(kind, value, width):
    return ie(kind, value.to_bytes(width, "big"))


def ies(packet):
    offset = 16 if packet[0] & 1 else 8
    result = {}
    while offset < len(packet):
        kind, size = struct.unpack_from("!HH", packet, offset)
        assert offset + 4 + size <= len(packet)
        result.setdefault(kind, []).append(packet[offset + 4:offset + 4 + size])
        offset += 4 + size
    return result


def pdr(rule, forwarding=False, invalid=False):
    fteid = b"\x04" if invalid else (
        b"\x01" + struct.pack("!I", 0x10000 + rule) + socket.inet_aton("127.0.0.7"))
    pdi = ie(20, b"\x00") + ie(21, fteid)
    if forwarding:
        pdi += ie(160, b"\x1c")
    return ie(1, integer(56, rule, 2) + integer(29, 100, 4) + ie(2, pdi)
              + ie(95, b"\x06") + integer(108, rule, 4))


def far(rule, forwarding=False, invalid=False):
    params = ie(84, b"\x01\x00" + struct.pack("!I", 0x20000 + rule)
                + socket.inet_aton("127.0.0.2"))
    if not invalid:
        params = ie(42, b"\x00" if forwarding else b"\x01") + params
    action = b"" if invalid else ie(44, b"\x02")
    return ie(3, integer(108, rule, 4) + action + ie(4, params))


def main():
    assert os.environ.get("XCN_FORWARDING_ISOLATED") == "1", "Use an isolated network namespace"
    control = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    control.bind(("127.0.0.4", 8805))
    control.settimeout(3)
    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    receiver.bind(("127.0.0.2", 2152))
    receiver.settimeout(1)
    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sequence = 0
    retries = 0

    def request(kind, body, seid=None, accepted=True):
        nonlocal sequence, retries
        sequence += 1
        tail = sequence.to_bytes(3, "big") + b"\x00"
        if seid is not None:
            tail = struct.pack("!Q", seid) + tail
        packet = bytes([0x21 if seid is not None else 0x20, kind])
        packet += struct.pack("!H", len(tail) + len(body)) + tail + body
        control.sendto(packet, ("127.0.0.7", 8805))
        attempts = 0
        while True:
            try:
                response, _ = control.recvfrom(65535)
            except socket.timeout:
                attempts += 1
                assert attempts <= 10, "PFCP did not recover after resource exhaustion"
                retries += 1
                control.sendto(packet, ("127.0.0.7", 8805))
                continue
            if response[1] == 1:  # Keep the association heartbeat alive.
                control.sendto(bytes([0x20, 2]) + response[2:8]
                               + integer(96, int(time.time()) + 2208988800, 4),
                               ("127.0.0.7", 8805))
                continue
            offset = 12 if response[0] & 1 else 4
            if response[offset:offset + 3] != sequence.to_bytes(3, "big"):
                continue
            assert response[1] == kind + 1, response.hex()
            fields = ies(response)
            assert (fields[19][0][0] == 1) == accepted, response.hex()
            return fields

    def probe(rule=1):
        # Valid IPv4 header: ordinary Access->Core traffic, independent of forwarding.
        payload = bytes.fromhex("4500001400000000401100000a2d00010a2d0002")
        packet = b"\x30\xff" + struct.pack("!HI", len(payload), 0x10000 + rule) + payload
        sender.sendto(packet, ("127.0.0.7", 2152))
        actual, _ = receiver.recvfrom(65535)
        expected = packet[:4] + struct.pack("!I", 0x20000 + rule) + packet[8:]
        assert actual == expected, (actual.hex(), expected.hex())

    node = ie(60, b"\x00" + socket.inet_aton("127.0.0.4"))
    request(5, node + integer(96, int(time.time()) + 2208988800, 4))
    fseid = ie(57, b"\x02" + struct.pack("!Q", 0xabc) + socket.inet_aton("127.0.0.4"))
    fields = request(50, node + fseid + pdr(1) + far(1), seid=0)
    seid = struct.unpack_from("!Q", fields[57][0], 1)[0]
    probe()
    for cycle in range(100):
        # Reject attempts to overwrite a normal rule with a forwarding rule.
        request(52, pdr(1, True) + far(1, True), seid, accepted=False)
        probe()
        # Successful first batch; failing second batch with missing Apply Action.
        request(52, pdr(2, True) + far(2, True), seid)
        probe(2)
        request(52, pdr(3, True) + far(3, True, invalid=True), seid, accepted=False)
        probe()
        # A failed CH allocation must not disturb the ordinary PDR either.
        request(52, pdr(4, True, invalid=True) + far(4, True), seid, accepted=False)
        probe()
        # Roll back the successful batch and the absent/failed rules together.
        remove = b"".join(ie(15, integer(56, rule, 2))
                          + ie(16, integer(108, rule, 4)) for rule in (2, 3, 4))
        request(52, remove, seid)
        probe()
        # Idempotent removal and normal forwarding are still usable.
        request(52, remove, seid)
        probe()
    request(54, b"", seid)
    print("PASS: 100 rollback cycles, 300 rejected allocations, 601 validated GTP-U packets")
    print(f"PFCP retransmissions after resource exhaustion: {retries}")


if __name__ == "__main__":
    main()
