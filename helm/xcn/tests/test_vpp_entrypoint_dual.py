#!/usr/bin/env python3

import importlib.util
import os
import unittest
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).parents[1] / "files" / "vpp-entrypoint-dual.py"
SPEC = importlib.util.spec_from_file_location("vpp_entrypoint_dual", MODULE_PATH)
VPP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VPP)


class AllocatedCpusTest(unittest.TestCase):
    def test_uses_webhook_isocpu_while_cgroup_is_node_wide(self):
        with mock.patch.object(VPP, "read_allowed_cpus", return_value=(
                list(range(144)), "/cpuset")), mock.patch.dict(
                    os.environ, {"ISOCPU": "cpu=47,119"}, clear=False):
            self.assertEqual(VPP.allocated_cpus(2, 0, 0.01), [47, 119])

    def test_waits_for_controller_to_converge_cgroup(self):
        allocations = [
            (list(range(144)), "/cpuset"),
            ([47, 119], "/cpuset"),
        ]
        with mock.patch.object(VPP, "read_allowed_cpus",
                               side_effect=allocations), mock.patch.dict(
                                   os.environ, {}, clear=True), mock.patch.object(
                                       VPP.time, "sleep") as sleep:
            self.assertEqual(VPP.allocated_cpus(2, 1, 0.01), [47, 119])
            sleep.assert_called_once()

    def test_fails_after_timeout_without_allocation(self):
        with mock.patch.object(VPP, "read_allowed_cpus", return_value=(
                list(range(144)), "/cpuset")), mock.patch.dict(
                    os.environ, {}, clear=True):
            with self.assertRaisesRegex(
                    VPP.ConfigError, "did not become ready within 0s"):
                VPP.allocated_cpus(2, 0, 0.01)

    def test_rejects_wrong_isocpu_count_without_waiting(self):
        with mock.patch.object(VPP, "read_allowed_cpus", return_value=(
                list(range(144)), "/cpuset")), mock.patch.dict(
                    os.environ, {"ISOCPU": "cpu=47"}, clear=True):
            with self.assertRaisesRegex(VPP.ConfigError, "assigns 1"):
                VPP.allocated_cpus(2, 120, 0.01)


if __name__ == "__main__":
    unittest.main()
