param(
  [Parameter(Mandatory=$true)]
  [ValidateSet('windows-x64', 'linux-x64', 'linux-arm64', 'macos-arm64', 'android-arm64-v8a')]
  [string]$Rid,
  [string]$Profile,
  [switch]$Local
)

$ErrorActionPreference = 'Stop'
if (-not $Local -and (-not $env:GITHUB_ENV -or -not $env:GITHUB_PATH)) {
  throw 'GITHUB_ENV and GITHUB_PATH are required in CI'
}

$arguments = @((Join-Path $PSScriptRoot 'restore-native-sdks.py'), '--rid', $Rid)
if ($Profile) { $arguments += @('--profile', $Profile) }
$restoredJson = & python @arguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$restored = ($restoredJson -join [Environment]::NewLine) | ConvertFrom-Json -AsHashtable
foreach ($key in $restored.Keys) {
  [Environment]::SetEnvironmentVariable($key, [string]$restored[$key], 'Process')
  if (-not $Local) {
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "$key=$($restored[$key])"
  }
}
$runtimeBins = @(
  (Join-Path $restored['RE2C_ROOT'] 'bin')
  (Join-Path $restored['SALTS_BASE_ROOT'] 'bin')
  (Join-Path $restored['CMETA_ROOT'] 'bin')
  (Join-Path $restored['SALTS_UTILS_ROOT'] 'bin')
  (Join-Path $restored['SALTS_CORE_ROOT'] 'bin')
  (Join-Path $restored['CHTTP_ROOT'] 'bin')
  (Join-Path $restored['VCPKG_INSTALLED_DIR'] "$($restored['VCPKG_TARGET_TRIPLET'])/bin")
)
if ($env:TURBODB_ROOT) { $runtimeBins += Join-Path $env:TURBODB_ROOT 'bin' }
$env:PATH = ($runtimeBins -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:PATH
if (-not $Local) {
  foreach ($runtimeBin in $runtimeBins) {
    Add-Content -LiteralPath $env:GITHUB_PATH -Value $runtimeBin
  }
}
Write-Host "Restored exact published SDKs for $Rid ($env:VCPKG_TARGET_TRIPLET)"
