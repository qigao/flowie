#!/usr/bin/env python3
"""Prepare shared native dependencies; all resolution and transport belong to vcpkg-cache."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile

SPEC = importlib.util.spec_from_file_location(
    "sdk_settings", Path(__file__).with_name("restore-native-sdks.py"))
settings = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(settings)

CI_PROFILES = ("linux-gcc-x64", "windows-msvc-x64", "android-arm64")
INFRASTRUCTURE = "build/dependencies/vcpkg-cache"
SHARED_INSTALLATION = "build/dependencies/shared"


def materialize_environment(root, profile, runner_environment):
    """Create the job's single .env without importing machine-specific settings."""
    rid, triplet = settings.SDK_PROFILES[profile]
    template = settings.load_environment_file(root / ".env.example", {})
    versions = {key: settings.version(key, template) for key in settings.VERSIONS.values()}
    values = dict(versions, VCPKG_CACHE_REPOSITORY_ROOT=INFRASTRUCTURE,
                  SHARED_DEPENDENCY_ROOT=SHARED_INSTALLATION, VCPKG_TARGET_TRIPLET=triplet,
                  VCPKG_BINARY_SOURCES="clear")
    if rid == "windows-x64":
        openwtl_version = template["OPENWTL_VERSION"]
        if not re.fullmatch(r"\d+\.\d+\.\d+", openwtl_version):
            raise ValueError("OPENWTL_VERSION in .env.example must select an exact release")
        digest = template["OPENWTL_SHA256"]
        if not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ValueError("OPENWTL_SHA256 in .env.example must be a SHA-256 digest")
        values.update(OPENWTL_VERSION=openwtl_version, OPENWTL_SHA256=digest)
    if rid == "android-arm64-v8a":
        ndk = Path(runner_environment["ANDROID_NDK_HOME"]).resolve(strict=True)
        if not (ndk / "build/cmake/android.toolchain.cmake").is_file():
            raise ValueError("ANDROID_NDK_HOME must point to the runner's installed NDK")
        values["ANDROID_NDK_HOME"] = ndk.as_posix()
    with (root / ".env").open("x", encoding="utf-8", newline="\n") as output:
        output.writelines(f"{key}={value}\n" for key, value in values.items())
    return values


def run(*arguments):
    return subprocess.check_output(list(map(str, arguments)), text=True).strip()


def host_rid():
    system = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}[platform.system()]
    architecture = {"AMD64": "x64", "x86_64": "x64",
                    "aarch64": "arm64", "arm64": "arm64"}[platform.machine()]
    return system + "-" + architecture


def archive_transport(infrastructure):
    spec = importlib.util.spec_from_file_location(
        "archive_transport", infrastructure / "scripts/setup-compiler-toolchain.py")
    transport = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transport)
    return transport.verified_download


def prepare(root, configuration, host, execute=run, download=None):
    infrastructure = (root / configuration["VCPKG_CACHE_REPOSITORY_ROOT"]).resolve()
    destination = (root / configuration["SHARED_DEPENDENCY_ROOT"]).resolve()
    version = settings.version("VCPKG_CACHE_VERSION", configuration)
    triplet = configuration["VCPKG_TARGET_TRIPLET"]
    if execute("git", "-C", infrastructure, "rev-parse", "HEAD") != execute(
            "git", "-C", infrastructure, "rev-parse", "v" + version + "^{commit}"):
        raise ValueError("The shared infrastructure checkout must match .env")
    if execute("git", "-C", infrastructure, "status", "--porcelain"):
        raise ValueError("The shared infrastructure checkout has local changes")

    inputs = infrastructure / "build" / "native-inputs" / version / triplet
    inputs.mkdir(parents=True, exist_ok=True)
    helper = infrastructure / "scripts" / "restore-native-dependency.py"
    descriptor = inputs / "vcpkg-release.json"
    descriptor.write_text(json.dumps({"schema": 1, "source": {
        "kind": "github-release", "repository": "qigao/vcpkg-cache",
        "version_env": "VCPKG_CACHE_VERSION",
        "asset": "installed-" + triplet + ".tar.xz"}}) + "\n", encoding="utf-8")
    lock = inputs / "vcpkg.lock.json"
    execute(sys.executable, helper, "resolve-release", descriptor, "--output-lock", lock)
    digest = json.loads(lock.read_text(encoding="utf-8"))["sha256"]
    receipt = json.loads(execute(
        sys.executable, helper, "locked", lock, "--destination",
        destination / ("vcpkg-" + version + "-" + triplet + "-" + digest[:12]),
        "--payload", "vcpkg_installed", "--require", triplet + "/include/xxhash.h", "--reuse"))
    values = {"VCPKG_CACHE_REPOSITORY_ROOT": infrastructure.as_posix(),
              "VCPKG_INSTALLED_DIR": receipt["root"]}

    release = json.loads(execute(
        "gh", "api", "repos/qigao/vcpkg-cache/releases/tags/v" + version))
    tag = "v" + version
    if release["draft"] or not release["published_at"] or release["tag_name"] != tag:
        raise ValueError("Expected the explicitly selected infrastructure release")
    download = download or archive_transport(infrastructure)
    for package, variable, binary, extra in (
            ("Qigao.Re2c.Binary", "RE2C_ROOT", "re2c", ()),
            ("Qigao.Lemon.Binary", "LEMON_ROOT", "lemon",
             ("share/lemon/lempar.c", "share/lemon/source.json"))):
        # The pinned shared release declares the tool identity and archive digest.
        pattern = re.compile(re.escape(package) + r"\.(\d+\.\d+\.\d+)\.nupkg")
        asset, = [item for item in release["assets"] if pattern.fullmatch(item["name"])]
        tool_version = pattern.fullmatch(asset["name"])[1]
        digest = asset["digest"]
        size = asset["size"]
        url = f"https://github.com/qigao/vcpkg-cache/releases/download/{tag}/{asset['name']}"
        if (not re.fullmatch(r"sha256:[0-9a-f]{64}", digest)
                or type(size) is not int or size <= 0 or asset["browser_download_url"] != url):
            raise ValueError("The shared release does not qualify the tool archive: " + package)
        sha256 = digest[7:]
        executable = "bin/" + binary + (".exe" if host.startswith("windows-") else "")
        command = [sys.executable, helper, "package", package, tool_version,
                   "--sha256", sha256, "--destination",
                   destination / (package + "-" + tool_version + "-" + host + "-" +
                                  sha256[:12]),
                   "--payload", "tools/" + host, "--executable", executable, "--reuse"]
        for required in extra:
            command += ["--require", required]
        # The shared package command checks the published archive's identity and extracts it.
        with tempfile.TemporaryDirectory(dir=inputs) as temporary:
            archive = Path(temporary) / asset["name"]
            download(url, archive, sha256, size)
            if archive.stat().st_size != size:
                raise ValueError("Shared tool archive size differs from its release: " + package)
            values[variable] = json.loads(execute(*command, "--archive", archive))["root"]
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--materialize", metavar="PROFILE", choices=CI_PROFILES)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.materialize:
        outputs = Path(os.environ["GITHUB_OUTPUT"])
        values = materialize_environment(root, args.materialize, os.environ)
        with outputs.open("a", encoding="utf-8") as output:
            output.write("infrastructure=" + values["VCPKG_CACHE_REPOSITORY_ROOT"] + "\n")
            output.write("ref=v" + values["VCPKG_CACHE_VERSION"] + "\n")
        return
    configuration = settings.load_environment_file(root / ".env", os.environ)
    values = prepare(root, configuration, host_rid())
    settings.write_environment_file(root / ".env", values)
    print("Prepared shared vcpkg and host-tool installations")


if __name__ == "__main__":
    main()
