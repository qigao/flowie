"""Offline contracts for delegating native dependency preparation to shared infrastructure."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    'setup_native_ci', Path(__file__).with_name('setup_native_ci.py'))
setup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(setup)


class SharedPreparationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.infrastructure = self.root / 'infrastructure'
        self.configuration = {
            'VCPKG_CACHE_REPOSITORY_ROOT': str(self.infrastructure),
            'SHARED_DEPENDENCY_ROOT': str(self.root / 'shared'),
            'VCPKG_CACHE_VERSION': '0.1.3', 'VCPKG_TARGET_TRIPLET': 'x64-linux'}
        self.commands = []
        self.dirty = False
        self.mismatched_digest = False

    def execute(self, *arguments):
        command = tuple(map(str, arguments))
        self.commands.append(command)
        if command[0] == 'git':
            return 'modified' if self.dirty and 'status' in command else (
                '' if 'status' in command else 'release-commit')
        if command[0] == 'gh':
            return json.dumps({'draft': False, 'tag_name': 'v0.1.3', 'assets': [
                {'name': package + '.' + version + '.archive', 'digest': 'sha256:' + 'a' * 64}
                for package, version in [('Qigao.Re2c.Binary', '4.6.7'),
                                         ('Qigao.Lemon.Binary', '1.0.0')]]})
        if 'resolve-release' in command or 'resolve' in command:
            path = Path(command[command.index('--output-lock') + 1])
            path.write_text(json.dumps({'sha256': 'b' * 64 if self.mismatched_digest
                                        and 'resolve' in command else 'a' * 64}), encoding='utf-8')
            return '{}'
        self.assertIn('locked', command)
        return json.dumps({'root': command[command.index('--destination') + 1] + '/payload'})

    def test_shared_release_selects_roots_and_paired_host_generator_inputs(self):
        result = setup.prepare(self.root, self.configuration, 'linux-x64', self.execute)
        self.assertEqual(set(result), {'VCPKG_CACHE_REPOSITORY_ROOT', 'VCPKG_INSTALLED_DIR',
                                      'RE2C_ROOT', 'LEMON_ROOT'})
        restores = [command for command in self.commands if 'locked' in command]
        self.assertEqual(len(restores), 3)
        self.assertIn('x64-linux/include/xxhash.h', restores[0])
        self.assertIn('tools/linux-x64', restores[1])
        self.assertIn('bin/re2c', restores[1])
        self.assertIn('share/lemon/lempar.c', restores[2])
        self.assertIn('share/lemon/source.json', restores[2])
        self.assertTrue(all(str(self.infrastructure / 'scripts') in command[1]
                            for command in restores))

    def test_dirty_infrastructure_fails_before_dependency_resolution(self):
        self.dirty = True
        with self.assertRaisesRegex(ValueError, 'local changes'):
            setup.prepare(self.root, self.configuration, 'linux-x64', self.execute)
        self.assertTrue(all(command[0] == 'git' for command in self.commands))

    def test_conflicting_tool_archive_identity_is_not_restored(self):
        self.mismatched_digest = True
        with self.assertRaisesRegex(ValueError, 'identity differs'):
            setup.prepare(self.root, self.configuration, 'linux-x64', self.execute)
        self.assertEqual(sum('locked' in command for command in self.commands), 1)


if __name__ == '__main__':
    unittest.main()
