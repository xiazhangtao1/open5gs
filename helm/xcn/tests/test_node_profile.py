#!/usr/bin/env python3
"""Verify node-profile defaults, explicit false, and type errors in the real chart."""
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

CHART = Path(__file__).resolve().parents[1]


def render(profile=None):
    values = {}
    if profile is not None:
        values = {"networking": {"amf": {"ngap": {"nodeProfile": profile}}}}
    with tempfile.TemporaryDirectory() as directory:
        filename = Path(directory) / "values.yaml"
        filename.write_text(yaml.safe_dump(values))
        return subprocess.run(
            ["helm", "template", "xcn", str(CHART), "-f",
             str(CHART / "tests/site-values.yaml"), "-f", str(filename)],
            capture_output=True, text=True, check=False)


class NodeProfileTest(unittest.TestCase):
    def enabled(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        config = next(item["data"]["amf.yaml.tmpl"]
                      for item in yaml.safe_load_all(result.stdout)
                      if item and item["kind"] == "ConfigMap"
                      and "amf.yaml.tmpl" in item.get("data", {}))
        return yaml.safe_load(config)["amf"]["ngap"]["node_profile"]["enabled"]

    def test_defaults_enabled(self):
        self.assertIs(self.enabled(render()), True)

    def test_explicit_false_is_not_replaced_by_default(self):
        self.assertIs(self.enabled(render({"enabled": False})), False)

    def test_explicit_true(self):
        self.assertIs(self.enabled(render({"enabled": True})), True)

    def test_invalid_setting_rejected(self):
        for value in ("false", "true", "wrong", 0, 1, [], {}):
            with self.subTest(value=value):
                result = render({"enabled": value})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("nodeProfile.enabled", result.stderr)


if __name__ == "__main__":
    unittest.main()
