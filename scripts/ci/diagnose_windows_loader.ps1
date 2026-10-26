# Diagnostic-only exact SDK Windows Loader probe for Flowie #74.
# This never substitutes an alternate DLL, edits PATH, registers a Plugin,
# treats failure as successful qualification, or skips full CTest.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Module
)
$ErrorActionPreference = "Stop"

if (-not [System.IO.Path]::IsPathRooted($Module)) {
    throw "Loader probe requires an absolute SDK module path"
}
if (-not (Test-Path -LiteralPath $Module -PathType Leaf)) {
    throw "Loader probe module missing from exact SDK: $Module"
}

$interop = @'
using System;
using System.Runtime.InteropServices;
public static class FlowieDllProbeNative {
    [DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW",
       CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(
        string path, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll", EntryPoint = "FreeLibrary",
       SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
Add-Type -TypeDefinition $interop -Language CSharp -ErrorAction Stop
$full = [System.IO.Path]::GetFullPath($Module)
$handle = [FlowieDllProbeNative]::LoadLibraryExW(
    $full, [IntPtr]::Zero, [uint32]0)
if ($handle -eq [IntPtr]::Zero) {
    $code = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $reason = [System.ComponentModel.Win32Exception]::new($code).Message
    Write-Output (("FLOWIE_WIN32_LOADER_PROBE status=FAILED code={0} " +
        "module={1} message={2}") -f $code, $full, $reason)
} else {
    Write-Output (("FLOWIE_WIN32_LOADER_PROBE status=LOADABLE " +
        "module={0}") -f $full)
    if (-not [FlowieDllProbeNative]::FreeLibrary($handle)) {
        throw "Loader probe failed to release module: $full"
    }
}
# A FAIL status is evidence for the existing full CTest failure, not a
# substitute for that required CTest result or a reason to skip its checks.
exit 0
