"""CPU quantities follow real allocations, including delayed/shared cpusets."""
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import common


class CpuAllocationTest(unittest.TestCase):
    def setUp(self):
        environment = patch.dict(os.environ, {
            'MONITOR_CPUSET': '', 'MONITOR_REQUIRE_AFFINITY': '1',
            'MONITOR_CPU_LIMIT_MILLICORES': '500', 'MONITOR_CPUSET_WAIT_SECONDS': '0',
            'ISOCPU': ''}, clear=True)
        environment.start(); self.addCleanup(environment.stop)
        self.files = {
            '/proc/self/cgroup': '0::/\n',
            '/sys/fs/cgroup/cpuset.cpus.effective': '6-7',
            '/sys/devices/system/cpu/online': '0-7',
            '/sys/devices/system/cpu/cpu0/topology/thread_siblings_list': '0,4',
            '/sys/devices/system/cpu/cpu1/topology/thread_siblings_list': '1,5',
        }
        def read(path, *args, **kwargs):
            if str(path) not in self.files:
                raise FileNotFoundError(str(path))
            return self.files[str(path)]
        reader = patch.object(Path, 'read_text', autospec=True, side_effect=read)
        reader.start(); self.addCleanup(reader.stop)
        setter = patch.object(os, 'sched_setaffinity')
        self.set_affinity = setter.start(); self.addCleanup(setter.stop)

    def allocation(self, value):
        self.files['/sys/fs/cgroup/cpuset.cpus.effective'] = value

    def test_fractional_limit_follows_shared_pool_without_pinning(self):
        self.assertEqual(common.pin_cpu([0]), [6, 7])
        self.allocation('2-3,6-7')
        self.assertEqual(common.pin_cpu([0]), [2, 3, 6, 7])
        self.set_affinity.assert_not_called()

    def test_nonintegral_multiple_cores_still_uses_shared_pool(self):
        os.environ['MONITOR_CPU_LIMIT_MILLICORES'] = '1500'
        self.assertEqual(common.pin_cpu([0]), [6, 7])

    def test_initial_node_wide_shared_pool_is_not_ready(self):
        self.allocation('0-7')
        with self.assertRaises(common.AllocationPending):
            common.pin_cpu()

    def test_integer_waits_for_kernel_even_when_isocpu_is_present(self):
        os.environ.update(MONITOR_CPU_LIMIT_MILLICORES='2000', ISOCPU='cpu=6,7')
        self.allocation('0-7')
        with self.assertRaises(common.AllocationPending):
            common.pin_cpu()
        self.allocation('6-7')
        self.assertEqual(common.pin_cpu([0]), [6, 7])
        self.set_affinity.assert_not_called()

    def test_two_logical_siblings_are_a_valid_two_cpu_allocation(self):
        os.environ.update(MONITOR_CPU_LIMIT_MILLICORES='2000', ISOCPU='cpu=2,6')
        self.allocation('2,6')
        self.assertEqual(common.pin_cpu([0]), [2, 6])

    def test_kernel_integer_allocation_without_webhook_hint(self):
        os.environ['MONITOR_CPU_LIMIT_MILLICORES'] = '1000'
        self.allocation('6')
        self.assertEqual(common.pin_cpu([0]), [6])

    def test_stale_hint_waits_and_malformed_hint_fails(self):
        os.environ.update(MONITOR_CPU_LIMIT_MILLICORES='2000', ISOCPU='cpu=2,3')
        with self.assertRaises(common.AllocationPending):
            common.pin_cpu()
        for value in ['6,7', 'cpu=', 'cpu=7-6']:
            with self.subTest(hint=value):
                os.environ['ISOCPU'] = value
                with self.assertRaises(ValueError):
                    common.pin_cpu()

    def test_reserved_smt_sibling_stops_collection_and_can_recover(self):
        self.allocation('4,6')
        with self.assertRaises(common.AllocationPending):
            common.pin_cpu([0])
        self.allocation('6-7')
        self.assertEqual(common.pin_cpu([0]), [6, 7])

    def test_v1_own_leaf_is_used_instead_of_node_wide_root(self):
        del self.files['/sys/fs/cgroup/cpuset.cpus.effective']
        self.files.update({
            '/proc/self/cgroup': '3:cpuset:/pod/container\n',
            '/sys/fs/cgroup/cpuset/cpuset.cpus': '0-7',
            '/sys/fs/cgroup/cpuset/pod/container/cpuset.cpus': '6-7'})
        self.assertEqual(common.pin_cpu([0]), [6, 7])

    def test_v1_scoped_mount_falls_back_to_its_own_root(self):
        del self.files['/sys/fs/cgroup/cpuset.cpus.effective']
        self.files.update({'/proc/self/cgroup': '3:cpuset:/pod/container\n',
                           '/sys/fs/cgroup/cpuset/cpuset.cpus': '6-7'})
        self.assertEqual(common.pin_cpu([0]), [6, 7])

    def test_v2_own_leaf_is_used_instead_of_node_wide_root(self):
        self.files.update({'/proc/self/cgroup': '0::/pod/container\n',
                           '/sys/fs/cgroup/cpuset.cpus.effective': '0-7',
                           '/sys/fs/cgroup/pod/container/cpuset.cpus.effective': '6-7'})
        self.assertEqual(common.pin_cpu([0]), [6, 7])

    def test_missing_kernel_cpuset_does_not_trust_process_affinity(self):
        del self.files['/sys/fs/cgroup/cpuset.cpus.effective']
        with self.assertRaises(common.AllocationPending):
            common.pin_cpu()

    def test_wait_retries_until_kernel_allocation_converges(self):
        os.environ.update(MONITOR_CPU_LIMIT_MILLICORES='2000', MONITOR_CPUSET_WAIT_SECONDS='2')
        self.allocation('0-7')
        clock = [0.0]
        def sleep(seconds):
            clock[0] += seconds
            self.allocation('6-7')
        with patch('common.time.monotonic', side_effect=lambda: clock[0]), \
                patch('common.time.sleep', side_effect=sleep) as wait:
            self.assertEqual(common.wait_for_cpu(), [6, 7])
            wait.assert_called_once_with(.25)

    def test_wait_timeout_never_uses_unallocated_cpus(self):
        self.allocation('0-7')
        with patch('common.time.sleep') as wait, self.assertRaisesRegex(ValueError, '等待超时'):
            common.wait_for_cpu()
        wait.assert_not_called(); self.set_affinity.assert_not_called()

    def test_bad_limits_and_wait_options_fail(self):
        for value in ['', '0', '-1', '500m', '1.5', '65536001']:
            with self.subTest(limit=value):
                os.environ['MONITOR_CPU_LIMIT_MILLICORES'] = value
                with self.assertRaises(ValueError):
                    common.pin_cpu()
        for value in ['-1', '601', '1.5', '']:
            with self.subTest(wait=value):
                os.environ['MONITOR_CPUSET_WAIT_SECONDS'] = value
                with self.assertRaises(ValueError):
                    common.wait_for_cpu()

    def test_legacy_manual_override_keeps_validation_and_pinning(self):
        os.environ['MONITOR_CPUSET'] = '6-7'
        with patch('common.os.sched_getaffinity', return_value={6, 7}):
            self.assertEqual(common.pin_cpu([0]), [6, 7])
        self.set_affinity.assert_called_once_with(0, {6, 7})
        os.environ['MONITOR_CPUSET'] = '4'
        with self.assertRaisesRegex(ValueError, '重叠'):
            common.pin_cpu([0])

    def test_local_validation_opt_out_does_not_need_kubernetes_limits(self):
        os.environ.update(MONITOR_REQUIRE_AFFINITY='0', MONITOR_CPU_LIMIT_MILLICORES='')
        with patch('common.os.sched_getaffinity', return_value={1, 3}):
            self.assertEqual(common.pin_cpu(), [1, 3])
        self.set_affinity.assert_not_called()

    def test_invalid_cpu_ranges_and_membership_are_rejected(self):
        for value in ['7-6', '1,,2', '-1', '0-65536', '1-2-3', 'x']:
            with self.subTest(cpus=value), self.assertRaises(ValueError):
                common.cpu_list(value)
        self.files['/proc/self/cgroup'] = '0::/../other\n'
        with self.assertRaises(ValueError):
            common.pin_cpu()


if __name__ == '__main__':
    unittest.main()
