#!/usr/bin/env python3
"""Render the real chart to check shared addresses and reject unsafe pools."""

import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml


CHART = Path(__file__).parents[1]


def render(values):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "values.yaml"
        path.write_text(yaml.safe_dump(values))
        return subprocess.run(
            ["helm", "template", "xcn", str(CHART), "-f", str(path)],
            capture_output=True, text=True, check=False)


def custom_values(mode="tun"):
    return {
        "networking": {
            "ue": {"ipv4Subnet": "10.66.12.0/24", "ipv4Gateway": "10.66.12.254"},
            "smf": {"ipv4PoolRanges": ["10.66.12.10-10.66.12.199"]},
            "upf": {"mode": mode, "n3": {"address": "192.0.2.10"}},
        },
        "vpp": {"n3": {"interfaceAddress": "192.0.2.11/24"},
                "n6": {"externalAddress": "192.0.2.12/24"}},
    }


class UeIpv4ConfigTest(unittest.TestCase):
    def check_shared(self, values, subnet, gateway, ranges):
        result = render(values)
        self.assertEqual(result.returncode, 0, result.stderr)
        resources = list(yaml.safe_load_all(result.stdout))
        config = next(item["data"] for item in resources if item["kind"] == "ConfigMap")
        for filename, nf in (("smf.yaml", "smf"), ("upf.yaml.tmpl", "upf")):
            sessions = yaml.safe_load(config[filename])[nf]["session"]
            self.assertEqual(sessions[0]["subnet"], subnet)
            self.assertEqual(sessions[0]["gateway"], gateway)
            self.assertEqual(sessions[1]["subnet"], "2001:db8:cafe::/48")
            if nf == "smf":
                self.assertEqual(sessions[0].get("range", []), ranges)
        self.assertIn(f"memif1/0 {gateway}/{subnet.split('/')[1]}",
                      config["vpp-cli-commands.conf.template"])
        deployment = next(item for item in resources if item["kind"] == "Deployment"
                          and item["metadata"]["name"] == "xcn-5gc")
        upf = next(item for item in deployment["spec"]["template"]["spec"]["containers"]
                   if item["name"] == "upf")
        env = {item["name"]: item.get("value") for item in upf["env"]}
        self.assertEqual(env["UE_IPV4_SUBNET"], subnet)
        self.assertEqual(env["UE_IPV4_GATEWAY"], gateway)

    def test_defaults_preserved(self):
        self.check_shared({}, "10.45.0.0/16", "10.45.0.1", [])

    def test_custom_network_in_both_modes(self):
        for mode in ("tun", "memif"):
            with self.subTest(mode=mode):
                self.check_shared(custom_values(mode), "10.66.12.0/24", "10.66.12.254",
                                  ["10.66.12.10-10.66.12.199"])

    def test_empty_pool_and_prefix_boundaries(self):
        for subnet, gateway in (("128.0.0.0/1", "128.0.0.1"),
                                ("192.168.10.0/30", "192.168.10.1")):
            with self.subTest(subnet=subnet):
                self.check_shared({"networking": {"ue": {
                    "ipv4Subnet": subnet, "ipv4Gateway": gateway}}}, subnet, gateway, [])

    def test_multiple_disjoint_ranges(self):
        values = custom_values()
        ranges = ["10.66.12.10-10.66.12.99", "10.66.12.110-10.66.12.199"]
        values["networking"]["smf"]["ipv4PoolRanges"] = ranges
        self.check_shared(values, "10.66.12.0/24", "10.66.12.254", ranges)

    def test_invalid_subnet_and_gateway(self):
        cases = [
            ("ipv4Subnet", "10.66.12.1/24"), ("ipv4Subnet", "10.66.12.0/31"),
            ("ipv4Subnet", "10.66.12.0/32"), ("ipv4Subnet", "10.66.12.0/0"),
            ("ipv4Subnet", "10.66.12.0/024"), ("ipv4Subnet", "300.66.12.0/24"),
            ("ipv4Subnet", "2001:db8::/64"), ("ipv4Subnet", ""),
            ("ipv4Subnet", None), ("ipv4Subnet", 24),
            ("ipv4Gateway", "10.66.13.1"), ("ipv4Gateway", "10.66.12.0"),
            ("ipv4Gateway", "10.66.12.255"), ("ipv4Gateway", "10.66.12.256"),
            ("ipv4Gateway", "10.066.12.1"), ("ipv4Gateway", "10.66.12.1/24"),
            ("ipv4Gateway", ""), ("ipv4Gateway", None), ("ipv4Gateway", True),
        ]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                values = custom_values()
                values["networking"]["ue"][key] = value
                result = render(values)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("networking.ue", result.stderr)

    def test_invalid_dynamic_ranges(self):
        cases = [None, "10.66.12.10-10.66.12.199", {}, [True], ["invalid"],
                 ["10.66.12.10-"], ["10.66.12.200-10.66.12.100"],
                 ["10.66.11.10-10.66.12.100"], ["10.66.12.10-10.66.13.1"],
                 ["10.66.12.0-10.66.12.100"], ["10.66.12.10-10.66.12.255"],
                 ["10.66.12.10-10.66.12.254"],
                 ["10.66.12.10-10.66.12.99", "10.66.12.99-10.66.12.199"],
                 ["10.66.12.10-10.66.12.199"] * 17]
        for ranges in cases:
            with self.subTest(ranges=ranges):
                values = custom_values()
                values["networking"]["smf"]["ipv4PoolRanges"] = ranges
                result = render(values)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("networking.smf.ipv4PoolRanges", result.stderr)

    def test_legacy_vpp_address_must_match(self):
        values = custom_values("memif")
        values["vpp"]["n6"]["interfaceAddress"] = "10.66.12.254/24"
        self.assertEqual(render(values).returncode, 0)
        values["vpp"]["n6"]["interfaceAddress"] = "10.45.0.1/16"
        result = render(values)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vpp.n6.interfaceAddress", result.stderr)


if __name__ == "__main__":
    unittest.main()
