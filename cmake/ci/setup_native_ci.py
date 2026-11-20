#!/usr/bin/env python3
"""Prepare shared native dependencies; all resolution and transport belong to vcpkg-cache."""
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

SPEC = importlib.util.spec_from_file_location(
    "sdk_settings", Path(__file__).with_name("restore-native-sdks.py"))
settings = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(settings)


def run(*arguments):
    return subprocess.check_output(list(map(str, arguments)), text=True).strip()


def host_rid():
    system = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}[platform.system()]
    architecture = {"AMD64": "x64", "x86_64": "x64",
                    "aarch64": "arm64", "arm64": "arm64"}[platform.machine()]
    return system + "-" + architecture


def prepare(root, configuration, host, execute=run):
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
    if release["draft"] or release["tag_name"] != "v" + version:
        raise ValueError("Expected the explicitly selected infrastructure release")
    for package, variable, binary, extra in (
            ("Qigao.Re2c.Binary", "RE2C_ROOT", "re2c", ()),
            ("Qigao.Lemon.Binary", "LEMON_ROOT", "lemon",
             ("share/lemon/lempar.c", "share/lemon/source.json"))):
        # The pinned shared release declares the tool identity and archive digest.
        asset, = [item for item in release["assets"]
                  if item["name"].startswith(package + ".")]
        tool_version = asset["name"][len(package) + 1:].rsplit(".", 1)[0]
        lock = inputs / (package + ".lock.json")
        execute(sys.executable, helper, "resolve", package, "--version", tool_version,
                "--output-lock", lock)
        locked = json.loads(lock.read_text(encoding="utf-8"))
        if "sha256:" + locked["sha256"] != asset["digest"]:
            raise ValueError("Shared tool identity differs from its published release: " + package)
        executable = "bin/" + binary + (".exe" if host.startswith("windows-") else "")
        command = [sys.executable, helper, "locked", lock, "--destination",
                   destination / (package + "-" + tool_version + "-" + host + "-" +
                                  locked["sha256"][:12]),
                   "--payload", "tools/" + host, "--executable", executable, "--reuse"]
        for required in extra:
            command += ["--require", required]
        values[variable] = json.loads(execute(*command))["root"]
    return values


def main():
    root = Path(__file__).resolve().parents[2]
    configuration = settings.load_environment_file(root / ".env", os.environ)
    values = prepare(root, configuration, host_rid())
    settings.write_environment_file(root / ".env", values)
    print("Prepared shared vcpkg and host-tool installations")


if __name__ == "__main__":
    main()
