# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Windows installer end to end (docs/design/28 "Installing"), as a person
# would run it. CI's Windows leg runs it on the zip it has just packed; off
# Windows, tools/package/test_driftwire_repo.py runs it in install.ps1's test
# mode (DRIFTWIRE_INSTALL_TEST=1: a scratch LOCALAPPDATA, the user's PATH in a
# file, no Authenticode).
#
#   check-driftwire-install.ps1 -Zip driftwire-<V>-windows-x64.zip [-NewerZip <a later version>]
#                               [-SignedZip <a signed build>]
#
# It builds a site of install.ps1 and the zip (driftwire_repo.py windows-site),
# serves it on 127.0.0.1 (python -m http.server), and checks that:
#   - a copy of install.ps1 cut off part way does nothing;
#   - `irm <site>/install.ps1 | iex` installs (Windows PowerShell 5.1 and
#     PowerShell 7 on Windows), and a new process with only the user's and the
#     machine's PATH runs `driftwire --version`;
#   - running it again replaces the copy and leaves one PATH entry; with
#     -NewerZip, the newer site's installer updates it to that version;
#   - a zip whose SHA-256 is not the one baked in is refused, and the installed
#     copy stays as it was;
#   - `& ([scriptblock]::Create((irm <site>/install.ps1))) -Uninstall` takes the
#     folder and the PATH entry away, and keeps the user's other entries (with
#     their %VARIABLES%), the Path value's kind and the data folder;
#   - with -SignedZip, a signed build installs with its signatures checked.
#
# On Windows it installs into, and removes from, this user's own profile and
# PATH: run it on a CI runner or a throwaway account. It refuses to start when
# driftwire is already installed there.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [string]$NewerZip,
    [string]$SignedZip,
    [string]$Python
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$builder = Join-Path $repo 'tools/package/driftwire_repo.py'

$onWindows = $true
$isWindowsVariable = Get-Variable -Name IsWindows -ValueOnly -ErrorAction SilentlyContinue
if ($null -ne $isWindowsVariable) { $onWindows = [bool]$isWindowsVariable }
if (-not $Python) { $Python = if ($onWindows) { 'python' } else { 'python3' } }
# Found now: the checks below run it with a PATH of the user's alone.
$pwshPath = (Get-Command pwsh).Source

function Check([bool]$Condition, [string]$What) {
    if (-not $Condition) { throw "check: $What" }
    Write-Host "check: ${What}: yes"
}

function Get-ZipVersion([string]$Path) {
    $m = [regex]::Match((Split-Path -Leaf $Path), '^driftwire-([0-9]+\.[0-9]+\.[0-9]+)-windows-x64\.zip$')
    if (-not $m.Success) { throw "check: $Path is not a driftwire Windows zip" }
    $m.Groups[1].Value
}
$Zip = (Resolve-Path -LiteralPath $Zip).Path
$version = Get-ZipVersion $Zip

$work = Join-Path ([IO.Path]::GetTempPath()) ('dwcheck-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$saved = @{ PATH = $env:PATH; LOCALAPPDATA = $env:LOCALAPPDATA; DRIFTWIRE_INSTALL_TEST = $env:DRIFTWIRE_INSTALL_TEST
            DRIFTWIRE_TEST_USER_PATH = $env:DRIFTWIRE_TEST_USER_PATH; DRIFTWIRE_REPO_URL = $env:DRIFTWIRE_REPO_URL }
$env:DRIFTWIRE_REPO_URL = $null

# ---- the user's PATH, as install.ps1 sees it ----------------------------------

if ($onWindows) {
    function Get-RawUserPath {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $false)
        try {
            if ($key.GetValueNames() -notcontains 'Path') { return @{ Value = $null; Kind = $null } }
            @{ Value = [string]$key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
               Kind = $key.GetValueKind('Path') }
        } finally { $key.Close() }
    }
    function Set-RawUserPath($Saved) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment')
        try {
            if ($null -eq $Saved.Value) { $key.DeleteValue('Path', $false) } else { $key.SetValue('Path', $Saved.Value, $Saved.Kind) }
        } finally { $key.Close() }
    }
    # A new process the way a new terminal starts: the machine's PATH and the user's.
    function Invoke-NewTerminal([string]$Command) {
        $before = $env:PATH
        try {
            $env:PATH = [Environment]::GetEnvironmentVariable('Path', 'User') + ';' + [Environment]::GetEnvironmentVariable('Path', 'Machine')
            (& $pwshPath -NoProfile -NonInteractive -Command $Command | Out-String).Trim()
        } finally { $env:PATH = $before }
    }
    $programName = 'driftwire'
} else {
    $env:DRIFTWIRE_INSTALL_TEST = '1'
    $env:LOCALAPPDATA = Join-Path $work 'LocalAppData'
    $env:DRIFTWIRE_TEST_USER_PATH = Join-Path $work 'user-path.txt'
    New-Item -ItemType Directory -Path $env:LOCALAPPDATA | Out-Null
    function Get-RawUserPath {
        if (-not (Test-Path -LiteralPath $env:DRIFTWIRE_TEST_USER_PATH)) { return @{ Value = $null; Kind = 'ExpandString' } }
        @{ Value = [IO.File]::ReadAllText($env:DRIFTWIRE_TEST_USER_PATH); Kind = 'ExpandString' }
    }
    function Set-RawUserPath($Saved) {
        if ($null -eq $Saved.Value) { Remove-Item -LiteralPath $env:DRIFTWIRE_TEST_USER_PATH -ErrorAction SilentlyContinue }
        else { [IO.File]::WriteAllText($env:DRIFTWIRE_TEST_USER_PATH, $Saved.Value) }
    }
    function Invoke-NewTerminal([string]$Command) {
        $before = $env:PATH
        try {
            $user = ((Get-RawUserPath).Value -split ';' | Where-Object { $_ }) -join [IO.Path]::PathSeparator
            $env:PATH = $user + [IO.Path]::PathSeparator + $before
            # The stand-in is a shell script under a PE header: sh runs it when exec cannot.
            (& sh -c $Command | Out-String).Trim()
        } finally { $env:PATH = $before }
    }
    # Off Windows the program is the test's stand-in named driftwire.exe; PATHEXT does not apply.
    $programName = 'driftwire.exe'
}

$installDir = Join-Path (Join-Path $env:LOCALAPPDATA 'Programs') 'Driftwire'
$dataDir = Join-Path $env:LOCALAPPDATA 'Driftwire'
if (Test-Path -LiteralPath $installDir) { throw "check: driftwire is already installed in $installDir; not touching it" }
$originalPath = Get-RawUserPath
$madeData = $false
$server = $null

function Get-PathEntries { @(((Get-RawUserPath).Value + '') -split ';' | Where-Object { $_ }) }
function Get-InstallCount {
    $trim = [char[]]@('\', '/')
    $want = $installDir.TrimEnd($trim).ToLowerInvariant()
    @(Get-PathEntries | Where-Object { [Environment]::ExpandEnvironmentVariables($_).TrimEnd($trim).ToLowerInvariant() -eq $want }).Count
}

# Runs a command in a new PowerShell (each one this machine has), as `irm | iex` runs.
function Invoke-Installer([string]$Command, [string]$Shell = $pwshPath) {
    Write-Host "check: $(Split-Path -Leaf $Shell) -Command `"$Command`""
    $output = & $Shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $Command 2>&1 | Out-String
    Write-Host $output.TrimEnd()
    @{ Code = $LASTEXITCODE; Output = $output }
}

try {
    # The user's own entries: one with a %VARIABLE%, kept unexpanded through it all.
    $sentinel = '%USERPROFILE%\driftwire-check-kept'
    $startValue = if ($originalPath.Value) { $originalPath.Value.TrimEnd(';') + ';' + $sentinel } else { $sentinel }
    Set-RawUserPath @{ Value = $startValue; Kind = $(if ($originalPath.Kind) { $originalPath.Kind } else { [Microsoft.Win32.RegistryValueKind]::ExpandString }) }
    $startKind = (Get-RawUserPath).Kind
    if (-not (Test-Path -LiteralPath $dataDir)) {
        New-Item -ItemType Directory -Path $dataDir | Out-Null
        Set-Content -LiteralPath (Join-Path $dataDir 'driftwire.json') -Value '{}'
        $madeData = $true
    }

    # ---- the site ----
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    $root = Join-Path $work 'www'
    $url = "http://127.0.0.1:$port/unsigned/driftwire"
    & $Python $builder windows-site --zip $Zip --out (Join-Path $root 'unsigned') --base-url $url
    if ($LASTEXITCODE) { throw 'check: driftwire_repo.py windows-site failed' }
    if ($NewerZip) {
        $newerVersion = Get-ZipVersion $NewerZip
        $newerUrl = "http://127.0.0.1:$port/newer/driftwire"
        & $Python $builder windows-site --zip (Resolve-Path -LiteralPath $NewerZip).Path --out (Join-Path $root 'newer') `
            --base-url $newerUrl
        if ($LASTEXITCODE) { throw 'check: driftwire_repo.py windows-site (newer) failed' }
    }
    if ($SignedZip) {
        $signedUrl = "http://127.0.0.1:$port/signed/driftwire"
        & $Python $builder windows-site --zip (Resolve-Path -LiteralPath $SignedZip).Path --out (Join-Path $root 'signed') `
            --base-url $signedUrl --require-windows-signature
        if ($LASTEXITCODE) { throw 'check: driftwire_repo.py windows-site --require-windows-signature failed' }
    }
    $server = Start-Process -FilePath $Python -ArgumentList @('-m', 'http.server', "$port", '--bind', '127.0.0.1', '--directory', $root) `
        -PassThru -NoNewWindow -RedirectStandardError (Join-Path $work 'server.log') -RedirectStandardOutput (Join-Path $work 'server.out')
    for ($i = 0; ; $i++) {
        try { Invoke-WebRequest -Uri "$url/install.ps1" -UseBasicParsing | Out-Null; break }
        catch { if ($i -ge 50) { throw 'check: the local site did not come up' }; Start-Sleep -Milliseconds 200 }
    }
    $served = Join-Path $root 'unsigned/driftwire'
    $script = [IO.File]::ReadAllText((Join-Path $served 'install.ps1'))

    # ---- cut off part way: nothing happens ----
    $callAt = $script.LastIndexOf('Install-Driftwire -Remove')
    foreach ($cut in @($script.Substring(0, [int]($script.Length / 2)), $script.Substring(0, $callAt))) {
        [IO.File]::WriteAllText((Join-Path $served 'install-cut.ps1'), $cut)
        Invoke-Installer "irm $url/install-cut.ps1 | iex" | Out-Null
        Check (-not (Test-Path -LiteralPath $installDir) -and (Get-InstallCount) -eq 0) `
            "install.ps1 cut off at $($cut.Length) of $($script.Length) characters does nothing"
    }

    # ---- install, in each PowerShell, and again ----
    $shells = if ($onWindows) { @((Get-Command powershell).Source, $pwshPath) } else { @($pwshPath, $pwshPath) }
    foreach ($shell in $shells) {
        $run = Invoke-Installer "irm $url/install.ps1 | iex" $shell
        Check ($run.Code -eq 0 -and $run.Output -match "driftwire $([regex]::Escape($version))") "$(Split-Path -Leaf $shell) installs driftwire $version"
        Check (Test-Path -LiteralPath (Join-Path $installDir 'driftwire.exe')) "driftwire.exe is in $installDir"
        Check ((Get-InstallCount) -eq 1) 'its folder is on the user PATH once'
        $said = Invoke-NewTerminal "$programName --version"
        Check ($said -eq "driftwire $version") "a new terminal runs '$programName --version' and gets '$said'"
    }
    $left = @(Get-ChildItem -LiteralPath (Split-Path $installDir) -Force |
        Where-Object { $_.Name -like 'Driftwire.old-*' -or $_.Name -like '.driftwire-*' })
    Check ($left.Count -eq 0) 'no old copy or unpacked download left beside the install'

    # ---- an update to a newer version, then back to this one ----
    if ($NewerZip) {
        $run = Invoke-Installer "irm $newerUrl/install.ps1 | iex"
        $said = Invoke-NewTerminal "$programName --version"
        Check ($run.Code -eq 0 -and $said -eq "driftwire $newerVersion" -and (Get-InstallCount) -eq 1) `
            "the newer site's installer updates it to $newerVersion, still once on the PATH"
        $run = Invoke-Installer "irm $url/install.ps1 | iex"
        Check ($run.Code -eq 0 -and (Invoke-NewTerminal "$programName --version") -eq "driftwire $version") "and this one puts $version back"
    }

    # ---- a zip that is not the one baked in ----
    $zipOnSite = Join-Path $served (Split-Path -Leaf $Zip)
    $good = [IO.File]::ReadAllBytes($zipOnSite)
    $before = (Get-FileHash -LiteralPath (Join-Path $installDir 'driftwire.exe')).Hash
    [IO.File]::WriteAllBytes($zipOnSite, $good + [byte[]]@(0))
    $run = Invoke-Installer "irm $url/install.ps1 | iex"
    [IO.File]::WriteAllBytes($zipOnSite, $good)
    Check ($run.Code -ne 0 -and $run.Output -match 'does not match its SHA-256') 'a changed zip is refused'
    Check ((Get-FileHash -LiteralPath (Join-Path $installDir 'driftwire.exe')).Hash -eq $before) 'and the installed copy is as it was'

    # ---- uninstall ----
    $run = Invoke-Installer "& ([scriptblock]::Create((irm $url/install.ps1))) -Uninstall"
    Check ($run.Code -eq 0 -and -not (Test-Path -LiteralPath $installDir)) "-Uninstall deletes $installDir"
    Check ((Get-InstallCount) -eq 0) 'and takes it off the user PATH'
    $after = Get-RawUserPath
    Check ($after.Value -ceq $startValue) "and keeps the user's other entries as they were ($sentinel unexpanded)"
    Check ("$($after.Kind)" -eq "$startKind") "and the Path value's kind ($startKind)"
    Check (Test-Path -LiteralPath (Join-Path $dataDir 'driftwire.json')) 'and leaves the data folder'
    $run = Invoke-Installer "& ([scriptblock]::Create((irm $url/install.ps1))) -Uninstall"
    Check ($run.Code -eq 0 -and $run.Output -match 'nothing of driftwire') 'a second -Uninstall finds nothing to remove'

    # ---- a signed build ----
    if ($SignedZip) {
        $run = Invoke-Installer "irm $signedUrl/install.ps1 | iex"
        $checked = if ($onWindows) { 'signatures valid' } else { 'test mode: Authenticode' }
        Check ($run.Code -eq 0 -and $run.Output -match [regex]::Escape($checked)) "the signed build installs ($checked)"
        $run = Invoke-Installer "& ([scriptblock]::Create((irm $signedUrl/install.ps1))) -Uninstall"
        Check ($run.Code -eq 0 -and -not (Test-Path -LiteralPath $installDir)) 'and uninstalls'
    }
    Write-Host "check: the Windows installer of driftwire $version installs, updates, refuses a changed zip and uninstalls"
} finally {
    if ($null -ne $server) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $installDir) { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
    Set-RawUserPath $originalPath
    if ($madeData) { Remove-Item -LiteralPath $dataDir -Recurse -Force -ErrorAction SilentlyContinue }
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
