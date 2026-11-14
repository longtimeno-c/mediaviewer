// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// drives::port on Windows: Storage Spaces through PowerShell's storage module
// (docs/plans/self-host-drives.md §2). The listing is one script whose JSON
// drives.cpp's parse_windows reads. A combine or a replace is a script written
// to %TEMP% and run once elevated (ShellExecuteEx "runas": the UAC prompt);
// it checks each disk's UniqueId and system flags again before Clear-Disk.
// The test build's own VHDs are changed without the prompt (CI runs elevated).
#include "addons/driftwire/cli/drives.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <fstream>
#include <random>

namespace mv::driftwire::cli::drives {

namespace {

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string powershell() {
  wchar_t sys[MAX_PATH] = {};
  const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "powershell.exe";
  return narrow(std::wstring(sys, n)) + "\\WindowsPowerShell\\v1.0\\powershell.exe";
}

std::string temp_file(const char* ext) {
  wchar_t tmp[MAX_PATH + 1] = {};
  const DWORD n = ::GetTempPathW(MAX_PATH, tmp);
  std::random_device rd;
  char name[64];
  std::snprintf(name, sizeof name, "mv-drives-%08x%08x%s", rd(), rd(), ext);
  return narrow(std::wstring(tmp, n)) + name;
}

bool write_file(const std::string& path, const std::string& text) {
  std::ofstream f(wide(path), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write("\xEF\xBB\xBF", 3);  // UTF-8 with its mark: Windows PowerShell 5 reads it as UTF-8
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(f);
}

std::string read_file(const std::string& path) {
  std::ifstream f(wide(path), std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (s.rfind("\xEF\xBB\xBF", 0) == 0) s.erase(0, 3);
  return s;
}

void remove_file(const std::string& path) { ::DeleteFileW(wide(path).c_str()); }

constexpr const char* kSetsScript = R"PS(
$setsOf = {
@(Get-VirtualDisk | ForEach-Object {
  $vd = $_
  $members = @($vd | Get-PhysicalDisk | ForEach-Object {
    $n = -1; if ($_.DeviceId -match '^\d+$') { $n = [int]$_.DeviceId }
    [ordered]@{ Number = $n; UniqueId = [string]$_.UniqueId; Health = [string]$_.HealthStatus; Operational = [string](($_.OperationalStatus) -join ','); Usage = [string]$_.Usage }
  })
  $mount = ''
  $dk = $vd | Get-Disk
  if ($dk) { $p = Get-Partition -DiskNumber $dk.Number | Where-Object { $_.DriveLetter } | Select-Object -First 1; if ($p) { $mount = "$($p.DriveLetter):\" } }
  $pct = -1
  $job = Get-StorageJob | Where-Object { $_.JobState -eq 'Running' } | Select-Object -First 1
  if ($job -and ([string]$vd.OperationalStatus) -match 'Service') { $pct = [int]$job.PercentComplete }
  [ordered]@{ UniqueId = [string]$vd.UniqueId; FriendlyName = [string]$vd.FriendlyName; Size = [int64]$vd.Size
    Resiliency = [string]$vd.ResiliencySettingName; Columns = [int]$vd.NumberOfColumns; Health = [string]$vd.HealthStatus
    Operational = [string](($vd.OperationalStatus) -join ','); Mount = $mount; Percent = $pct; Members = $members }
})
}
)PS";

constexpr const char* kListScript = R"PS(
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$poolOk = $false
try { $poolOk = [bool](Get-StorageSubSystem -ErrorAction Stop | Where-Object { $_.FriendlyName -like 'Windows Storage*' }) } catch {}
$pooled = @{}
Get-StoragePool -IsPrimordial $false | ForEach-Object { $_ | Get-PhysicalDisk | ForEach-Object { $pooled[$_.UniqueId] = $true } }
$disks = @(Get-Disk | ForEach-Object {
  $d = $_
  $pd = Get-PhysicalDisk | Where-Object { $_.DeviceId -eq [string]$d.Number } | Select-Object -First 1
  $vols = @(Get-Partition -DiskNumber $d.Number | ForEach-Object {
    $p = $_
    $v = $p | Get-Volume
    if ($v) {
      $path = ''
      if ($p.DriveLetter) { $path = "$($p.DriveLetter):\" } else { $path = [string]($p.AccessPaths | Where-Object { $_ -notlike '\\?\*' } | Select-Object -First 1) }
      $folders = 0; $cat = $false
      if ($path) {
        $folders = @(Get-ChildItem -LiteralPath $path -Directory | Where-Object { -not $_.Name.StartsWith('.') -and -not ($_.Attributes -band [IO.FileAttributes]::Hidden) }).Count
        $cat = Test-Path -LiteralPath (Join-Path $path '.mediaviewer')
      }
      $sys = [bool]($p.IsSystem -or $p.IsBoot -or ($path -and $path.TrimEnd('\') -ieq $env:SystemDrive))
      [ordered]@{ Path = $path; Label = [string]$v.FileSystemLabel; Size = [int64]$v.Size; Free = [int64]$v.SizeRemaining; Folders = $folders; Catalogue = $cat; System = $sys }
    }
  })
  $bus = [string]$d.BusType
  $img = ''
  if ($bus -eq 'File Backed Virtual') { $img = [string]$d.Location }
  [ordered]@{
    Number = [int]$d.Number; FriendlyName = [string]$d.FriendlyName; Model = [string]$d.Model; UniqueId = [string]$d.UniqueId
    Size = [int64]$d.Size; IsSystem = [bool]$d.IsSystem; IsBoot = [bool]$d.IsBoot; IsReadOnly = [bool]$d.IsReadOnly
    IsOffline = [bool]$d.IsOffline; BusType = $bus; ImagePath = $img
    InPool = [bool](($pd -and $pooled.ContainsKey($pd.UniqueId)) -or $bus -eq 'Spaces')
    Health = [string]$pd.HealthStatus; Operational = [string](($pd.OperationalStatus) -join ',')
    Volumes = $vols
  }
})
[ordered]@{ pool_ok = $poolOk; disks = $disks; sets = (& $setsOf) } | ConvertTo-Json -Depth 6 -Compress
)PS";

// Runs a script, elevated through UAC or not; its output is what it wrote to its result file.
struct script_run {
  bool started = false;
  int code = -1;
  std::string out;
};

script_run run_script(const std::string& body, bool elevate) {
  script_run r;
  const std::string ps1 = temp_file(".ps1"), result = temp_file(".txt");
  const std::string script = "$ErrorActionPreference = 'Stop'\n[Console]::OutputEncoding = [Text.Encoding]::UTF8\n"
                             "try {\n  & {\n" + body + "\n  } | Out-File -LiteralPath " + ps_quote(result) +
                             " -Encoding utf8\n  exit 0\n} catch {\n  $_.Exception.Message | Out-File -LiteralPath " +
                             ps_quote(result) + " -Encoding utf8\n  exit 1\n}\n";
  if (!write_file(ps1, script)) return r;
  const std::wstring params = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + wide(ps1) + L"\"";
  if (elevate) {
    const std::wstring exe = wide(powershell());
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (::ShellExecuteExW(&sei) && sei.hProcess) {
      r.started = true;
      ::WaitForSingleObject(sei.hProcess, INFINITE);
      DWORD code = 1;
      ::GetExitCodeProcess(sei.hProcess, &code);
      ::CloseHandle(sei.hProcess);
      r.code = static_cast<int>(code);
    } else if (::GetLastError() == ERROR_CANCELLED) {
      r.started = true;
      r.code = 1;
      r.out = "the administrator prompt was cancelled; nothing was erased";
      remove_file(ps1);
      return r;
    }
  } else {
    int code = 0;
    const std::vector<std::string> args = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", ps1};
    if (run_tool(powershell(), args, code)) {
      r.started = true;
      r.code = code;
    }
  }
  r.out = read_file(result);
  remove_file(ps1);
  remove_file(result);
  return r;
}

std::string first_line(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  const auto nl = s.find('\n');
  if (nl != std::string::npos) s.resize(nl);
  if (!s.empty() && s.back() == '\r') s.pop_back();
  return s;
}

std::string mount_in(const std::string& out) {
  const auto at = out.find("MOUNT ");
  if (at == std::string::npos) return {};
  return first_line(out.substr(at + 6));
}

class win_port final : public port {
 public:
  listing list() override { return listed(std::string(kSetsScript) + kListScript); }

  listing sets_only() override {
    return listed(std::string(kSetsScript) +
                  "[Console]::OutputEncoding = [Text.Encoding]::UTF8\n"
                  "[ordered]@{ pool_ok = $true; disks = @(); sets = (& $setsOf) } | ConvertTo-Json -Depth 6 -Compress\n");
  }

 private:
  listing listed(const std::string& script) {
    listing l;
    const script_run r = run_script(script, false);
    const auto doc = r.code == 0 ? json::parse(r.out, 16) : std::nullopt;
    const auto w = doc ? parse_windows(*doc) : std::nullopt;
    if (!w) {
      l.error = r.started ? "Windows' storage commands could not list the drives" : "PowerShell could not be started";
      return l;
    }
    l.disks = w->disks;
    l.sets = w->sets;
    l.pool_ok = w->pool_ok;
    unique_names(l.disks);
    return l;
  }

 public:
  run_result create(const plan& p, bool elevate) override {
    run_result res;
    const std::string body = windows_create_script(p);
    if (body.empty()) {
      res.why = "Windows cannot make that";
      return res;
    }
    const script_run r = run_script(body, elevate);
    if (!r.started) {
      res.why = "PowerShell could not be started";
    } else if (r.code != 0) {
      res.why = first_line(r.out);
      if (res.why.empty()) res.why = "Storage Spaces failed";
    } else {
      res.ok = true;
      res.mount = mount_in(r.out);
    }
    return res;
  }

  run_result replace(const set_health& s, const disk& fresh, bool elevate) override {
    run_result res;
    const std::string body = windows_replace_script(s, fresh);
    if (body.empty()) {
      res.why = "only a mirror or parity space can take a new drive and rebuild";
      return res;
    }
    const script_run r = run_script(body, elevate);
    res.ok = r.started && r.code == 0;
    if (!res.ok) res.why = r.started ? first_line(r.out) : "PowerShell could not be started";
    if (res.ok) res.mount = s.mount;
    return res;
  }

  void notify(const std::string& title, const std::string& body) override {
    const std::string script =
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null\n"
        "$t = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02)\n"
        "$x = $t.GetElementsByTagName('text')\n"
        "$x.Item(0).AppendChild($t.CreateTextNode(" + ps_quote(title) + ")) | Out-Null\n"
        "$x.Item(1).AppendChild($t.CreateTextNode(" + ps_quote(body) + ")) | Out-Null\n"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\WindowsPowerShell\\v1.0\\powershell.exe').Show([Windows.UI.Notifications.ToastNotification]::new($t))\n";
    (void)run_script(script, false);
  }
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<win_port>(); }

}  // namespace mv::driftwire::cli::drives
