#!/usr/bin/env python3
"""Check build/push failure contracts without accessing Docker or any network."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BuildImagesTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='xcn-build-test-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        (self.root / 'docker').mkdir()
        shutil.copy2(ROOT / 'docker/build-images.sh', self.root / 'docker/build-images.sh')
        (self.root / 'docker/vendor.py').write_text('''import json,os,sys
with open(os.environ['VENDOR_CALLS'],'a') as f:f.write(json.dumps(sys.argv[1:])+'\\n')
sys.exit(int(os.getenv('VENDOR_FAILURE','0')))
''')
        binary = self.root / 'bin'
        binary.mkdir()
        fake = binary / 'docker'
        fake.write_text('''#!/usr/bin/env python3
import json,os,sys
a=sys.argv[1:]
with open(os.environ['CALLS'],'a') as f:f.write(json.dumps(a)+'\\n')
if a[:2]==['image','inspect']:
    if os.getenv('BASE_MISSING') or a[-1]==os.getenv('MISSING_BASE'):sys.exit(1)
    if '--format' in a:print('linux/amd64')
elif a[0]=='pull':sys.exit(12)
elif a[0]=='push':sys.exit(int(os.getenv('PUSH_FAILURE','0')))
elif a[0]=='build' and os.getenv('FAIL_COMPONENT','never') in a[a.index('-f')+1]:sys.exit(9)
''')
        fake.chmod(0o755)
        timeout = binary / 'timeout'
        timeout.write_text('#!/usr/bin/env python3\nimport os,sys\nos.execvp(sys.argv[2],sys.argv[2:])\n')
        timeout.chmod(0o755)
        self.env = {k:v for k,v in os.environ.items() if k.lower() not in
                    ('http_proxy', 'https_proxy', 'all_proxy', 'no_proxy') and k not in
                    ('UBUNTU_IMAGE', 'VPP_IMAGE', 'BUILD_JOBS', 'BUILD_CPU_QUOTA', 'BUILD_MEMORY', 'PUSH_TIMEOUT', 'BUILD_NETWORK')}
        self.env.update(PATH=str(binary)+os.pathsep+os.environ['PATH'], CALLS=str(self.root / 'calls.jsonl'),
                        VENDOR_CALLS=str(self.root / 'vendor-calls.jsonl'))

    def run_build(self, *args, **env):
        return subprocess.run(['bash', str(self.root / 'docker/build-images.sh'), *args],
                              capture_output=True, text=True, env=dict(self.env, **env))

    def calls(self):
        path = self.root / 'calls.jsonl'
        return [json.loads(x) for x in path.read_text().splitlines()] if path.exists() else []

    def vendor_calls(self):
        path = self.root / 'vendor-calls.jsonl'
        return [json.loads(x) for x in path.read_text().splitlines()] if path.exists() else []

    def test_push_failure_is_nonfatal_and_all_images_built(self):
        result = self.run_build('--tag', 'test-1', PUSH_FAILURE='7')
        self.assertEqual(result.returncode, 0, result.stderr)
        builds = [x for x in self.calls() if x[0] == 'build']
        pushes = [x for x in self.calls() if x[0] == 'push']
        self.assertEqual(len(builds), 3)
        self.assertEqual(len(pushes), 3)
        self.assertEqual(pushes[-1][-1], '192.168.10.35/xcn/xcn-monitor:test-1')
        self.assertEqual(result.stdout.count('build OK, push FAILED'), 3)
        self.assertEqual(self.vendor_calls(), [['verify', 'all']])

    def test_image_selection_limits_builds_pushes_bases_and_asset_checks(self):
        files = {'runtime': 'docker/runtime/Dockerfile', 'webui': 'docker/webui/Dockerfile',
                 'monitor': 'monitor/Dockerfile'}
        for selected in ('runtime', 'webui', 'monitor', 'all'):
            with self.subTest(image=selected):
                (self.root / 'calls.jsonl').unlink(missing_ok=True)
                (self.root / 'vendor-calls.jsonl').unlink(missing_ok=True)
                result = self.run_build('--tag', 'selected', '--image', selected,
                                        '--registry', 'localhost:5000')
                self.assertEqual(result.returncode, 0, result.stderr)
                names = list(files) if selected == 'all' else [selected]
                images = ['localhost:5000/xcn-'+name+':selected' for name in names]
                builds = [x for x in self.calls() if x[0] == 'build']
                self.assertEqual([x[x.index('-f')+1] for x in builds], [files[x] for x in names])
                self.assertEqual([x[x.index('-t')+1] for x in builds], images)
                self.assertEqual([x[-1] for x in self.calls() if x[0] == 'push'], images)
                bases = ['192.168.10.35/xnet/ubuntu:22.04']
                if selected in ('runtime', 'all'):
                    bases.append('192.168.10.35/xcn/vpp:26.06')
                self.assertEqual([x[-1] for x in self.calls()
                                  if x[:2] == ['image', 'inspect'] and '--format' not in x], bases)
                self.assertEqual(self.vendor_calls(), [['verify', selected]])

    def test_webui_does_not_pull_an_unavailable_vpp_base(self):
        result = self.run_build('--tag', 'test', '--image', 'webui',
                                MISSING_BASE='192.168.10.35/xcn/vpp:26.06')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(any(x[0] == 'pull' for x in self.calls()))

    def test_single_image_push_failure_is_nonfatal(self):
        result = self.run_build('--tag', 'test', '--image', 'monitor', PUSH_FAILURE='7')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len([x for x in self.calls() if x[0] == 'build']), 1)
        self.assertEqual(len([x for x in self.calls() if x[0] == 'push']), 1)
        self.assertEqual(result.stdout.count('build OK, push FAILED'), 1)

    def test_build_failure_remains_fatal(self):
        result = self.run_build('--tag', 'test', FAIL_COMPONENT='webui')
        self.assertEqual(result.returncode, 9)
        self.assertEqual(len([x for x in self.calls() if x[0] == 'build']), 2)
        self.assertEqual(len([x for x in self.calls() if x[0] == 'push']), 1)

    def test_missing_assets_fail_before_any_docker_action(self):
        self.assertNotEqual(self.run_build('--tag', 'test', VENDOR_FAILURE='1').returncode, 0)
        self.assertEqual(self.calls(), [])

    def test_selected_image_missing_assets_fail_before_any_docker_action(self):
        result = self.run_build('--tag', 'test', '--image', 'monitor', VENDOR_FAILURE='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.vendor_calls(), [['verify', 'monitor']])
        self.assertEqual(self.calls(), [])

    def test_missing_base_does_not_silently_use_another_image(self):
        self.assertNotEqual(self.run_build('--tag', 'test', BASE_MISSING='1').returncode, 0)
        self.assertEqual(self.calls()[-1], ['pull', '192.168.10.35/xnet/ubuntu:22.04'])

    def test_offline_clears_proxy_and_keeps_expected_bases_and_cpu_limits(self):
        cpu = str(min(os.sched_getaffinity(0)))
        result = self.run_build('--tag', 'test', '--offline', '--cpuset', cpu, HTTP_PROXY='http://invalid.test:9')
        self.assertEqual(result.returncode, 0, result.stderr)
        for args in [x for x in self.calls() if x[0] == 'build']:
            for value in ('OFFLINE=1', 'HTTP_PROXY=', 'HTTPS_PROXY=', 'http_proxy=',
                          'UBUNTU_IMAGE=192.168.10.35/xnet/ubuntu:22.04',
                          'VPP_IMAGE=192.168.10.35/xcn/vpp:26.06', 'BUILD_CPUSET='+cpu):
                self.assertIn(value, args)
            self.assertEqual(args[args.index('--cpuset-cpus')+1], cpu)
            self.assertEqual(args[args.index('--memory')+1], '4g')

    def test_invalid_arguments_are_rejected_before_dependencies_or_docker(self):
        for args in [[], ['--tag', 'bad/tag'], ['--tag', 'x', '--cpuset', '2-1'],
                     ['--tag', 'x', '--cpuset', '1;echo'], ['--tag'],
                     ['--tag', 'x', '--image'], ['--tag', 'x', '--image', ''],
                     ['--tag', 'x', '--image', 'invalid'], ['--tag', 'x', '--image', 'webui;echo']]:
            with self.subTest(args=args):
                self.assertNotEqual(self.run_build(*args).returncode, 0)
                self.assertEqual(self.calls(), [])
                self.assertEqual(self.vendor_calls(), [])


if __name__ == '__main__':
    unittest.main()
