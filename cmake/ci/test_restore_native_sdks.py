"""Offline tests for the sole SDK configuration file and non-secret root updates."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).with_name('restore-native-sdks.py')
SPEC = importlib.util.spec_from_file_location('restore_native_sdks', SCRIPT)
restore = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(restore)


class EnvironmentFileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'sdk.env'

    def write(self, content):
        self.path.write_text(content, encoding='utf-8-sig')

    def test_repository_example_supplies_every_required_release(self):
        environment = {}
        selected = restore.load_environment_file(
            SCRIPT.parents[2] / '.env.example', environment)
        self.assertTrue(set(restore.VERSIONS.values()) <= selected.keys())
        with patch.dict(os.environ, environment, clear=True):
            for key in restore.VERSIONS.values():
                self.assertEqual(restore.version(key), selected[key])

    def test_shared_installations_are_selected_by_the_environment_file(self):
        self.write('VCPKG_INSTALLED_DIR=C:/shared/vcpkg_installed\n'
                   'RE2C_ROOT=C:/shared/re2c\nLEMON_ROOT=C:/shared/lemon\n')
        selected = restore.load_environment_file(self.path, {})
        self.assertEqual(selected, {
            'VCPKG_INSTALLED_DIR': 'C:/shared/vcpkg_installed',
            'RE2C_ROOT': 'C:/shared/re2c', 'LEMON_ROOT': 'C:/shared/lemon'})

    def test_file_selects_build_settings_and_preserves_parent_credentials(self):
        self.write('SALTS_BASE_VERSION=0.1.8\nLLVM_MINGW_ROOT=C:/example\n'
                   'CMAKE_BUILD_PARALLEL_LEVEL=3\nGITHUB_TOKEN=file-secret\n')
        environment = {'SALTS_BASE_VERSION': '0.1.9', 'LLVM_MINGW_ROOT': 'C:/actual',
                       'GITHUB_TOKEN': 'parent-secret'}
        selected = restore.load_environment_file(self.path, environment)
        self.assertEqual(selected['SALTS_BASE_VERSION'], '0.1.8')
        self.assertEqual(selected['LLVM_MINGW_ROOT'], 'C:/example')
        self.assertEqual(selected['CMAKE_BUILD_PARALLEL_LEVEL'], '3')
        self.assertEqual(environment['GITHUB_TOKEN'], 'parent-secret')
        self.assertNotIn('GITHUB_TOKEN', selected)

    def test_android_selects_a_native_sdk_profile_for_the_idl_compiler(self):
        self.assertEqual(restore.android_idlc_profile('linux-x64'), 'linux-gcc-x64')
        for host in ('linux-x64', 'linux-arm64', 'windows-x64', 'windows-arm64', 'macos-arm64'):
            with self.subTest(host=host):
                profile = restore.android_idlc_profile(host)
                self.assertEqual(restore.SDK_PROFILES[profile][0], host)

    def test_windows_native_arm64_machine_name(self):
        with patch.object(restore.platform, 'system', return_value='Windows'):
            with patch.object(restore.platform, 'machine', return_value='ARM64'):
                self.assertEqual(restore.host_rid(), 'windows-arm64')

    def test_unknown_fields_do_not_enter_environment_or_output(self):
        self.write('GITHUB_TOKEN=file-secret\nUNRELATED_SETTING=value\n')
        environment = {}
        self.assertEqual(restore.load_environment_file(self.path, environment), {})
        self.assertEqual(environment, {})

    def test_quoted_paths_and_cache_values_remain_literal(self):
        self.write('# Defaults\n\n LLVM_MINGW_ROOT = "C:/compiler path"\n'
                   "VCPKG_BINARY_SOURCES='clear;files,$(ignored)#cache,read'\n")
        selected = restore.load_environment_file(self.path, {})
        self.assertEqual(selected['LLVM_MINGW_ROOT'], 'C:/compiler path')
        self.assertEqual(selected['VCPKG_BINARY_SOURCES'],
                         'clear;files,$(ignored)#cache,read')

    def test_invalid_files_leave_environment_unchanged(self):
        for invalid in ('missing separator', 'SALTS_BASE_VERSION="0.1.8',
                        'SALTS_BASE_VERSION=0.1.9'):
            with self.subTest(invalid=invalid):
                self.write('SALTS_BASE_VERSION=0.1.8\n' + invalid + '\n')
                environment = {'LLVM_MINGW_ROOT': 'C:/actual'}
                with self.assertRaises(ValueError):
                    restore.load_environment_file(self.path, environment)
                self.assertEqual(environment, {'LLVM_MINGW_ROOT': 'C:/actual'})

    def test_missing_selected_file_fails_without_changing_environment(self):
        environment = {}
        with self.assertRaises(FileNotFoundError):
            restore.load_environment_file(self.path, environment)
        self.assertEqual(environment, {})

    def test_main_loads_repository_env_before_dependency_validation(self):
        self.write(''.join(f'{key}=0.1.0\n' for key in restore.VERSIONS.values())
                   + 'VCPKG_CACHE_REPOSITORY_ROOT=C:/selected\n'
                   + 'VCPKG_TARGET_TRIPLET=x64-windows\n')
        loader = restore.load_environment_file
        with patch.dict(os.environ, {}, clear=True), patch.object(
                restore, 'load_environment_file',
                side_effect=lambda path, environment: loader(self.path, environment)) as load, patch.object(
                sys, 'argv', [str(SCRIPT), '--rid', 'windows-x64']), patch.object(
                restore, 'run', side_effect=RuntimeError('stop before network')) as run:
            with self.assertRaisesRegex(RuntimeError, 'stop before network'):
                restore.main()
            self.assertEqual(os.environ['CHTTP_VERSION'], '0.1.0')
            self.assertEqual(load.call_args.args[0], SCRIPT.parents[2] / '.env')
            self.assertEqual(run.call_args.args[0], 'git')

    def test_profile_mismatch_fails_before_querying_or_downloading_dependencies(self):
        self.write('VCPKG_TARGET_TRIPLET=qigao-x64-windows-llvm-mingw\n')
        loader = restore.load_environment_file
        with patch.dict(os.environ, {}, clear=True), patch.object(
                restore, 'load_environment_file',
                side_effect=lambda path, environment: loader(self.path, environment)), patch.object(
                sys, 'argv', [str(SCRIPT), '--rid', 'windows-x64',
                              '--profile', 'windows-msvc-x64']), patch.object(restore, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'VCPKG_TARGET_TRIPLET'):
                restore.main()
            run.assert_not_called()

    def test_restored_roots_preserve_credentials_comments_and_machine_settings(self):
        original = (b'# selected machine\r\nGITHUB_TOKEN=secret-value\r\n'
                    b'CHTTP_ROOT=C:/old\r\nMACHINE_SETTING="keep = literal"\r\n')
        self.path.write_bytes(original)
        restore.write_environment_file(self.path, {
            'CHTTP_ROOT': 'C:/shared/chttp-0.1.2', 'SALTS_CORE_ROOT': 'C:/shared/cnet'})
        updated = self.path.read_bytes()
        self.assertIn(b'GITHUB_TOKEN=secret-value\r\n', updated)
        self.assertIn(b'MACHINE_SETTING="keep = literal"\r\n', updated)
        self.assertIn(b'# selected machine\r\n', updated)
        self.assertEqual(updated.count(b'CHTTP_ROOT='), 1)
        selected = restore.load_environment_file(self.path, {})
        self.assertEqual(selected['SALTS_CORE_ROOT'], 'C:/shared/cnet')
        self.assertNotIn('GITHUB_TOKEN', selected)


if __name__ == '__main__':
    unittest.main()
