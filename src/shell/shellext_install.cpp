// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/shellext_install.h"

#include <windows.h>
#include <shlobj.h>

#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/trace.h"
#include "shell/os_integration.h"

namespace mv::shell {
namespace {

namespace fs = std::filesystem;

// The shell's thumbnail handler slot (IThumbnailProvider).
constexpr wchar_t kThumbnailShellEx[] = L"{e357fccd-a995-4576-b01f-234630154e96}";

// True when the value was written: it was missing or different.
bool write_reg_string(const std::wstring& subkey, const wchar_t* name, const std::wstring& value,
                      bool& failed) {
  wchar_t buf[2 * MAX_PATH]{};
  DWORD bytes = sizeof(buf);
  if (::RegGetValueW(HKEY_CURRENT_USER, subkey.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &bytes) ==
          ERROR_SUCCESS &&
      value == buf) {
    return false;
  }
  const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  if (::RegSetKeyValueW(HKEY_CURRENT_USER, subkey.c_str(), name, REG_SZ, value.c_str(), size) !=
      ERROR_SUCCESS) {
    failed = true;
    return false;
  }
  return true;
}

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

fs::path module_dir() {
  wchar_t exe[2 * MAX_PATH]{};
  const DWORD n = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
  if (n == 0 || n >= std::size(exe)) return {};
  return fs::path(exe).parent_path();
}

// The DLL names a 64-bit PE image imports, eagerly or delay-loaded. Empty for
// anything that is not one. Reads headers only through bounds-checked copies.
std::vector<std::string> pe_imports(const fs::path& file) {
  std::vector<std::string> out;
  std::ifstream in(file, std::ios::binary);
  if (!in) return out;
  const std::string img{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const auto fits = [&](std::size_t off, std::size_t n) { return off <= img.size() && n <= img.size() - off; };
  IMAGE_DOS_HEADER dos{};
  if (!fits(0, sizeof(dos))) return out;
  std::memcpy(&dos, img.data(), sizeof(dos));
  if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) return out;
  const auto nt_off = static_cast<std::size_t>(dos.e_lfanew);
  IMAGE_NT_HEADERS64 nt{};
  if (!fits(nt_off, sizeof(nt))) return out;
  std::memcpy(&nt, img.data() + nt_off, sizeof(nt));
  if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return out;

  std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
  const std::size_t sec_off =
      nt_off + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
  if (!fits(sec_off, sections.size() * sizeof(IMAGE_SECTION_HEADER))) return out;
  std::memcpy(sections.data(), img.data() + sec_off, sections.size() * sizeof(IMAGE_SECTION_HEADER));
  constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  const auto to_off = [&](DWORD rva) -> std::size_t {
    for (const IMAGE_SECTION_HEADER& s : sections) {
      const DWORD span = s.Misc.VirtualSize > s.SizeOfRawData ? s.Misc.VirtualSize : s.SizeOfRawData;
      if (rva >= s.VirtualAddress && rva - s.VirtualAddress < span) {
        return static_cast<std::size_t>(s.PointerToRawData) + (rva - s.VirtualAddress);
      }
    }
    return kNone;
  };
  const auto name_at = [&](DWORD rva) {
    const std::size_t off = to_off(rva);
    if (off == kNone || off >= img.size()) return std::string{};
    const std::size_t end = img.find('\0', off);
    if (end == std::string::npos || end - off > MAX_PATH) return std::string{};
    return img.substr(off, end - off);
  };
  const auto& dirs = nt.OptionalHeader.DataDirectory;
  if (std::size_t off = to_off(dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress); off != kNone) {
    for (int i = 0; i < 4096; ++i, off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
      IMAGE_IMPORT_DESCRIPTOR d{};
      if (!fits(off, sizeof(d))) break;
      std::memcpy(&d, img.data() + off, sizeof(d));
      if (d.Name == 0) break;
      if (std::string n = name_at(d.Name); !n.empty()) out.push_back(std::move(n));
    }
  }
  if (std::size_t off = to_off(dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress); off != kNone) {
    for (int i = 0; i < 4096; ++i, off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
      IMAGE_DELAYLOAD_DESCRIPTOR d{};
      if (!fits(off, sizeof(d))) break;
      std::memcpy(&d, img.data() + off, sizeof(d));
      if (d.DllNameRVA == 0) break;
      if (std::string n = name_at(d.DllNameRVA); !n.empty()) out.push_back(std::move(n));
    }
  }
  return out;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// `files` plus every DLL beside the app that they import, transitively.
// MediaViewerThumbs.files comes from $<TARGET_RUNTIME_DLLS>, which leaves out
// imported libraries CMake types UNKNOWN (libjpeg, libtiff via their Find
// modules) and the DLLs those DLLs load (libde265 under libheif, zlib under
// libtiff). Without them the handler does not load and Explorer shows no
// thumbnails, while the app, beside every DLL, works. System DLLs are not
// beside the app and stay the loader's business.
std::vector<std::string> with_import_closure(const fs::path& dir, std::vector<std::string> files) {
  std::set<std::string> seen;
  for (const std::string& f : files) seen.insert(lower(f));
  std::error_code ec;
  for (std::size_t i = 0; i < files.size(); ++i) {
    const std::string name = files[i];
    if (lower(fs::path(widen(name)).extension().string()) != ".dll") continue;
    for (const std::string& imp : pe_imports(dir / widen(name))) {
      if (imp.find_first_of("\\/:") != std::string::npos) continue;  // a bare file name only
      if (!seen.insert(lower(imp)).second) continue;
      if (fs::is_regular_file(dir / widen(imp), ec)) files.push_back(imp);
    }
  }
  return files;
}

// Copies every listed file from `from` into `to` through a staging folder, so
// a half-copied version is never registered. True when `to` is complete.
bool stage_version(const fs::path& from, const fs::path& to, const std::vector<std::string>& files) {
  std::error_code ec;
  bool complete = fs::is_directory(to, ec);
  for (const std::string& f : files) {
    if (!complete) break;
    complete = fs::is_regular_file(to / widen(f), ec);
  }
  if (complete) return true;
  fs::path staging = to;
  staging += L".partial";
  fs::remove_all(staging, ec);
  if (!fs::create_directories(staging, ec) && ec) return false;
  for (const std::string& f : files) {
    const std::wstring name = widen(f);
    if (!fs::copy_file(from / name, staging / name, fs::copy_options::overwrite_existing, ec)) {
      MV_LOG_WARN("shellext: a handler file could not be staged");
      fs::remove_all(staging, ec);
      return false;
    }
  }
  fs::remove_all(to, ec);  // an incomplete earlier attempt
  fs::rename(staging, to, ec);
  return !ec;
}

}  // namespace

void install_thumbnail_handler(const std::wstring& root, const std::string& version) noexcept {
  try {
    if (root.empty() || version.empty()) return;
    const fs::path current = module_dir();
    if (current.empty()) return;
    std::ifstream list_file(current / L"MediaViewerThumbs.files", std::ios::binary);
    if (!list_file) {
      MV_LOG_WARN("shellext: no MediaViewerThumbs.files beside the app; handler not installed");
      return;
    }
    const std::string text{std::istreambuf_iterator<char>(list_file), std::istreambuf_iterator<char>()};
    std::vector<std::string> files = parse_shellext_file_list(text);
    if (files.empty() || files.front() != "MediaViewerThumbs.dll") return;
    files = with_import_closure(current, std::move(files));

    const fs::path base = fs::path(root) / L"shellext";
    std::vector<std::string> existing;
    std::error_code ec;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
      if (it->is_directory(ec)) existing.push_back(it->path().filename().string());
    }
    const shellext_plan plan = plan_shellext_install(version, existing);
    if (plan.version_dir.empty()) return;
    const fs::path target = base / widen(plan.version_dir);
    if (!stage_version(current, target, files)) return;

    const std::wstring clsid_key = std::wstring(L"Software\\Classes\\CLSID\\") + kThumbHandlerClsid;
    const std::wstring dll = (target / L"MediaViewerThumbs.dll").wstring();
    bool failed = false;
    bool changed = false;
    changed |= write_reg_string(clsid_key, nullptr, L"MediaViewer thumbnail handler", failed);
    changed |= write_reg_string(clsid_key, L"AppID", kThumbHandlerAppId, failed);
    changed |= write_reg_string(clsid_key + L"\\InprocServer32", nullptr, dll, failed);
    changed |= write_reg_string(clsid_key + L"\\InprocServer32", L"ThreadingModel", L"Apartment", failed);
    // The handler's own surrogate: an empty DllSurrogate is the system dllhost.
    changed |= write_reg_string(std::wstring(L"Software\\Classes\\AppID\\") + kThumbHandlerAppId,
                                L"DllSurrogate", L"", failed);
    changed |= write_reg_string(std::wstring(L"Software\\Classes\\MediaViewer.Image\\ShellEx\\") +
                                    kThumbnailShellEx,
                                nullptr, kThumbHandlerClsid, failed);
    if (failed) MV_LOG_WARN("shellext: a registry write failed; thumbnails may not show");
    if (changed) ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);

    // Older versions: removed once no surrogate holds them. A folder that is
    // still in use stays until a later start.
    for (const std::string& old : plan.prune) {
      const fs::path dir = base / widen(old);
      if (dir == target) continue;
      fs::remove_all(dir, ec);
    }
  } catch (...) {
    MV_LOG_WARN("shellext: handler install failed");
  }
}

}  // namespace mv::shell
