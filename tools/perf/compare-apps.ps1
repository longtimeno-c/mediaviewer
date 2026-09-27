# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# App launch/teardown for the comparison. Each app is opened on the same file the way a user would.
$Repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$Apps = @{
  mv     = @{ Label = 'MediaViewer';   Proc = 'mediaviewer_lab'; Ctrl = $false }
  photos = @{ Label = 'Windows Photos'; Proc = 'Photos';          Ctrl = $true  }
  wmp    = @{ Label = 'Media Player';   Proc = 'Microsoft.Media.Player'; Ctrl = $false }
}
function Start-App([string]$App, [string]$File) {
  switch ($App) {
    'mv'     { Start-Process "$Repo\build\bin\Release\mediaviewer_lab.exe" -ArgumentList "`"$File`"" | Out-Null }
    'photos' { Start-Process "ms-photos:viewer?fileName=$File" }
    'wmp'    { & "$PSScriptRoot\uwp-open.ps1" -AppId 'Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic' -Path $File | Out-Null }
  }
}
function Stop-App([string]$App) {
  Get-Process -Name $Apps[$App].Proc -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Seconds 2
}
function App-Title([string]$App, [string]$File) { if ($App -eq 'wmp') { 'Media Player' } else { [IO.Path]::GetFileName($File) } }
