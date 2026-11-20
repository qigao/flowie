"""Restore explicitly selected published SDKs before invoking CMake presets."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tarfile
import tempfile


def run(*args):
    return subprocess.check_output(list(map(str, args)), text=True).strip()


def version(name, environment=None):
    value = (os.environ if environment is None else environment)[name]
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?', value):
        raise ValueError(f'{name} must be an exact release version without v')
    return value


def host_rid():
    system = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}[platform.system()]
    architecture = {"AMD64": "x64", "x86_64": "x64",
                    "aarch64": "arm64", "arm64": "arm64"}[platform.machine()]
    return system + "-" + architecture


def android_idlc_profile(host):
    return 'linux-gcc-x64' if host == 'linux-x64' else PROFILES[host][0]


def load_environment_file(path, environment):
    """Load the selected .env as the sole source of non-secret build settings."""
    values = {}
    for number, line in enumerate(Path(path).read_text(encoding='utf-8-sig').splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, separator, value = line.partition('=')
        if not separator:
            raise ValueError(f'{path}:{number}: expected KEY=VALUE')
        name = name.strip()
        if name not in ENVIRONMENT_FILE_FIELDS:
            continue
        if name in values:
            raise ValueError(f'{path}:{number}: duplicate field {name}')
        value = value.strip()
        if value.startswith(('"', "'")):
            if len(value) < 2 or value[-1] != value[0]:
                raise ValueError(f'{path}:{number}: unmatched quotes for {name}')
            value = value[1:-1]
        values[name] = value
    # Validate the whole file before updating the process environment.
    for name, value in values.items():
        environment[name] = value
    return {name: environment[name] for name in values}


def write_environment_file(path, values):
    """Commit restored non-secret roots without rewriting credentials or other settings."""
    path = Path(path)
    lines = path.read_bytes().decode('utf-8-sig').splitlines(keepends=True)
    remaining = dict(values)
    for index, line in enumerate(lines):
        name, separator, _ = line.partition('=')
        name = name.strip()
        if separator and name in remaining:
            ending = '\r\n' if line.endswith('\r\n') else '\n'
            lines[index] = f'{name}={remaining.pop(name)}{ending}'
    if lines and not lines[-1].endswith('\n'):
        lines[-1] += '\n'
    lines.extend(f'{name}={value}\n' for name, value in remaining.items())
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='',
                                         dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(''.join(lines))
        temporary.chmod(path.stat().st_mode)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rid', required=True, choices=PROFILES)
    parser.add_argument('--profile', choices=SDK_PROFILES)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    environment_file = root / '.env'
    file_environment = load_environment_file(environment_file, os.environ)
    profile = args.profile or PROFILES[args.rid][0]
    if SDK_PROFILES[profile][0] != args.rid:
        raise ValueError('The SDK profile must match the target RID')
    triplet = SDK_PROFILES[profile][1]
    if file_environment['VCPKG_TARGET_TRIPLET'] != triplet:
        raise ValueError('The SDK profile must match VCPKG_TARGET_TRIPLET in .env')
    versions = {repo: version(key, file_environment) for repo, key in VERSIONS.items()}
    infra = Path(file_environment['VCPKG_CACHE_REPOSITORY_ROOT']).resolve()
    if run('git', '-C', infra, 'rev-parse', 'HEAD') != run(
            'git', '-C', infra, 'rev-parse', 'v' + versions['vcpkg-cache'] + '^{commit}'):
        raise ValueError('The infrastructure checkout must match VCPKG_CACHE_VERSION')
    if run('git', '-C', infra, 'status', '--porcelain'):
        raise ValueError('The selected infrastructure checkout has local changes')
    spec = importlib.util.spec_from_file_location(
        'archive_transport', infra / 'scripts/setup-compiler-toolchain.py')
    transport = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transport)
    destination = root / 'build/dependencies'
    archives = destination / 'archives'
    archives.mkdir(parents=True, exist_ok=True)
    environment = dict(file_environment)
    environment.update({key: versions[repo] for repo, key in VERSIONS.items()})
    environment.update(VCPKG_TARGET_TRIPLET=triplet,
                       VCPKG_CACHE_REPOSITORY_ROOT=infra.as_posix())
    environment['VCPKG_BINARY_SOURCES'] = file_environment['VCPKG_BINARY_SOURCES']
    for name in ('VCPKG_INSTALLED_DIR', 'RE2C_ROOT', 'LEMON_ROOT'):
        environment[name] = file_environment[name]
    if args.rid in ANDROID_ABIS:
        environment['ANDROID_ABI'] = ANDROID_ABIS[args.rid]
        idlc_profile = android_idlc_profile(host_rid())
    releases = {}

    def release(repo):
        if repo not in releases:
            selected = json.loads(run('gh', 'api',
                f'repos/qigao/{repo}/releases/tags/v{versions[repo]}'))
            if selected['draft'] or selected['tag_name'] != 'v' + versions[repo]:
                raise ValueError('Expected an exact published release: ' + repo)
            releases[repo] = selected
        return releases[repo]

    def asset(repo, name):
        items = [a for a in release(repo)['assets'] if a['name'] == name]
        if len(items) != 1 or not re.fullmatch(r'sha256:[0-9a-f]{64}', items[0].get('digest', '')):
            raise ValueError(f'Missing unique SHA-256-qualified asset: {repo}/{name}')
        return items[0]

    def download(repo, name):
        item = asset(repo, name)
        archive = archives / name
        if not archive.exists():
            transport.verified_download(item['browser_download_url'], archive,
                                        item['digest'][7:], transport.MAX_ARCHIVE)
        with archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != item['digest'][7:] or archive.stat().st_size != item['size']:
            raise ValueError('Release archive size/digest mismatch: ' + name)
        return archive, item

    def sdk(repo, prefix, config, selected_profile):
        name = f'{repo}-{versions[repo]}-{selected_profile}.tar'
        item = asset(repo, name)
        if repo == 'salts-base':
            metadata, _ = download(repo, f'{repo}-{versions[repo]}-manifest.json')
            manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
            producer, = [s for s in manifest['sdks'] if s['platform'] == selected_profile]
            if (manifest['schemaVersion'] != 1 or manifest['name'] != 'SaltsBase'
                    or manifest['repository'] != 'https://github.com/qigao/salts-base'
                    or manifest['version'] != versions[repo]
                    or manifest['tag'] != release(repo)['tag_name']
                    or manifest['dependencies']['release'] != 'v' + versions['vcpkg-cache']
                    or manifest['ci']['conclusion'] != 'success'
                    or not manifest['ci']['runId'] or producer['file'] != name
                    or producer['triplet'] != SDK_PROFILES[selected_profile][1]
                    or producer['sizeBytes'] != item['size']
                    or 'sha256:' + producer['sha256'] != item['digest']):
                raise ValueError('SaltsBase manifest does not qualify the selected SDK')
        elif repo == 'cmeta':
            metadata, _ = download(repo, f'{repo}-{versions[repo]}-manifest.json')
            manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
            producer, = [s for s in manifest['sdks'] if s['platform'] == selected_profile]
            if (manifest['schemaVersion'] != 1 or manifest['name'] != 'cmeta'
                    or manifest['version'] != versions[repo]
                    or manifest['repository'] != 'https://github.com/qigao/cmeta'
                    or manifest['ci']['conclusion'] != 'success'
                    or producer['file'] != name or producer['sizeBytes'] != item['size']
                    or 'sha256:' + producer['sha256'] != item['digest']
                    or manifest['dependencies']['vcpkg-cache'] != versions['vcpkg-cache']):
                raise ValueError('CMeta manifest does not qualify the selected SDK')
            required_base = manifest['dependencies']['SaltsBase']
            if required_base != versions['salts-base']:
                raise ValueError(f'CMeta {versions[repo]} release requires SaltsBase '
                                 f'{required_base}; selected {versions["salts-base"]}. '
                                 'A matching published CMeta SDK is required.')
        elif repo == 'chttp':
            metadata, _ = download(repo, f'{repo}-{versions[repo]}-manifest.json')
            manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
            producer, = [s for s in manifest['sdks'] if s['profile'] == selected_profile]
            dependencies = manifest['dependencies']
            # CMeta publishes SameMinorVersion compatibility. A selected patch
            # upgrade must still match Base exactly via its own release manifest.
            cmeta_version = tuple(map(int, versions['cmeta'].split('.')))
            cmeta_minimum = tuple(map(int, dependencies['CMeta'].split('.')))
            run_info = json.loads(run('gh', 'api',
                f'repos/qigao/chttp/actions/runs/{manifest["producer_run"]}'))
            if (manifest['schema'] != 1 or manifest['repository'] != 'qigao/chttp'
                    or manifest['cmake_package'] != 'Chttp'
                    or manifest['version'] != versions[repo]
                    or run_info['conclusion'] != 'success'
                    or run_info['head_sha'] != manifest['commit']
                    or producer['asset'] != name or producer['sizeBytes'] != item['size']
                    or 'sha256:' + producer['sha256'] != item['digest']
                    or producer['sdk_manifest']['profile'] != selected_profile
                    or cmeta_version[:2] != cmeta_minimum[:2]
                    or cmeta_version < cmeta_minimum
                    or any(dependencies[key] != versions[dependency] for key, dependency in (
                        ('SaltsBase', 'salts-base'),
                        ('SaltsCNet', 'salts-cnet'), ('SaltsUtils', 'salts-utils'),
                        ('vcpkg-cache', 'vcpkg-cache')))):
                raise ValueError('CHttp manifest does not qualify the selected SDK/dependency set')
        elif repo == 'turbodb':
            metadata, _ = download(repo, f'{repo}-{versions[repo]}-manifest.json')
            manifest = json.loads(metadata.read_text(encoding='utf-8-sig'))
            producer, = [s for s in manifest['sdks'] if s['profile'] == selected_profile]
            run_info = json.loads(run('gh', 'api',
                f'repos/qigao/turbodb/actions/runs/{manifest["producer_run"]}'))
            if (manifest['schema'] != 1 or manifest['name'] != 'TurboDB'
                    or manifest['version'] != versions[repo]
                    or manifest['tag'] != release(repo)['tag_name']
                    or run_info['conclusion'] != 'success'
                    or run_info['head_sha'] != manifest['commit']
                    or producer['file'] != name or producer['sizeBytes'] != item['size']
                    or 'sha256:' + producer['sha256'] != item['digest']
                    or producer['sdk_manifest']['profile'] != selected_profile
                    or producer['sdk_manifest']['cache_version'] != versions['vcpkg-cache']):
                raise ValueError('TurboDB manifest does not qualify the selected SDK')
        archive, item = download(repo, name)
        target = destination / f'{repo}-{versions[repo]}-{selected_profile}'
        receipt = dict(repository=repo, version=versions[repo], profile=selected_profile,
                       sha256=item['digest'][7:])
        if target.exists():
            if json.loads((target / 'receipt.json').read_text()) != receipt:
                raise ValueError('SDK receipt mismatch; choose a fresh destination: ' + str(target))
        else:
            with tempfile.TemporaryDirectory(dir=destination) as temporary:
                staging = Path(temporary) / 'sdk'
                staging.mkdir()
                with tarfile.open(archive) as package:
                    package.extractall(staging, filter='data')
                (staging / 'receipt.json').write_text(json.dumps(receipt) + '\n')
                staging.rename(target)
        if not (target / config).is_file():
            raise ValueError('Incomplete SDK: ' + str(target / config))
        environment[prefix + '_ROOT'] = target.as_posix()
        print(f'Restored {repo} {versions[repo]} ({selected_profile})', file=sys.stderr)

    sdk('salts-base', 'SALTS_BASE', 'lib/cmake/SaltsBase/SaltsBaseConfig.cmake', profile)
    sdk('cmeta', 'CMETA', 'lib/cmake/CMeta/CMetaConfig.cmake', profile)
    sdk('salts-utils', 'SALTS_UTILS', 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake', profile)
    if args.rid in ANDROID_ABIS:
        sdk('salts-base', 'SALTS_IDLC_BASE', 'lib/cmake/SaltsBase/SaltsBaseConfig.cmake',
            idlc_profile)
        sdk('salts-utils', 'SALTS_IDLC', 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake',
            idlc_profile)
    cnet_profile = {'windows-llvm-mingw-x64': 'windows-x64',
                    'linux-clang-x64': 'linux-x64', 'linux-clang-arm64': 'linux-arm64',
                    'macos-clang-arm64': 'macos-arm64',
                    'macos-clang-x64': 'macos-x64'}.get(profile, profile)
    sdk('salts-cnet', 'SALTS_CORE', 'lib/cmake/SaltsCNet/SaltsCNetConfig.cmake', cnet_profile)
    environment['SALTS_CNET_ROOT'] = environment['SALTS_CORE_ROOT']
    sdk('chttp', 'CHTTP', 'lib/cmake/Chttp/ChttpConfig.cmake', profile)
    sdk('turbodb', 'TURBODB', 'lib/cmake/TurboDB/TurboDBConfig.cmake', profile)
    write_environment_file(environment_file, environment)
    output = root / 'build/native-sdk-environment.json'
    output.write_text(json.dumps(environment, indent=2) + '\n')
    print(json.dumps(environment))


VERSIONS = {'vcpkg-cache': 'VCPKG_CACHE_VERSION', 'salts-base': 'SALTS_BASE_VERSION',
            'cmeta': 'CMETA_VERSION', 'salts-utils': 'SALTS_UTILS_VERSION',
            'salts-cnet': 'SALTS_CORE_VERSION', 'chttp': 'CHTTP_VERSION',
            'turbodb': 'TURBODB_VERSION'}
ENVIRONMENT_FILE_FIELDS = frozenset(VERSIONS.values()) | {
    'VCPKG_ROOT', 'VCPKG_CACHE_REPOSITORY_ROOT', 'VCPKG_BINARY_SOURCES',
    'VSDEVCMD_BAT', 'CMAKE_BUILD_PARALLEL_LEVEL', 'MSVC_TARGET_TRIPLET',
    'LLVM_NATIVE_TARGET_TRIPLET', 'GCC_TARGET_TRIPLET', 'GCC_C_COMPILER',
    'GCC_CXX_COMPILER', 'LLVM_MINGW_ROOT', 'LLVM_MINGW_TARGET',
    'LLVM_MINGW_TOOLCHAIN_FILE', 'ANDROID_NDK_HOME', 'ANDROID_ABI', 'TURBODB_ROOT',
    'VCPKG_INSTALLED_DIR', 'VCPKG_TARGET_TRIPLET', 'RE2C_ROOT', 'LEMON_ROOT',
    'SHARED_DEPENDENCY_ROOT',
    'OPENWTL_VERSION', 'OPENWTL_SHA256',
    'TURBORAFT_VERSION', 'TURBORAFT_ROOT',
    'SALTS_BASE_ROOT', 'CMETA_ROOT', 'SALTS_UTILS_ROOT', 'SALTS_CORE_ROOT',
    'SALTS_CNET_ROOT', 'SALTS_CNET_VERSION', 'CHTTP_ROOT',
    'SALTS_IDLC_ROOT', 'SALTS_IDLC_BASE_ROOT'
}
ANDROID_ABIS = {'android-arm64-v8a': 'arm64-v8a', 'android-x86_64': 'x86_64'}
PROFILES = {'windows-x64': ('windows-msvc-x64', 'x64-windows'),
            'linux-x64': ('linux-clang-x64', 'x64-linux'),
            'linux-arm64': ('linux-clang-arm64', 'arm64-linux'),
            'macos-arm64': ('macos-clang-arm64', 'arm64-osx'),
            'macos-x64': ('macos-clang-x64', 'x64-osx'),
            'android-arm64-v8a': ('android-arm64', 'arm64-android'),
            'android-x86_64': ('android-x64', 'x64-android')}
SDK_PROFILES = {profile: (rid, triplet) for rid, (profile, triplet) in PROFILES.items()}
SDK_PROFILES.update({'windows-llvm-mingw-x64': ('windows-x64', 'qigao-x64-windows-llvm-mingw'),
                     'linux-gcc-x64': ('linux-x64', 'x64-linux'),
                     'linux-gcc-arm64': ('linux-arm64', 'arm64-linux')})

if __name__ == '__main__':
    main()
