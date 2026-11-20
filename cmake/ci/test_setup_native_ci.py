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
        self.bad_tool_digest = False
        self.bad_download_size = False
        self.downloads = []

    def download(self, url, archive, digest, size):
        self.downloads.append((url, digest, size))
        archive.write_bytes(b"x" * (size - self.bad_download_size))

    def execute(self, *arguments):
        command = tuple(map(str, arguments))
        self.commands.append(command)
        if command[0] == 'git':
            return 'modified' if self.dirty and 'status' in command else (
                '' if 'status' in command else 'release-commit')
        if command[0] == 'gh':
            names = ['Qigao.Re2c.Binary.4.6.7.nupkg', 'Qigao.Lemon.Binary.1.0.0.nupkg']
            return json.dumps({'draft': False, 'published_at': '2026-01-01T00:00:00Z',
                               'tag_name': 'v0.1.3', 'assets': [
                {'name': name, 'digest': 'sha256:' + ('invalid' if self.bad_tool_digest else 'a' * 64),
                 'size': 12, 'browser_download_url':
                    'https://github.com/qigao/vcpkg-cache/releases/download/v0.1.3/' + name}
                for name in names]})
        if 'resolve-release' in command:
            path = Path(command[command.index('--output-lock') + 1])
            path.write_text(json.dumps({'sha256': 'a' * 64}), encoding='utf-8')
            return '{}'
        if '--archive' in command:
            self.assertEqual(command[2], 'package')
            self.assertEqual(Path(command[command.index('--archive') + 1]).read_bytes(), b"x" * 12)
        else:
            self.assertIn('locked', command)
        return json.dumps({'root': command[command.index('--destination') + 1] + '/payload'})

    def test_shared_release_selects_roots_and_paired_host_generator_inputs(self):
        result = setup.prepare(self.root, self.configuration, 'linux-x64', self.execute, self.download)
        self.assertEqual(set(result), {'VCPKG_CACHE_REPOSITORY_ROOT', 'VCPKG_INSTALLED_DIR',
                                      'RE2C_ROOT', 'LEMON_ROOT'})
        restores = [command for command in self.commands if command[2] in ('locked', 'package')]
        self.assertEqual(len(restores), 3)
        self.assertIn('x64-linux/include/xxhash.h', restores[0])
        self.assertIn('tools/linux-x64', restores[1])
        self.assertEqual(restores[1][2:5], ('package', 'Qigao.Re2c.Binary', '4.6.7'))
        self.assertEqual(restores[2][2:5], ('package', 'Qigao.Lemon.Binary', '1.0.0'))
        self.assertIn('bin/re2c', restores[1])
        self.assertIn('share/lemon/lempar.c', restores[2])
        self.assertIn('share/lemon/source.json', restores[2])
        self.assertEqual(len(self.downloads), 2)
        self.assertTrue(all('/releases/download/v0.1.3/' in url for url, _, _ in self.downloads))
        expected_scripts = self.infrastructure.resolve() / 'scripts'
        for command in restores:
            self.assertEqual(Path(command[1]).parent, expected_scripts)

    def test_dirty_infrastructure_fails_before_dependency_resolution(self):
        self.dirty = True
        with self.assertRaisesRegex(ValueError, 'local changes'):
            setup.prepare(self.root, self.configuration, 'linux-x64', self.execute, self.download)
        self.assertTrue(all(command[0] == 'git' for command in self.commands))

    def test_conflicting_tool_archive_identity_is_not_restored(self):
        self.bad_tool_digest = True
        with self.assertRaisesRegex(ValueError, 'does not qualify the tool archive'):
            setup.prepare(self.root, self.configuration, 'linux-x64', self.execute, self.download)
        self.assertEqual(sum('locked' in command for command in self.commands), 1)
        self.assertEqual(self.downloads, [])

    def test_download_size_mismatch_stops_before_extracting_the_tool(self):
        self.bad_download_size = True
        with self.assertRaisesRegex(ValueError, 'size differs'):
            setup.prepare(self.root, self.configuration, 'linux-x64', self.execute, self.download)
        self.assertFalse(any('--archive' in command for command in self.commands))


class CIEnvironmentTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.template = self.root / '.env.example'
        example = Path(__file__).resolve().parents[2] / '.env.example'
        self.template.write_text(example.read_text(encoding='utf-8') + '\nGH_TOKEN=fixture\n',
                                 encoding='utf-8')

    def test_msvc_selects_published_triplet_and_openwtl_without_local_paths(self):
        values = setup.materialize_environment(self.root, 'windows-msvc-x64', {})
        self.assertEqual(values['VCPKG_TARGET_TRIPLET'], 'x64-windows')
        self.assertEqual(len(values['OPENWTL_SHA256']), 64)
        self.assertNotIn('LLVM_MINGW_ROOT', values)
        self.assertNotIn('GH_TOKEN', (self.root / '.env').read_text())
        self.assertEqual(setup.settings.load_environment_file(self.root / '.env', {}), values)
        with self.assertRaises(FileExistsError):
            setup.materialize_environment(self.root, 'linux-gcc-x64', {})

    def test_linux_needs_only_portable_versions_and_selects_its_triplet(self):
        values = setup.materialize_environment(self.root, 'linux-gcc-x64', {})
        self.assertEqual(values['VCPKG_TARGET_TRIPLET'], 'x64-linux')
        self.assertNotIn('OPENWTL_VERSION', values)
        self.assertNotIn('ANDROID_NDK_HOME', values)

    def test_android_requires_a_runner_ndk_before_creating_the_file(self):
        with self.assertRaisesRegex(KeyError, 'ANDROID_NDK_HOME'):
            setup.materialize_environment(self.root, 'android-arm64', {})
        self.assertFalse((self.root / '.env').exists())
        ndk = self.root / 'ndk'
        tools = ndk / 'build/cmake'
        tools.mkdir(parents=True)
        (tools / 'android.toolchain.cmake').touch()
        values = setup.materialize_environment(self.root, 'android-arm64',
                                                {'ANDROID_NDK_HOME': str(ndk)})
        self.assertEqual(values['ANDROID_NDK_HOME'], ndk.resolve().as_posix())
        self.assertEqual(values['VCPKG_TARGET_TRIPLET'], 'arm64-android')

    def test_invalid_openwtl_hash_stops_before_creating_the_file(self):
        path = self.template
        values = setup.settings.load_environment_file(path, {})
        path.write_text(path.read_text().replace(values['OPENWTL_SHA256'], 'invalid'))
        with self.assertRaisesRegex(ValueError, 'OPENWTL_SHA256'):
            setup.materialize_environment(self.root, 'windows-msvc-x64', {})
        self.assertFalse((self.root / '.env').exists())


if __name__ == '__main__':
    unittest.main()
