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

#include <sodium.h>

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

void remove_file(const std::string& path) { ::DeleteFileW(wide(path).c_str()); }

// Every listing starts with this. A storage cmdlet that finds nothing (no pool, a blank disk's
// partitions) reports an error, and the storage module takes the wrapper's 'Stop', not this
// script's preference: without a default -ErrorAction it ends the listing (exit 5).
constexpr const char* kSetsScript = R"PS(
$PSDefaultParameterValues = @{ '*:ErrorAction' = 'SilentlyContinue' }
$setsOf = {
$ErrorActionPreference = 'SilentlyContinue'  # a detached space has no disk or partition: still listed, never an error
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

// The script's text around a body: errors and refusals end it with a code (exit_words).
std::string wrapped(const std::string& body) {
  return "$ErrorActionPreference = 'Stop'\n[Console]::OutputEncoding = [Text.Encoding]::UTF8\n$global:mvMount = ''\n"
         "try {\n& {\n" + body + "\n}\n} catch {\n$_.Exception.Message\nexit 5\n}\n"
         "if ($global:mvMount) { 'MOUNT ' + $global:mvMount + ':\\'; exit [int][char]$global:mvMount }\nexit 0\n";
}

std::string base64(const std::string& bytes) {
  static constexpr char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                       static_cast<unsigned char>(bytes[i + 2]);
    out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], k[v & 63]};
  }
  if (i + 1 == bytes.size()) {
    const unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
    out += {k[(v >> 18) & 63], k[(v >> 12) & 63], '=', '='};
  } else if (i + 2 == bytes.size()) {
    const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
    out += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], '='};
  }
  return out;
}

// Runs a script, elevated through UAC or not. The elevated run never runs a file
// another process could change under it: a short bootstrap (on its command line,
// -EncodedCommand) reads the script into memory once, checks its SHA-256 against
// the one given, and runs those bytes. It writes no file (an elevated write into
// the person's %TEMP% could be redirected): what it did is its exit code.
script_run run_script(const std::string& body, bool elevate) {
  script_run r;
  const std::string ps1 = temp_file(".ps1");
  const std::string text = wrapped(body);
  if (!write_file(ps1, text)) return r;
  if (elevate) {
    const std::string bytes = "\xEF\xBB\xBF" + text;  // as write_file wrote it
    std::uint8_t digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    char hex[2 * crypto_hash_sha256_BYTES + 1] = {};
    for (std::size_t i = 0; i < sizeof digest; ++i) std::snprintf(hex + 2 * i, 3, "%02X", digest[i]);
    const std::string boot =
        "$b = [IO.File]::ReadAllBytes(" + ps_quote(ps1) + "); "
        "$h = -join ([Security.Cryptography.SHA256]::Create().ComputeHash($b) | ForEach-Object { $_.ToString('X2') }); "
        "if ($h -ne '" + std::string(hex) + "') { exit 6 }; "
        "& ([scriptblock]::Create([Text.Encoding]::UTF8.GetString($b, 3, $b.Length - 3)))";
    std::string utf16;
    for (const char c : boot) {
      utf16 += c;
      utf16 += '\0';
    }
    const std::wstring params = L"-NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand " + wide(base64(utf16));
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
    }
  } else {
    int code = 0;
    const std::vector<std::string> args = {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", ps1};
    if (auto out = run_tool(powershell(), args, code)) {
      r.started = true;
      r.code = code;
      r.out = std::move(*out);
    }
  }
  remove_file(ps1);
  return r;
}

// An exit code's meaning (the scripts' `exit N`, drives.cpp).
std::string exit_words(int code, const std::string& said) {
  switch (code) {
    case 2: return "a drive changed since it was confirmed; nothing was erased";
    case 3: return "that drive holds this computer's system; nothing was erased";
    case 4: return "Windows cannot pool these drives (Storage Spaces says they are not ready)";
    case 6: return "the script changed before it ran; nothing was erased";
    default: break;
  }
  return said.empty() ? std::string("Storage Spaces failed") : said;
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
    } else if (r.code == 0 || (r.code >= 'A' && r.code <= 'Z')) {
      res.ok = true;
      res.mount = r.code >= 'A' ? std::string(1, static_cast<char>(r.code)) + ":\\" : mount_in(r.out);
    } else {
      res.why = r.out == "the administrator prompt was cancelled; nothing was erased" ? r.out : exit_words(r.code, first_line(r.out));
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
    if (!res.ok) res.why = r.started ? exit_words(r.code, first_line(r.out)) : "PowerShell could not be started";
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
