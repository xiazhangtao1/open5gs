#!/usr/bin/env python3

import importlib.util
import os
import tempfile
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


class OwnCgroupTest(unittest.TestCase):
    def resolve(self, memberships, mounts, controller="cpu"):
        def read(path, **_kwargs):
            return memberships if str(path) == "/proc/self/cgroup" else mounts
        with mock.patch.object(Path, "read_text", read):
            return VPP.own_cgroup_file(controller, "cpu.cfs_quota_us", "cpu.max")

    def test_v1_whole_tree_resolves_leaf_not_parent(self):
        self.assertEqual(self.resolve(
            "7:cpu,cpuacct:/kubepods/pod/container\n",
            "1 0 0:1 / /sys/fs/cgroup/cpu rw - cgroup cgroup rw,cpu,cpuacct\n"),
            Path("/sys/fs/cgroup/cpu/kubepods/pod/container/cpu.cfs_quota_us"))

    def test_v1_scoped_container_mount(self):
        self.assertEqual(self.resolve(
            "7:cpu,cpuacct:/kubepods/pod/container\n",
            "1 0 0:1 /kubepods/pod/container /sys/fs/cgroup/cpu,cpuacct rw "
            "- cgroup cgroup rw,cpu,cpuacct\n"),
            Path("/sys/fs/cgroup/cpu,cpuacct/cpu.cfs_quota_us"))

    def test_v2_namespace(self):
        self.assertEqual(self.resolve(
            "0::/\n", "1 0 0:1 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n"),
            Path("/sys/fs/cgroup/cpu.max"))

    def test_v2_whole_tree(self):
        self.assertEqual(self.resolve(
            "0::/kubepods/pod/container\n",
            "1 0 0:1 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n"),
            Path("/sys/fs/cgroup/kubepods/pod/container/cpu.max"))

    def test_refuses_unrelated_mount(self):
        with self.assertRaisesRegex(VPP.ConfigError, "cannot locate"):
            self.resolve("7:cpu:/other/container\n",
                         "1 0 0:1 /kubepods /sys/fs/cgroup/cpu rw "
                         "- cgroup cgroup rw,cpu\n")


class CfsQuotaTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.cpuset = self.root / "cpuset.cpus"
        self.cpuset.write_text("44-45,116\n")
        self.quota = self.root / "cpu.cfs_quota_us"
        self.quota.write_text("300000\n")
        self.env = mock.patch.dict(os.environ, {"VPP_DISABLE_CFS_QUOTA": "true"})
        self.env.start()
        self.addCleanup(self.env.stop)
        patch = mock.patch.object(VPP, "own_cgroup_file", side_effect=(
            lambda controller, *_: self.cpuset if controller == "cpuset" else self.quota))
        self.resolve = patch.start()
        self.addCleanup(patch.stop)

    def test_default_never_reads_or_writes_cgroups(self):
        os.environ.pop("VPP_DISABLE_CFS_QUOTA")
        VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.resolve.assert_not_called()
        self.assertEqual(self.quota.read_text(), "300000\n")

    def test_only_own_quota_changes(self):
        parent = self.root / "parent-quota"
        parent.write_text("1375000\n")
        VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.assertEqual(self.quota.read_text(), "-1\n")
        self.assertEqual(self.cpuset.read_text(), "44-45,116\n")
        self.assertEqual(parent.read_text(), "1375000\n")

    def test_refuses_node_wide_cpuset_even_with_isocpu_hint(self):
        self.cpuset.write_text("0-143\n")
        with mock.patch.dict(os.environ, {"ISOCPU": "cpu=44-45,116"}):
            with self.assertRaisesRegex(VPP.ConfigError, "refusing to disable"):
                VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.assertEqual(self.quota.read_text(), "300000\n")

    def test_waits_for_actual_cpuset(self):
        self.cpuset.write_text("0-143\n")
        with mock.patch.object(VPP.time, "sleep", side_effect=(
                lambda _: self.cpuset.write_text("44-45,116\n"))) as sleep:
            VPP.configure_cfs_quota([44, 45, 116], 1, .01)
        sleep.assert_called_once()
        self.assertEqual(self.quota.read_text(), "-1\n")

    def test_refuses_same_count_but_different_cpus(self):
        self.cpuset.write_text("46-47,118\n")
        with self.assertRaisesRegex(VPP.ConfigError, "refusing to disable"):
            VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.assertEqual(self.quota.read_text(), "300000\n")

    def test_v2_preserves_period(self):
        self.quota = self.root / "cpu.max"
        self.quota.write_text("300000 100000\n")
        VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.assertEqual(self.quota.read_text(), "max 100000\n")

    def test_invalid_v2_quota_is_not_written(self):
        self.quota = self.root / "cpu.max"
        for value in ("300000", "300000 0", "oops 100000"):
            self.quota.write_text(value)
            with self.assertRaisesRegex(VPP.ConfigError, "invalid own cgroup"):
                VPP.configure_cfs_quota([44, 45, 116], 0, .01)
            self.assertEqual(self.quota.read_text(), value)

    def test_rejects_misspelled_switch(self):
        os.environ["VPP_DISABLE_CFS_QUOTA"] = "treu"
        with self.assertRaisesRegex(VPP.ConfigError, "must be true or false"):
            VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.resolve.assert_not_called()

    def test_failed_write_stops_startup(self):
        with mock.patch.object(Path, "write_text", side_effect=PermissionError):
            with self.assertRaises(PermissionError):
                VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.assertEqual(self.quota.read_text(), "300000\n")

    def test_failed_readback_stops_startup(self):
        with mock.patch.object(Path, "write_text", return_value=3):
            with self.assertRaisesRegex(VPP.ConfigError, "was not applied"):
                VPP.configure_cfs_quota([44, 45, 116], 0, .01)
        self.assertEqual(self.quota.read_text(), "300000\n")


if __name__ == "__main__":
    unittest.main()
