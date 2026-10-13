#!/usr/bin/env bash
# Flowie #70: relink published-version provider source against the exact Salts
# candidate. Draft-only provenance gate; no production package fallback.
set -euo pipefail
: "${GITHUB_WORKSPACE:?}"
: "${RUNNER_TEMP:?}"
: "${SALTS_SOURCE_REF:?}"
: "${SALTS_UTILS_SOURCE_REF:?}"
: "${CHTTP_SOURCE_REF:?}"
root="$GITHUB_WORKSPACE/build/dependencies"
sdk="$RUNNER_TEMP/flowie-sdk"
su_src="$root/salts-utils"
chttp_src="$root/chttp"
salts="$sdk/salts/release"
su_sdk="$sdk/salts-utils/release"
chttp_sdk="$sdk/http-services/release"
provider="0e3139d209b20e7bb95b29b4ee8ff9323a531cbe"

test "$(git -C "$root/salts" rev-parse HEAD)" = "$SALTS_SOURCE_REF"
test "$(git -C "$su_src" rev-parse HEAD)" = "$SALTS_UTILS_SOURCE_REF"
test "$(git -C "$chttp_src" rev-parse HEAD)" = "$CHTTP_SOURCE_REF"
test -f "$salts/lib/cmake/Salts/SaltsConfig.cmake"

# The source rebuilt SaltsUtils and CHttp both must resolve the same
# GmSSL candidate as Salts. SaltsUtils already has a separate *zstd* registry:
# preserve that exact existing entry and add/override ONLY the gmssl package.
# Both files live in disposable source checkouts, never the Flowie manifest.
python3 - "$su_src/vcpkg-configuration.json" "$chttp_src/vcpkg-configuration.json" "$provider" <<'PY'
import json
import sys
from pathlib import Path
*paths, sha = sys.argv[1:]
for path_text in paths:
    path = Path(path_text)
    data = json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else {}
    registries = list(data.get("registries", []))
    matches = [r for r in registries if "gmssl" in r.get("packages", [])]
    if len(matches) > 1:
        raise SystemExit(f"Multiple gmssl registries in {path}")
    if matches:
        reg = matches[0]
        reg.update(reference=sha, baseline=sha)
        if reg.get("kind") != "git" or reg.get("repository") != "https://github.com/qigao/vcpkg-cache":
            raise SystemExit(f"Unexpected gmssl Registry: {path}")
    else:
        registries.append({
            "kind": "git", "repository": "https://github.com/qigao/vcpkg-cache",
            "reference": sha, "baseline": sha, "packages": ["gmssl"]})
    data["registries"] = registries
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    reread = json.loads(path.read_text(encoding="utf-8"))
    assert len([r for r in reread["registries"] if "gmssl" in r["packages"]]) == 1
    print(f"Exact provider pinned: {path.name} {sha}")
PY

echo "::group::Mixed released DSO CSTL unresolved symbol audit"
for dir in "$su_sdk/lib" "$chttp_sdk/lib" "$sdk/turbodb/release/lib"; do
  if [ -d "$dir" ]; then
    for file in "$dir"/*.so; do
      [ -f "$file" ] || continue
      symbols="$(readelf -W --dyn-syms "$file" | grep -E 'UND.*(vec_at|vec_at_const|vec_size|vec_push)' || true)"
      if [ -n "$symbols" ]; then
        printf '%s\n%s\n' "$file" "$symbols"
      fi
    done
  fi
done
echo "::endgroup::"

# Strict source replacement: a failure must not silently use the published DSO.
rm -rf "$su_sdk" "$chttp_sdk"
mkdir -p "$su_sdk" "$chttp_sdk"
export SALTS_ROOT="$salts"
export QIGAO_SDK_RID="linux-x64"
export QIGAO_TARGET_TRIPLET="x64-linux"
export PROJECT_ROOT="$GITHUB_WORKSPACE"
unset VCPKG_OVERLAY_PORTS

echo "::group::Source-build SaltsUtils against candidate Salts"
(
  cd "$su_src"
  cmake --preset ci-linux-release-user \
    -DBUILD_TESTS=ON -DBUILD_BENCHMARKS=OFF -DBUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX="$su_sdk" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build --preset ci-linux-release-user --parallel 2
  ctest --preset ci-linux-release-user --no-tests=error --output-on-failure \
    --timeout 90 -R '^test_data_bind_(public_api|dynamic_cstl|format_provider|public_header_boundary)$'
  cmake --install build/ci-linux-release --prefix "$su_sdk"
)
test -f "$su_sdk/lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"
test -f "$su_sdk/lib/libdata_bind.so"
if [ -f "$su_sdk/bin/salts-idlc" ]; then chmod +x "$su_sdk/bin/salts-idlc"; fi
echo "::endgroup::"

echo "::group::Source-build CHttp against matched Salts and SaltsUtils"
export SALTS_UTILS_ROOT="$su_sdk"
export PKG_ROOT="$sdk"
(
  cd "$chttp_src"
  cmake --preset linux-sdk-package -DBUILD_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$chttp_sdk" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  cmake --build --preset linux-sdk-package --parallel 2
  cmake --install build/linux-gcc-sdk-package --prefix "$chttp_sdk"
)
test -f "$chttp_sdk/lib/cmake/Chttp/ChttpConfig.cmake"
echo "::endgroup::"

echo "::group::Verify no private CSTL dependency is left undefined in rebuilt DSOs"
for lib in "$su_sdk/lib/libdata_bind.so" "$chttp_sdk/lib/libchttp_client.so" "$chttp_sdk/lib/libchttp_server.so"; do
  test -f "$lib"
  if readelf -W --dyn-syms "$lib" | grep -E 'UND.*(vec_at|vec_at_const|vec_size|vec_push)'; then
    echo "Unresolved private CSTL from exact-source rebuilt DSO: $lib" >&2
    exit 1
  fi
done
echo "::endgroup::"

{
  echo "### Flowie #70 exact-source provider DSO closure (Draft)"
  echo "- Salts: $SALTS_SOURCE_REF"
  echo "- SaltsUtils: $SALTS_UTILS_SOURCE_REF"
  echo "- CHttp: $CHTTP_SOURCE_REF"
  echo "- GmSSL registry SHA: $provider"
  echo "- Released SDK DSOs were replaced; no fallback or topology change"
} >> "$GITHUB_STEP_SUMMARY"
