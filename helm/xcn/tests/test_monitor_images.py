#!/usr/bin/env python3
"""Render real workloads to check shared-image and independent-upgrade behavior."""
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml


CHART = Path(__file__).resolve().parents[1]


def render(values):
    with tempfile.TemporaryDirectory(prefix="xcn-monitor-images-") as directory:
        filename = Path(directory) / "values.yaml"
        filename.write_text(yaml.safe_dump(values))
        output = subprocess.check_output(
            ["helm", "template", "xcn", str(CHART), "-f", str(filename)], text=True)
    return list(yaml.safe_load_all(output))


def workloads(documents):
    return {item["metadata"]["name"]: item["spec"]["template"]["spec"]
            for item in documents if item and item["kind"] in ("Deployment", "StatefulSet")}


def containers(documents):
    return {item["name"]: item for spec in workloads(documents).values()
            for item in spec.get("containers", [])}


class MonitorImagesTest(unittest.TestCase):
    def values(self, placement="colocated"):
        return {"networking": {"amf": {"ngap": {"serverAddress": "192.0.2.9"}},
                               "upf": {"mode": "memif", "n3": {"address": "192.0.2.10"}}},
                "vpp": {"n3": {"interfaceAddress": "192.0.2.11/24"},
                        "n6": {"externalAddress": "192.0.2.12/24"}},
                "monitor": {"enabled": True, "cpuSet": "36,37",
                            "image": {"repository": "example/xcn-monitor", "tag": "combined"},
                            "trace": {"enabled": True, "decoderPlacement": placement}}}

    def test_default_release_uses_five_images_in_both_layouts(self):
        for placement in ("colocated", "separate"):
            with self.subTest(placement=placement):
                docs = render(self.values(placement))
                images = {c["image"] for s in workloads(docs).values()
                          for key in ("containers", "initContainers") for c in s.get(key, [])}
                self.assertEqual(len(images), 5, images)
                roles = containers(docs)
                for role in ("monitor", "decoder", "upf-monitor-agent", "signal-trace-agent"):
                    self.assertEqual(roles[role]["image"], "example/xcn-monitor:combined")
                self.assertEqual(roles["decoder"]["command"],
                                 ["python3", "/opt/xcn-monitor/decoder.py"])
                self.assertTrue(roles["decoder"]["securityContext"]["runAsNonRoot"])
                self.assertEqual(roles["upf-monitor-agent"]["securityContext"]["runAsUser"], 0)
                self.assertEqual(roles["decoder"]["resources"]["limits"]["memory"], "512Mi")
                self.assertEqual(roles["signal-trace-agent"]["resources"]["limits"]["memory"], "128Mi")

    def test_decoder_and_collector_upgrade_leave_core_templates_unchanged(self):
        for placement in ("colocated", "separate"):
            values = self.values(placement)
            before = workloads(render(values))
            values["monitor"]["trace"]["decoderImage"] = {"tag": "codec-only"}
            values["monitor"]["collectorImage"] = {"tag": "collector-only"}
            docs = render(values)
            after = workloads(docs)
            for name in ("xcn-5gc", "xcn-core", "xcn-webui", "xcn-mongodb"):
                self.assertEqual(before[name], after[name], name)
            roles = containers(docs)
            self.assertEqual(roles["decoder"]["image"], "example/xcn-monitor:codec-only")
            self.assertEqual(roles["monitor"]["image"], "example/xcn-monitor:collector-only")

    def test_explicit_legacy_images_and_pull_policies_are_preserved(self):
        values = self.values()
        values["monitor"]["trace"].update({
            "agentImage": {"repository": "old/agent", "tag": "old", "pullPolicy": "Always"},
            "decoderImage": {"repository": "old/decoder", "tag": "old", "pullPolicy": "Never"}})
        roles = containers(render(values))
        self.assertEqual(roles["signal-trace-agent"]["image"], "old/agent:old")
        self.assertEqual(roles["signal-trace-agent"]["imagePullPolicy"], "Always")
        self.assertEqual(roles["decoder"]["image"], "old/decoder:old")
        self.assertEqual(roles["decoder"]["imagePullPolicy"], "Never")

    def test_trace_disabled_keeps_monitor_without_decoder(self):
        values = self.values()
        values["monitor"]["trace"]["enabled"] = False
        roles = containers(render(values))
        self.assertIn("monitor", roles)
        self.assertNotIn("decoder", roles)
        self.assertNotIn("signal-trace-agent", roles)

    def test_intranet_defaults_render_with_explicit_site_addresses(self):
        values = self.values()
        del values["monitor"]["image"]
        del values["monitor"]["cpuSet"]
        del values["networking"]["upf"]["mode"]
        docs = render(values)
        roles = containers(docs)
        for role in ("amf", "smf", "upf", "pcf"):
            self.assertEqual(roles[role]["image"], "192.168.10.35/xcn/xcn-runtime:latest")
        self.assertEqual(roles["webui"]["image"], "192.168.10.35/xcn/xcn-webui:latest")
        self.assertEqual(roles["monitor"]["image"], "192.168.10.35/xcn/xcn-monitor:latest")
        self.assertEqual(roles["vpp"]["image"], "192.168.10.35/xcn/vpp:26.06")
        self.assertTrue(workloads(docs)["xcn-5gc"]["hostNetwork"])
        self.assertEqual(roles["vpp"]["resources"]["requests"]["intel.com/external_network"], "2")
        config = next(d["data"] for d in docs if d and d["kind"] == "ConfigMap"
                      and "amf.yaml.tmpl" in d.get("data", {}))
        self.assertEqual(yaml.safe_load(config["amf.yaml.tmpl"])["amf"]["network_name"],
                         {"full": "U6G", "short": "U6G"})
        defaults = yaml.safe_load((CHART / "values.yaml").read_text())
        self.assertFalse(defaults["vpp"]["n6"]["nat44"]["enabled"])
        self.assertEqual(defaults["vpp"]["sriov"]["deviceEnv"], "PCIDEVICE_INTEL_COM_EXTERNAL_NETWORK")

    def test_cpu_quantities_preserve_small_per_container_budgets(self):
        values = self.values()
        del values['monitor']['cpuSet']
        roles = containers(render(values))
        expected = {'monitor': ('200m', '900m'), 'decoder': ('100m', '500m'),
                    'upf-monitor-agent': ('50m', '250m'), 'signal-trace-agent': ('50m', '250m'),
                    'webui': ('250m', '900m')}
        for name, (request, limit) in expected.items():
            with self.subTest(container=name):
                role = roles[name]
                self.assertEqual(role['resources']['requests']['cpu'], request)
                self.assertEqual(role['resources']['limits']['cpu'], limit)
                env = {e['name']: e for e in role['env']}
                self.assertEqual(env['MONITOR_CPUSET']['value'], '')
                self.assertEqual(env['MONITOR_CPU_LIMIT_MILLICORES']['valueFrom']['resourceFieldRef'],
                                 {'resource': 'limits.cpu', 'divisor': '1m'})
                self.assertEqual(env['MONITOR_CPUSET_WAIT_SECONDS']['value'], '120')
        for name in ['monitor', 'decoder', 'webui']:
            probe = roles[name]['startupProbe']
            self.assertGreaterEqual(probe['periodSeconds'] * probe['failureThreshold'], 150)
        self.assertEqual(roles['webui']['command'],
                         ['python3', '/opt/xcn-cpu/cpu_entrypoint.py', 'npm', 'run', 'start'])

    def test_integer_quantities_are_forwarded_without_fixed_core_ids(self):
        values = self.values()
        values['monitor'].update(cpuSet='', cpuSetWaitSeconds=240)
        values['monitor']['resources'] = {'requests': {'cpu': 1, 'memory': '256Mi'},
                                          'limits': {'cpu': 2, 'memory': '512Mi'}}
        role = containers(render(values))['monitor']
        self.assertEqual(role['resources']['limits']['cpu'], 2)
        env = {e['name']: e for e in role['env']}
        self.assertEqual(env['MONITOR_CPUSET']['value'], '')
        self.assertEqual(env['MONITOR_CPUSET_WAIT_SECONDS']['value'], '240')
        probe = role['startupProbe']
        self.assertGreaterEqual(probe['periodSeconds'] * probe['failureThreshold'], 270)


if __name__ == "__main__":
    unittest.main()
