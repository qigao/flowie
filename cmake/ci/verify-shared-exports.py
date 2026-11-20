#!/usr/bin/env python3
import argparse
import pathlib
import re
import subprocess
import sys


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_allowlist(path: pathlib.Path) -> set[str]:
    symbols = {
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    if not symbols:
        fail(f"empty export allowlist: {path}")
    return symbols


def read_public_declarations(path: pathlib.Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r"TOAD_SCRIPT_C_API\s+[^;{]+?\b([A-Za-z_]\w*)\s*\(",
        re.DOTALL,
    )
    symbols = set(pattern.findall(text))
    if not symbols:
        fail(f"no TOAD_SCRIPT_C_API declarations found in {path}")
    return symbols


def run_nm(library: pathlib.Path, platform: str) -> set[str]:
    commands: list[list[str]]
    if platform == "macos":
        commands = [
            ["nm", "-gUj", str(library)],
            ["llvm-nm", "-gUj", str(library)],
        ]
    else:
        commands = [
            ["nm", "-D", "--defined-only", "--format=posix", str(library)],
            ["llvm-nm", "-D", "--defined-only", "--format=posix", str(library)],
        ]

    last_error = ""
    for command in commands:
        try:
            result = subprocess.run(
                command,
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except FileNotFoundError as exc:
            last_error = str(exc)
            continue
        if result.returncode != 0:
            last_error = result.stderr.strip()
            continue

        symbols: set[str] = set()
        for raw in result.stdout.splitlines():
            line = raw.strip()
            if not line:
                continue
            name = line.split()[0]
            if platform == "macos" and name.startswith("_"):
                name = name[1:]
            symbols.add(name)
        return symbols

    fail(f"unable to inspect dynamic exports for {library}: {last_error}")
    return set()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=pathlib.Path)
    parser.add_argument("--header", required=True, type=pathlib.Path)
    parser.add_argument("--allowlist", required=True, type=pathlib.Path)
    parser.add_argument("--platform", choices=("elf", "macos"), required=True)
    args = parser.parse_args()

    expected = read_allowlist(args.allowlist)
    declared = read_public_declarations(args.header)
    if declared != expected:
        missing = sorted(declared - expected)
        stale = sorted(expected - declared)
        fail(
            "public header/export allowlist drift:"
            f" missing={missing} stale={stale}"
        )

    actual = run_nm(args.library, args.platform)
    missing = sorted(expected - actual)
    leaked = sorted(actual - expected)
    if missing or leaked:
        fail(
            "shared-library export contract mismatch:"
            f" missing={missing} leaked={leaked}"
        )

    print(
        f"Toad shared export contract passed: {len(actual)} public symbols"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
