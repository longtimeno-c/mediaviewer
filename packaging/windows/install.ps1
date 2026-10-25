# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The driftwire installer for Windows (docs/design/28 "Installing"):
#
#   irm <site>/install.ps1 | iex                                     install driftwire, or update it
#   & ([scriptblock]::Create((irm <site>/install.ps1))) -Uninstall   remove what this added
#
# Per user, no admin: driftwire.exe and the DLLs it loads go in
# %LOCALAPPDATA%\Programs\Driftwire, and that folder goes on the user's PATH
# once. The archive's SHA-256, and for a signed build every program's
# Authenticode signature, is checked before anything is replaced; running it
# again installs the site's version over the old one. It needs no MediaViewer.
# Windows PowerShell 5.1 and PowerShell 7; x64 (Windows 11 on Arm runs it too).
# Only binary cmdlets and .NET: Windows PowerShell 5.1's Get-FileHash and
# Expand-Archive live in script modules, which the default policy on a Windows
# client (Restricted) will not load, though it runs `irm | iex`.
#
# tools/package/driftwire_repo.py writes the site's address and the archive's
# version, name, SHA-256 and signedness in below when it builds the site.
# DRIFTWIRE_REPO_URL, or -BaseUrl, takes the archive from another copy of the
# site (tests, a fork's); its SHA-256 must still be the one written here.
# Everything is in one function and the last line calls it, so a download cut
# off part way runs nothing; nothing but that function is left in the session.
# ASCII only, so it reads the same whatever charset the server names.
#
# DRIFTWIRE_INSTALL_TEST=1 runs it off Windows for the tests
# (tools/package/test_driftwire_repo.py): the user's PATH is then the file
# DRIFTWIRE_TEST_USER_PATH, Authenticode, the registry and the Windows checks
# are skipped, and the program (the tests' stand-in: a shell script under a PE
# header) runs through sh; DRIFTWIRE_TEST_WINDOWS_VERSION pretends to be that
# Windows for the version and Arm checks. Their real proof is CI's Windows leg
# (tools/package/check-driftwire-install.ps1).
param(
    [switch]$Uninstall,
    [string]$BaseUrl = ''
)

function Install-Driftwire {
    param([bool]$Remove, [string]$Base)

    Set-StrictMode -Version 3.0
    $ErrorActionPreference = 'Stop'
    # Windows PowerShell 5.1 downloads crawl while it draws a progress bar.
    $ProgressPreference = 'SilentlyContinue'

    $defaultUrl = '@DRIFTWIRE_REPO_URL@'
    $version = '@DRIFTWIRE_VERSION@'
    $zipName = '@DRIFTWIRE_WINDOWS_ZIP@'
    $zipSha256 = '@DRIFTWIRE_WINDOWS_SHA256@'
    $signed = '@DRIFTWIRE_WINDOWS_SIGNED@' -eq 'yes'
    $brewTap = '@DRIFTWIRE_BREW_TAP@'

    function Say([string]$Text) { Write-Host "driftwire: $Text" }
    function Step([string]$Text) { Write-Host "  $Text" }
    function Fail([string]$Text) { throw "driftwire: $Text" }

    # A PATH entry as Windows compares them: variables expanded, no trailing
    # backslash, any case.
    function Get-PathKey([string]$Entry) {
        [Environment]::ExpandEnvironmentVariables($Entry.Trim()).TrimEnd([char[]]@('\', '/')).ToLowerInvariant()
    }

    # The user's Path as the registry holds it: %VARIABLES% left unexpanded and
    # its kind kept, so writing it back changes only our entry.
    function Get-UserPath {
        if ($test) {
            $file = $env:DRIFTWIRE_TEST_USER_PATH
            if (-not $file) { Fail 'test mode needs DRIFTWIRE_TEST_USER_PATH.' }
            $text = ''
            if (Test-Path -LiteralPath $file) { $text = [IO.File]::ReadAllText($file) }
            return @{ Value = $text; Kind = 'ExpandString' }
        }
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $false)
        try {
            if ($null -eq $key -or $key.GetValueNames() -notcontains 'Path') {
                return @{ Value = ''; Kind = [Microsoft.Win32.RegistryValueKind]::ExpandString }
            }
            $text = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            return @{ Value = [string]$text; Kind = $key.GetValueKind('Path') }
        } finally {
            if ($null -ne $key) { $key.Close() }
        }
    }

    function Set-UserPath([string]$Value, $Kind) {
        if ($test) {
            [IO.File]::WriteAllText($env:DRIFTWIRE_TEST_USER_PATH, $Value)
            return
        }
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Environment')
        try {
            if ($Value) { $key.SetValue('Path', $Value, $Kind) } else { $key.DeleteValue('Path', $false) }
        } finally {
            $key.Close()
        }
        # Terminals opened from now on take the environment from Explorer, which
        # re-reads it on WM_SETTINGCHANGE "Environment". SetEnvironmentVariable
        # broadcasts that, but would also rewrite Path expanded and as REG_SZ;
        # setting and clearing a variable of our own broadcasts it alone.
        [Environment]::SetEnvironmentVariable('DRIFTWIRE_INSTALLER', '1', 'User')
        [Environment]::SetEnvironmentVariable('DRIFTWIRE_INSTALLER', $null, 'User')
    }

    function Add-ToUserPath([string]$Folder) {
        $path = Get-UserPath
        $want = Get-PathKey $Folder
        foreach ($entry in ($path.Value -split ';')) {
            if ($entry.Trim() -and (Get-PathKey $entry) -eq $want) { return $false }
        }
        $value = $path.Value
        if ($value -and -not $value.EndsWith(';')) { $value += ';' }
        Set-UserPath ($value + $Folder) $path.Kind
        return $true
    }

    function Remove-FromUserPath([string]$Folder) {
        $path = Get-UserPath
        $want = Get-PathKey $Folder
        $entries = @($path.Value -split ';')
        $kept = @($entries | Where-Object { -not ($_.Trim() -and (Get-PathKey $_) -eq $want) })
        if ($kept.Count -eq $entries.Count) { return $false }
        Set-UserPath ($kept -join ';') $path.Kind
        return $true
    }

    # This session's PATH too, so `driftwire` works here without a new terminal.
    function Set-SessionPath([string]$Folder, [bool]$Present) {
        $sep = [IO.Path]::PathSeparator
        $want = Get-PathKey $Folder
        $entries = @(([string]$env:PATH).Split($sep) | Where-Object { $_ -and (Get-PathKey $_) -ne $want })
        if ($Present) { $entries += $Folder }
        $env:PATH = $entries -join $sep
    }

    # Folders an earlier run left: an old copy that was still running, a
    # download that was cut off. Only the names it makes, so nobody else's.
    function Remove-Leftovers([string]$Programs) {
        if (-not (Test-Path -LiteralPath $Programs)) { return }
        foreach ($old in Get-ChildItem -LiteralPath $Programs -Directory -Force |
                Where-Object { $_.Name -cmatch '^(Driftwire\.old|\.driftwire)-[0-9a-f]{8}$' }) {
            Write-Host "+ delete $($old.FullName) (an earlier run's)"
            Remove-Item -LiteralPath $old.FullName -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    # Get-FileHash, from .NET (see the top).
    function Get-Sha256([string]$Path) {
        $stream = [IO.File]::OpenRead($Path)
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant()
        } finally {
            $sha.Dispose()
            $stream.Dispose()
        }
    }

    # ---- where from, where to -------------------------------------------------

    $onWindows = $true
    $isWindowsVariable = Get-Variable -Name IsWindows -ValueOnly -ErrorAction SilentlyContinue
    if ($null -ne $isWindowsVariable) { $onWindows = [bool]$isWindowsVariable }   # PowerShell 6 and later say
    $test = (-not $onWindows) -and $env:DRIFTWIRE_INSTALL_TEST -eq '1'

    $url = $defaultUrl
    if ($Base) { $url = $Base } elseif ($env:DRIFTWIRE_REPO_URL) { $url = $env:DRIFTWIRE_REPO_URL }
    $url = $url.TrimEnd('/')
    if ($url.StartsWith('@') -or $zipSha256 -notmatch '^[0-9a-f]{64}$') {
        Fail "this is the installer's template, with no archive in it: use the one on the site."
    }
    if ($url -notmatch '^https://[^/]' -and $url -notmatch '^http://(127\.0\.0\.1|localhost)(:[0-9]+)?(/|$)') {
        Fail "the site's address must start with https:// ($url)."
    }
    if ($url -notmatch '^[A-Za-z0-9._~:/%+-]+$') { Fail "the site's address has a character it may not have ($url)." }

    if (-not $onWindows -and -not $test) {
        Fail "this installer is for Windows. On macOS: brew install $brewTap/driftwire (without Homebrew, the Linux line). On Linux: curl -fsSL $url/install.sh | sh"
    }
    $local = $env:LOCALAPPDATA
    if (-not $local) { Fail 'LOCALAPPDATA is not set.' }
    $programs = Join-Path $local 'Programs'
    $dir = Join-Path $programs 'Driftwire'
    # Only a folder this put there is replaced or removed: one with driftwire.exe in it.
    if ((Test-Path -LiteralPath $dir) -and -not (Test-Path -LiteralPath (Join-Path $dir 'driftwire.exe'))) {
        Fail "$dir is there and holds no driftwire.exe; it is not this installer's, so it is left alone."
    }

    # ---- removing ---------------------------------------------------------------

    if ($Remove) {
        $removed = $false
        if (Test-Path -LiteralPath $dir) {
            Write-Host "+ delete $dir"
            try {
                Remove-Item -LiteralPath $dir -Recurse -Force
            } catch {
                Fail "could not delete $dir (is driftwire running? close it and run this again): $($_.Exception.Message)"
            }
            $removed = $true
        }
        Remove-Leftovers $programs
        if (Remove-FromUserPath $dir) {
            Write-Host "+ take $dir off your user PATH"
            $removed = $true
        }
        Set-SessionPath $dir $false
        if ($removed) {
            Say "removed. Its data folder ($(Join-Path $local 'Driftwire')) is left as it was."
        } else {
            Say "nothing of driftwire's to remove here."
        }
        return
    }

    # ---- installing -------------------------------------------------------------

    $windows = $null
    if ($onWindows) {
        $windows = [Environment]::OSVersion.Version
    } elseif ($test -and $env:DRIFTWIRE_TEST_WINDOWS_VERSION) {
        $windows = [version]$env:DRIFTWIRE_TEST_WINDOWS_VERSION
    }
    if ($null -ne $windows) {
        if (-not [Environment]::Is64BitOperatingSystem) {
            Fail 'driftwire is built for 64-bit Windows (x64, which Windows 11 on Arm runs too); this Windows is 32-bit.'
        }
        if ($windows.Major -lt 10) { Fail 'driftwire needs Windows 10 or later.' }
        # Windows on Arm runs x64 programs from Windows 11 (build 22000) on;
        # Windows 10 on Arm runs 32-bit x86 ones only. PowerShell itself may be
        # Arm or x86 there: either says ARM64 in one of these.
        $onArm = @($env:PROCESSOR_ARCHITECTURE, $env:PROCESSOR_ARCHITEW6432) -contains 'ARM64'
        if ($onArm -and $windows.Build -lt 22000) {
            Fail 'driftwire is an x64 program, which Windows on Arm runs from Windows 11 on; this is Windows 10 on Arm, so nothing was installed.'
        }
    }
    if ($PSVersionTable.PSVersion.Major -lt 6) {
        # Windows PowerShell 5.1 may still offer TLS 1.0 only; the site needs 1.2.
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    }

    Say "driftwire $version for Windows, per user, from $url. About to:"
    $checks = 'its SHA-256'
    if ($signed) { $checks += " and each program's signature" }
    Step "1. download $zipName and check $checks"
    Step "2. put it in $dir (replacing what is there)"
    Step "3. add that folder to your user PATH, if it is not there"

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ('driftwire-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp | Out-Null
    # Unpacked beside the install, so putting it in place is a rename on one volume.
    $staging = Join-Path $programs ('.driftwire-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    try {
        $zip = Join-Path $tmp $zipName
        Write-Host "+ download $url/$zipName"
        Invoke-WebRequest -Uri "$url/$zipName" -OutFile $zip -UseBasicParsing
        $got = Get-Sha256 $zip
        if ($got -ne $zipSha256) {
            Fail "$zipName does not match its SHA-256 (it is $got, not $zipSha256); nothing was changed."
        }
        Remove-Leftovers $programs
        New-Item -ItemType Directory -Force -Path $programs | Out-Null
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, $staging)
        $top = Join-Path $staging ([IO.Path]::GetFileNameWithoutExtension($zipName))
        $exe = Join-Path $top 'driftwire.exe'
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Fail "$zipName has no driftwire.exe; nothing was changed." }
        if ($signed -and $test) {
            Say 'test mode: Authenticode is checked on Windows only.'
        } elseif ($signed) {
            foreach ($file in Get-ChildItem -LiteralPath $top -File | Where-Object { $_.Extension -in '.exe', '.dll' }) {
                $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
                if ($signature.Status -ne 'Valid') {
                    Fail "$($file.Name) is not validly signed ($($signature.Status)); nothing was changed."
                }
            }
            $signer = (Get-AuthenticodeSignature -LiteralPath $exe).SignerCertificate.Subject
            Say "signatures valid; driftwire.exe is signed by $signer"
        }
        if ($test) { & chmod 755 $exe }

        # Put it in place: the old copy aside, the new one in, the old one gone.
        $old = $null
        if (Test-Path -LiteralPath $dir) {
            $old = Join-Path $programs ('Driftwire.old-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
            try {
                Move-Item -LiteralPath $dir -Destination $old
            } catch {
                Fail "could not move the installed copy aside (is driftwire running? close it and run this again): $($_.Exception.Message)"
            }
        }
        Write-Host "+ put driftwire $version in $dir"
        try {
            Move-Item -LiteralPath $top -Destination $dir
        } catch {
            if ($old) { Move-Item -LiteralPath $old -Destination $dir }
            Fail "could not put the new copy in $dir; the old one is back: $($_.Exception.Message)"
        }
        if ($old) {
            Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $old) { Say "the old copy is still running from $old; the next run takes it away." }
        }
    } finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    }

    if (Add-ToUserPath $dir) { Write-Host "+ add $dir to your user PATH" }
    Set-SessionPath $dir $true
    if ($test) {
        $said = & sh (Join-Path $dir 'driftwire.exe') --version
    } else {
        $said = & (Join-Path $dir 'driftwire.exe') --version
    }
    if ($LASTEXITCODE -ne 0) { Fail "driftwire.exe did not run (exit $LASTEXITCODE)." }
    Write-Host $said
    Say 'installed. Start it with: driftwire (here, or in any terminal opened from now on)'
}

Install-Driftwire -Remove:$Uninstall -Base $BaseUrl
