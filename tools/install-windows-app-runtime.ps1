# SPDX-License-Identifier: GPL-2.0-or-later
#
# Install the unpackaged Windows App SDK runtime that MediaViewer.Chrome
# bootstraps (WindowsPackageType=None). Pin the versioned aka.ms URL — the
# /stable/ alias redirects to Bing HTML, which is what CI then tried to
# execute as windowsappruntimeinstall-x64.exe.
#
# Matches Microsoft.WindowsAppSDK 2.4.0 in src.managed/MediaViewer.Chrome.
# Exit 0 on success, 1 on a failed download or installer.

[CmdletBinding()]
param(
    [string]$Version = '2.4.0',
    [string]$Arch = 'x64',
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'

if (-not $OutDir) {
    if ($env:RUNNER_TEMP) {
        $OutDir = $env:RUNNER_TEMP
    } else {
        $OutDir = [System.IO.Path]::GetTempPath()
    }
}

$parts = $Version.Split('.')
if ($parts.Length -lt 2) {
    throw "Version '$Version' is not major.minor[.patch]."
}
$channel = "$($parts[0]).$($parts[1])"
$uri = "https://aka.ms/windowsappsdk/$channel/$Version/windowsappruntimeinstall-$Arch.exe"
$installer = Join-Path $OutDir "windowsappruntimeinstall-$Arch.exe"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
if (Test-Path $installer) {
    Remove-Item -Force $installer
}

Write-Host "Downloading Windows App SDK $Version runtime ($Arch)"
Write-Host "  $uri"

# curl.exe, not Invoke-WebRequest: IWR's progress bar has corrupted large
# binaries on Windows, and --fail rejects a 404 that IWR would save as HTML.
& curl.exe --fail --location --retry 5 --show-error --output $installer $uri
if ($LASTEXITCODE -ne 0) {
    throw "curl failed with exit $LASTEXITCODE downloading $uri"
}

$stream = [System.IO.File]::OpenRead($installer)
try {
    $mz = New-Object byte[] 2
    if ($stream.Read($mz, 0, 2) -ne 2 -or $mz[0] -ne 0x4D -or $mz[1] -ne 0x5A) {
        throw "Downloaded installer is not a PE executable (aka.ms likely returned HTML). URI: $uri"
    }
} finally {
    $stream.Close()
}

$size = (Get-Item $installer).Length
if ($size -lt 1MB) {
    throw "Installer is only $size bytes; expected ~100 MB. URI: $uri"
}
Write-Host "Installer $size bytes; running --quiet --force"

& $installer --quiet --force
if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
    throw "windowsappruntimeinstall exit $LASTEXITCODE"
}

Write-Host "Windows App SDK $Version runtime installed."
