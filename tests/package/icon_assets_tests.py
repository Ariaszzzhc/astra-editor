"""Tampered icon resources must fail before they can enter a binary/package."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location('prepare_icons', ROOT / 'scripts/prepare-icons.py')
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)


class IconIntegrityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='icon-integrity-', dir=ROOT / 'build')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        shutil.copytree(ROOT / 'resources/icons', self.root / 'resources/icons')
        shutil.copytree(ROOT / 'dependencies/icons', self.root / 'dependencies/icons')
        self.manifest_path = self.root / 'dependencies/icons/manifest.json'
        self.manifest = json.loads(self.manifest_path.read_text('utf-8'))

    def test_changed_svg(self):
        target = self.root / self.manifest['assets'][0]['path']
        target.write_bytes(target.read_bytes() + b'<!--changed-->')
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            PREPARE.prepare(self.root, self.root / 'generated')

    def test_changed_notice(self):
        (self.root / self.manifest['notices'][0]['path']).write_text('missing attribution')
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            PREPARE.prepare(self.root, self.root / 'generated')

    def test_untracked_svg(self):
        (self.root / 'resources/icons/extra.svg').write_text('<svg/>')
        with self.assertRaisesRegex(ValueError, 'membership'):
            PREPARE.prepare(self.root, self.root / 'generated')

    def test_external_content_even_with_updated_digest(self):
        item = self.manifest['assets'][0]
        data = b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><image href="https://example.com/icon.png"/></svg>'
        (self.root / item['path']).write_bytes(data)
        item['sha256'] = hashlib.sha256(data).hexdigest()
        self.manifest_path.write_text(json.dumps(self.manifest), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'Unsupported icon element'):
            PREPARE.prepare(self.root, self.root / 'generated')


if __name__ == '__main__':
    unittest.main()
