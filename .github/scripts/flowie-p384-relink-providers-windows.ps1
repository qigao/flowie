$ErrorActionPreference = 'Stop'

function Invoke-Checked {
  param(
    [string]$Executable,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
  )
  & $Executable @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed ($LASTEXITCODE): $Executable $($Arguments -join ' ')"
  }
}

$required = @('GITHUB_WORKSPACE', 'RUNNER_TEMP', 'SALTS_SOURCE_REF',
              'SALTS_UTILS_SOURCE_REF', 'CHTTP_SOURCE_REF', 'TURBODB_SOURCE_REF',
              'VCPKG_CACHE_REPOSITORY_ROOT')
foreach ($name in $required) {
  if (-not [Environment]::GetEnvironmentVariable($name)) {
    throw "Required environment variable is missing: $name"
  }
}

$root = Join-Path $env:GITHUB_WORKSPACE 'build/dependencies'
$sdk = Join-Path $env:RUNNER_TEMP 'flowie-sdk'
$saltsUtilsSource = Join-Path $root 'salts-utils'
$chttpSource = Join-Path $root 'chttp'
$turbodbSource = Join-Path $root 'turbodb'
$saltsSdk = Join-Path $sdk 'salts/release'
$saltsUtilsSdk = Join-Path $sdk 'salts-utils/release'
$chttpSdk = Join-Path $sdk 'http-services/release'
$turbodbSdk = Join-Path $sdk 'turbodb/release'
$provider = '2e214ae44a3e3244eded5db6361194576356fc9e'

$sdkFull = [IO.Path]::GetFullPath($sdk)
$tempFull = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd([IO.Path]::DirectorySeparatorChar) +
            [IO.Path]::DirectorySeparatorChar
if (-not $sdkFull.StartsWith($tempFull, [StringComparison]::OrdinalIgnoreCase)) {
  throw "Refusing to replace SDKs outside RUNNER_TEMP: $sdkFull"
}

$refs = @(
  @((Join-Path $root 'salts'), $env:SALTS_SOURCE_REF),
  @($saltsUtilsSource, $env:SALTS_UTILS_SOURCE_REF),
  @($chttpSource, $env:CHTTP_SOURCE_REF),
  @($turbodbSource, $env:TURBODB_SOURCE_REF)
)
foreach ($entry in $refs) {
  $actual = (& git -C $entry[0] rev-parse HEAD).Trim()
  if ($LASTEXITCODE -ne 0 -or $actual -ne $entry[1]) {
    throw "Source identity mismatch for $($entry[0]): $actual != $($entry[1])"
  }
}
if (-not (Test-Path -LiteralPath (Join-Path $saltsSdk 'lib/cmake/Salts/SaltsConfig.cmake'))) {
  throw 'Candidate Salts SDK is missing before dependent graph rebuild'
}

foreach ($source in @($saltsUtilsSource, $chttpSource, $turbodbSource)) {
  $path = Join-Path $source 'vcpkg-configuration.json'
  if (Test-Path -LiteralPath $path) {
    $config = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
  } else {
    # CHttp intentionally has no repository configuration. Match the Linux
    # exact-source lane by creating one only in this disposable checkout.
    $config = [PSCustomObject]@{}
  }
  $registries = @($config.registries)
  $matches = @($registries | Where-Object { $_.packages -contains 'gmssl' })
  if ($matches.Count -gt 1) {
    throw "Expected at most one GmSSL Registry in $path; got $($matches.Count)"
  }
  if ($matches.Count -eq 1) {
    if ($matches[0].kind -ne 'git' -or
        $matches[0].repository -ne 'https://github.com/qigao/vcpkg-cache') {
      throw "Unexpected GmSSL Registry in $path"
    }
    # Older manifests carry only a baseline. ConvertFrom-Json produces a
    # fixed-shape PSCustomObject, so assigning an absent candidate reference
    # throws instead of extending the object.
    if ($matches[0].PSObject.Properties.Name -contains 'reference') {
      $matches[0].reference = $provider
    } else {
      $matches[0] | Add-Member -NotePropertyName reference -NotePropertyValue $provider
    }
    if ($matches[0].PSObject.Properties.Name -contains 'baseline') {
      $matches[0].baseline = $provider
    } else {
      $matches[0] | Add-Member -NotePropertyName baseline -NotePropertyValue $provider
    }
  } else {
    $registries += [PSCustomObject]@{
      kind = 'git'
      repository = 'https://github.com/qigao/vcpkg-cache'
      reference = $provider
      baseline = $provider
      packages = @('gmssl')
    }
  }
  if ($config.PSObject.Properties.Name -contains 'registries') {
    $config.registries = $registries
  } else {
    $config | Add-Member -NotePropertyName registries -NotePropertyValue $registries
  }
  $config | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $path -Encoding utf8
  $check = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
  $pinned = @($check.registries | Where-Object { $_.packages -contains 'gmssl' })
  if ($pinned.Count -ne 1 -or $pinned[0].reference -ne $provider -or
      $pinned[0].baseline -ne $provider) {
    throw "Candidate GmSSL Registry pin verification failed in $path"
  }
}

foreach ($target in @($saltsUtilsSdk, $chttpSdk, $turbodbSdk)) {
  if (Test-Path -LiteralPath $target) {
    Remove-Item -LiteralPath $target -Recurse -Force
  }
  New-Item -ItemType Directory -Path $target -Force | Out-Null
}

$env:SALTS_ROOT = $saltsSdk
$env:QIGAO_SDK_RID = 'windows-x64'
$env:QIGAO_TARGET_TRIPLET = 'x64-windows'
$env:QIGAO_VCPKG_WINDOWS_TRIPLET = 'x64-windows'
$env:QIGAO_VCPKG_WINDOWS_HOST_TRIPLET = 'x64-windows'
$env:PROJECT_ROOT = $env:GITHUB_WORKSPACE

$quickjsSource = Join-Path $env:VCPKG_CACHE_REPOSITORY_ROOT 'ports/quickjs-ng'
$quickjsOverlay = Join-Path $env:RUNNER_TEMP 'flowie-quickjs-ng-only'
if (-not (Test-Path -LiteralPath $quickjsSource -PathType Container)) {
  throw 'Pinned quickjs-ng overlay is missing'
}
if (Test-Path -LiteralPath $quickjsOverlay) {
  Remove-Item -LiteralPath $quickjsOverlay -Recurse -Force
}
New-Item -ItemType Directory -Path $quickjsOverlay -Force | Out-Null
Copy-Item -LiteralPath $quickjsSource -Destination (Join-Path $quickjsOverlay 'quickjs-ng') -Recurse
$quickjsManifest = Get-Content -LiteralPath (Join-Path $quickjsOverlay 'quickjs-ng/vcpkg.json') -Raw |
                   ConvertFrom-Json
if ($quickjsManifest.name -ne 'quickjs-ng' -or $quickjsManifest.version -ne '0.16.2') {
  throw 'Unexpected QuickJS overlay identity'
}
$env:VCPKG_OVERLAY_PORTS = $quickjsOverlay

Push-Location $saltsUtilsSource
try {
  Invoke-Checked cmake --preset win-sdk-package-user -DBUILD_TESTS=OFF `
    -DBUILD_BENCHMARKS=OFF -DBUILD_EXAMPLES=OFF `
    "-DCMAKE_INSTALL_PREFIX=$saltsUtilsSdk"
  Invoke-Checked cmake --build --preset win-sdk-package-user --parallel 2
  Invoke-Checked cmake --install build/Msvc-SDK-Release --prefix $saltsUtilsSdk
} finally { Pop-Location }
if (-not (Test-Path -LiteralPath (Join-Path $saltsUtilsSdk 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake'))) {
  throw 'Source-rebuilt SaltsUtils SDK is missing'
}

$env:SALTS_UTILS_ROOT = $saltsUtilsSdk
$env:PKG_ROOT = $sdk
Push-Location $chttpSource
try {
  Invoke-Checked cmake --preset win-release-user -DBUILD_TESTS=OFF `
    "-DCMAKE_INSTALL_PREFIX=$chttpSdk"
  Invoke-Checked cmake --build --preset win-release-user --parallel 2
  Invoke-Checked cmake --install build/Msvc-Release --prefix $chttpSdk
} finally { Pop-Location }
if (-not (Test-Path -LiteralPath (Join-Path $chttpSdk 'lib/cmake/Chttp/ChttpConfig.cmake'))) {
  throw 'Source-rebuilt CHttp SDK is missing'
}

$env:TURBODB_CI_PKG_ROOT = $sdk
Push-Location $turbodbSource
try {
  Invoke-Checked cmake --preset ci-windows-release -DBUILD_TESTS=OFF `
    -DBUILD_E2E_TESTS=OFF -DBUILD_EXAMPLES=OFF -DTURBODB_BUILD_DBTOOLS=OFF `
    "-DCMAKE_INSTALL_PREFIX=$turbodbSdk"
  Invoke-Checked cmake --build --preset ci-windows-release --parallel 2
  Invoke-Checked cmake --install build/ci-windows-release --prefix $turbodbSdk
} finally { Pop-Location }
if (-not (Test-Path -LiteralPath (Join-Path $turbodbSdk 'lib/cmake/TurboDB/TurboDBConfig.cmake'))) {
  throw 'Source-rebuilt TurboDB SDK is missing'
}

@"
### Flowie #70 Windows exact-source provider closure (Draft)
- Salts: $env:SALTS_SOURCE_REF
- SaltsUtils: $env:SALTS_UTILS_SOURCE_REF
- CHttp: $env:CHTTP_SOURCE_REF
- TurboDB: $env:TURBODB_SOURCE_REF
- GmSSL registry SHA: $provider
- Released dependent SDKs were replaced; no provider fallback
"@ | Add-Content -LiteralPath $env:GITHUB_STEP_SUMMARY
