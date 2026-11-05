#!/usr/bin/env python3
"""Exercise release orchestration with Docker substitutes and real Helm packages."""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

import yaml


HELM = Path(__file__).resolve().parents[1]
REAL_HELM = shutil.which('helm')


class BuildReleaseTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='xcn release test-')
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.helm = self.root / 'helm'
        self.helm.mkdir()
        self.script = self.helm / 'build-release.sh'
        shutil.copy2(HELM / 'build-release.sh', self.script)
        shutil.copytree(HELM / 'xcn', self.helm / 'xcn',
                        ignore=shutil.ignore_patterns('__pycache__'))
        self.sources = self.chart_sources()
        binary = self.root / 'bin'
        binary.mkdir()
        (self.root / 'docker').mkdir()
        log = """import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as stream:
    stream.write(json.dumps([Path(sys.argv[0]).name, args]) + '\\n')
"""
        substitutes = {
            self.root / 'docker/build-images.sh': log + """
sys.exit(int(os.environ.get('BUILD_FAILURE', '0')))
""",
            binary / 'docker': log + """
if args == ['info']:
    sys.exit(int(os.environ.get('DOCKER_FAILURE', '0')))
if args[0] != 'save':
    sys.exit('Unexpected Docker operation')
Path(args[args.index('-o') + 1]).write_text(args[-1])
if os.environ.get('SAVE_FAILURE', 'never') + ':' in args[-1]:
    sys.exit(7)
""",
            binary / 'helm': log + """
if os.environ.get('PACKAGE_FAILURE'):
    sys.exit(8)
os.execv(os.environ['REAL_HELM'], [os.environ['REAL_HELM'], *args])
""",
        }
        for path, content in substitutes.items():
            path.write_text('#!/usr/bin/env python3\n' + content)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(binary) + os.pathsep + os.environ['PATH'],
                        CALLS=str(self.root / 'calls.jsonl'), REAL_HELM=REAL_HELM)
        for name in ('BUILD_FAILURE', 'DOCKER_FAILURE', 'SAVE_FAILURE', 'PACKAGE_FAILURE'):
            self.env.pop(name, None)

    def chart_sources(self):
        return {path.relative_to(self.helm): path.read_bytes()
                for path in (self.helm / 'xcn').rglob('*') if path.is_file()}

    def run_release(self, *args, **env):
        return subprocess.run([str(self.script), *args], cwd=self.root / 'bin',
                              env=dict(self.env, **env), capture_output=True, text=True)

    def calls(self):
        path = self.root / 'calls.jsonl'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def artifacts(self, version='0.2.0'):
        return [self.helm / f'xcn-{name}.tar' for name in ('runtime', 'webui', 'monitor')] + [
            self.helm / f'xcn-{version}.tgz']

    def check_clean(self):
        self.assertEqual(list(self.helm.glob('.build-release.*')), [])
        self.assertEqual(self.chart_sources(), self.sources)

    def test_success_from_another_directory_packages_versions_and_matching_images(self):
        version, tag = '1.2.3-rc.1+build.7', '26.11.05-test'
        result = self.run_release(version, tag)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([name for name, _ in self.calls()],
                         ['docker', 'build-images.sh', 'docker', 'docker', 'docker', 'helm'])
        self.assertEqual(self.calls()[1][1], ['--image', 'all', '--offline', '--tag', tag])
        for index, name in enumerate(('runtime', 'webui', 'monitor'), 2):
            path = self.helm / f'xcn-{name}.tar'
            image = f'192.168.10.35/xcn/xcn-{name}:{tag}'
            args = self.calls()[index][1]
            self.assertEqual(args, ['save', '-o', args[2], image])
            self.assertEqual(Path(args[2]).parent.parent, self.helm)
            self.assertEqual(Path(args[2]).name, path.name)
            self.assertEqual(path.read_text(), image)
            self.assertIn(str(path), result.stdout)
        package = self.helm / f'xcn-{version}.tgz'
        with tarfile.open(package) as archive:
            chart = yaml.safe_load(archive.extractfile('xcn/Chart.yaml'))
            values = yaml.safe_load(archive.extractfile('xcn/values.yaml'))
            self.assertNotIn('xcn/README.md', archive.getnames())
            self.assertFalse(any(name.startswith('xcn/tests/') for name in archive.getnames()))
        self.assertEqual((chart['version'], chart['appVersion']), (version, tag))
        expected = copy.deepcopy(yaml.safe_load((HELM / 'xcn/values.yaml').read_text()))
        expected['images']['runtime']['tag'] = tag
        expected['images']['webui']['tag'] = tag
        expected['monitor']['image']['tag'] = tag
        self.assertEqual(values, expected)
        rendered = subprocess.check_output(
            [REAL_HELM, 'template', 'xcn', str(package), '-f', str(HELM / 'tests/site-values.yaml')],
            text=True)
        containers = {container['name']: container
                      for item in yaml.safe_load_all(rendered)
                      if item and item['kind'] == 'Deployment'
                      for container in item['spec']['template']['spec']['containers']}
        for name in ('amf', 'smf', 'upf', 'webui', 'monitor', 'decoder',
                     'upf-monitor-agent', 'signal-trace-agent'):
            self.assertTrue(containers[name]['image'].endswith(':' + tag), name)
        webui_env = {entry['name']: entry.get('value') for entry in containers['webui']['env']}
        self.assertEqual(webui_env['XCN_CHART_VERSION'], version)
        self.assertEqual(webui_env['XCN_APP_VERSION'], tag)
        self.check_clean()

    def test_success_overwrites_existing_artifacts_and_preserves_numeric_tag_as_string(self):
        for path in self.artifacts():
            path.write_bytes(b'previous release')
        result = self.run_release('0.2.0', '20261105')
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in self.artifacts():
            self.assertNotEqual(path.read_bytes(), b'previous release')
        with tarfile.open(self.artifacts()[-1]) as archive:
            self.assertEqual(yaml.safe_load(archive.extractfile('xcn/Chart.yaml'))['appVersion'],
                             '20261105')
            values = yaml.safe_load(archive.extractfile('xcn/values.yaml'))
        self.assertEqual(values['images']['runtime']['tag'], '20261105')
        self.assertEqual(values['images']['webui']['tag'], '20261105')
        self.assertEqual(values['monitor']['image']['tag'], '20261105')
        self.check_clean()

    def test_invalid_arguments_fail_before_docker_or_build(self):
        for args in ([], ['0.2.0'], ['0.2.0', 'test', 'extra'], ['', 'test'],
                     ['0.2', 'test'], ['01.2.3', 'test'], ['0.2.0-01', 'test'],
                     ['0.2.0', ''], ['0.2.0', 'bad/tag'], ['0.2.0', 'bad+tag'],
                     ['0.2.0', 'x' * 129]):
            with self.subTest(args=args):
                result = self.run_release(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.calls(), [])
                self.assertFalse(any(path.exists() for path in self.artifacts()))
                self.check_clean()

    def test_help_does_not_contact_docker(self):
        for option in ('-h', '--help'):
            result = self.run_release(option)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('<chartVersion> <appVersion>', result.stdout)
        self.assertEqual(self.calls(), [])

    def test_failures_preserve_existing_artifacts_and_stop_later_steps(self):
        for env, status, steps in (
                ({'DOCKER_FAILURE': '11'}, 11, ['docker']),
                ({'BUILD_FAILURE': '9'}, 9, ['docker', 'build-images.sh']),
                ({'SAVE_FAILURE': 'webui'}, 7, ['docker', 'build-images.sh', 'docker', 'docker']),
                ({'PACKAGE_FAILURE': '1'}, 8,
                 ['docker', 'build-images.sh', 'docker', 'docker', 'docker', 'helm'])):
            with self.subTest(env=env):
                (self.root / 'calls.jsonl').unlink(missing_ok=True)
                for path in self.artifacts():
                    path.write_bytes(b'previous release')
                result = self.run_release('0.2.0', 'test', **env)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual([name for name, _ in self.calls()], steps)
                for path in self.artifacts():
                    self.assertEqual(path.read_bytes(), b'previous release')
                self.check_clean()


if __name__ == '__main__':
    unittest.main()
