#!/usr/bin/env python3
"""Verify corruption, stale locks and archive boundaries fail closed."""
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location('xcn_vendor', Path(__file__).resolve().parents[1] / 'vendor.py')
V = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(V)


class VendorTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='xcn-vendor-test-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.vendor = self.root / 'docker/vendor'
        self.vendor.mkdir(parents=True)
        patch = mock.patch.multiple(V, ROOT=self.root, VENDOR=self.vendor)
        patch.start()
        self.addCleanup(patch.stop)

    def fixture(self, members=None):
        path = self.vendor / 'test.tar.gz'
        with tarfile.open(path, 'w:gz') as archive:
            for name, link in members or [('source/file', None)]:
                info = tarfile.TarInfo(name)
                if link:
                    info.type = tarfile.SYMTYPE
                    info.linkname = link
                    archive.addfile(info)
                else:
                    info.size = 1
                    archive.addfile(info, io.BytesIO(b'x'))
        self.data = {'format':1, 'platform':'linux/amd64', 'assets':{'test':{
            'components':['runtime'], 'files':[{'path':path.name, 'sha256':V.digest(path)}]}}, 'locks':{}}
        self.save()
        return path

    def save(self):
        (self.vendor / 'manifest.json').write_text(json.dumps(self.data))

    def test_corrupt_and_missing_archive(self):
        path = self.fixture()
        self.assertEqual(V.verify('runtime'), 1)
        path.write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'SHA-256'): V.verify('runtime')
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'missing'): V.verify('runtime')

    def test_stale_lock(self):
        self.fixture()
        lock = self.root / 'requirements.lock'
        lock.write_text('original')
        self.data['locks']['requirements.lock'] = {'sha256':V.digest(lock), 'components':['runtime']}
        self.save()
        lock.write_text('changed')
        with self.assertRaisesRegex(ValueError, 'changed'): V.verify('runtime')

    def test_safe_archive_extracts(self):
        self.fixture()
        V.unpack('test', self.root / 'out')
        self.assertEqual((self.root / 'out/source/file').read_bytes(), b'x')

    def test_escaping_paths_and_symlinks_rejected(self):
        for members in [[('../escape', None)], [('/absolute', None)], [('source/link', '../../escape')],
                        [('link', 'dir'), ('dir', '../escape')]]:
            with self.subTest(members=members):
                self.fixture(members)
                with tempfile.TemporaryDirectory(dir=self.root) as output:
                    with self.assertRaises(ValueError): V.unpack('test', output)


if __name__ == '__main__':
    unittest.main()
