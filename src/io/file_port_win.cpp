// SPDX-License-Identifier: GPL-2.0-or-later
// Windows half of the file port (io/file_port.h). I/O workers only.
#include "io/file_port.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

namespace mv::io {
namespace {

std::wstring wide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), out.data(), n) != n) {
    return {};
  }
  return out;
}

std::string narrow(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
  return out;
}

// 100 ns intervals between 1601-01-01 and 1970-01-01.
constexpr std::int64_t kEpochDelta = 116444736000000000LL;

std::int64_t unix_from_filetime(const FILETIME& ft) noexcept {
  const std::int64_t t = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
  return (t - kEpochDelta) / 10000000LL;
}

FILETIME filetime_from_unix(std::int64_t s) noexcept {
  const std::int64_t t = s * 10000000LL + kEpochDelta;
  FILETIME ft;
  ft.dwLowDateTime = static_cast<DWORD>(t & 0xFFFFFFFF);
  ft.dwHighDateTime = static_cast<DWORD>(static_cast<std::uint64_t>(t) >> 32);
  return ft;
}

bool exists_error(DWORD e) noexcept { return e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS; }

}  // namespace

result<file_stat> stat_path(std::string_view utf8_path) {
  const std::wstring path = wide(utf8_path);
  if (path.empty()) return err(status::invalid_arg);
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return err(status::io);
  file_stat out;
  out.is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  out.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  out.mtime_unix = unix_from_filetime(data.ftLastWriteTime);
  return out;
}

expected make_directories(std::string_view utf8_dir) {
  std::wstring dir = wide(utf8_dir);
  if (dir.empty()) return err(status::invalid_arg);
  while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  const DWORD attr = ::GetFileAttributesW(dir.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES) {
    return (attr & FILE_ATTRIBUTE_DIRECTORY) ? expected{} : err(status::io);
  }
  const std::string utf8 = narrow(dir);
  const std::string_view parent = parent_of(utf8);
  if (!parent.empty() && parent.size() < utf8.size()) MV_TRY_VOID(make_directories(parent));
  if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
    return err(status::io);
  }
  return {};
}

expected remove_file(std::string_view utf8_path) {
  const std::wstring path = wide(utf8_path);
  if (path.empty()) return err(status::invalid_arg);
  if (::DeleteFileW(path.c_str())) return {};
  const DWORD e = ::GetLastError();
  if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return {};
  return err(status::io);
}

namespace {

bool remove_tree_w(const std::wstring& dir) {
  const DWORD attr = ::GetFileAttributesW(dir.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return true;
  if (!(attr & FILE_ATTRIBUTE_DIRECTORY) || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
    // A file, or a junction: remove the entry itself, never what it points at.
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return ::RemoveDirectoryW(dir.c_str()) != 0;
    ::SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    return ::DeleteFileW(dir.c_str()) != 0;
  }
  WIN32_FIND_DATAW fd{};
  const std::wstring glob = dir + L"\\*";
  HANDLE find = ::FindFirstFileExW(glob.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                   nullptr, 0);
  bool ok = true;
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring_view name(fd.cFileName);
      if (name == L"." || name == L"..") continue;
      ok = remove_tree_w(dir + L"\\" + std::wstring(name)) && ok;
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
  }
  return ::RemoveDirectoryW(dir.c_str()) != 0 && ok;
}

}  // namespace

expected remove_tree(std::string_view utf8_dir) {
  std::wstring dir = wide(utf8_dir);
  if (dir.empty()) return err(status::invalid_arg);
  while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  return remove_tree_w(dir) ? expected{} : err(status::io);
}

result<rename_outcome> rename_no_replace(std::string_view from_utf8, std::string_view to_utf8) {
  const std::wstring from = wide(from_utf8);
  const std::wstring to = wide(to_utf8);
  if (from.empty() || to.empty()) return err(status::invalid_arg);
  // No MOVEFILE_REPLACE_EXISTING (never an overwrite) and no COPY_ALLOWED (the
  // temp file is always beside its final name, on the same volume).
  // WRITE_THROUGH: the rename is on disk before F8 across volumes deletes the
  // source.
  if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return rename_outcome::renamed;
  }
  if (exists_error(::GetLastError())) return rename_outcome::name_taken;
  return err(status::io);
}

// ---------------------------------------------------------------------------
struct file_reader::impl {
  HANDLE h = INVALID_HANDLE_VALUE;
  std::uint64_t size = 0;
  bool eof = false;
  ~impl() {
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  }
};

file_reader::file_reader() = default;
file_reader::~file_reader() = default;
file_reader::file_reader(file_reader&&) noexcept = default;
file_reader& file_reader::operator=(file_reader&&) noexcept = default;

expected file_reader::open(std::string_view utf8_path, read_mode mode) {
  close();
  const std::wstring path = wide(utf8_path);
  if (path.empty()) return err(status::invalid_arg);
  // FILE_FLAG_NO_BUFFERING: the read-back comes from the device, not from the
  // pages the writer just filled (plan/18 "read back uncached").
  const DWORD flags = mode == read_mode::uncached ? FILE_FLAG_NO_BUFFERING
                                                  : FILE_FLAG_SEQUENTIAL_SCAN;
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           flags, nullptr);
  if (h == INVALID_HANDLE_VALUE) return err(status::io);
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0) {
    ::CloseHandle(h);
    return err(status::io);
  }
  impl_ = std::make_unique<impl>();
  impl_->h = h;
  impl_->size = static_cast<std::uint64_t>(size.QuadPart);
  return {};
}

result<std::size_t> file_reader::read(std::span<std::uint8_t> into) {
  if (!impl_) return err(status::invalid_arg);
  std::size_t filled = 0;
  // With NO_BUFFERING a short read means end of file, and the file pointer is
  // then unaligned: another ReadFile would fail, so stop asking.
  while (filled < into.size() && !impl_->eof) {
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(into.size() - filled, 1u << 30));
    DWORD got = 0;
    if (!::ReadFile(impl_->h, into.data() + filled, want, &got, nullptr)) return err(status::io);
    filled += got;
    if (got < want) impl_->eof = true;
  }
  return filled;
}

std::uint64_t file_reader::size() const noexcept { return impl_ ? impl_->size : 0; }
void file_reader::close() noexcept { impl_.reset(); }

// ---------------------------------------------------------------------------
struct file_writer::impl {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~impl() {
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  }
};

file_writer::file_writer() = default;
file_writer::~file_writer() = default;
file_writer::file_writer(file_writer&&) noexcept = default;
file_writer& file_writer::operator=(file_writer&&) noexcept = default;

result<rename_outcome> file_writer::create_new(std::string_view utf8_path) {
  impl_.reset();
  const std::wstring path = wide(utf8_path);
  if (path.empty()) return err(status::invalid_arg);
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (exists_error(::GetLastError())) return rename_outcome::name_taken;
    return err(status::io);
  }
  impl_ = std::make_unique<impl>();
  impl_->h = h;
  return rename_outcome::renamed;
}

expected file_writer::write(std::span<const std::uint8_t> bytes) {
  if (!impl_) return err(status::invalid_arg);
  std::size_t done = 0;
  while (done < bytes.size()) {
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 30));
    DWORD wrote = 0;
    if (!::WriteFile(impl_->h, bytes.data() + done, want, &wrote, nullptr) || wrote == 0) {
      return err(status::io);
    }
    done += wrote;
  }
  return {};
}

expected file_writer::flush_durable() {
  if (!impl_) return err(status::invalid_arg);
  return ::FlushFileBuffers(impl_->h) ? expected{} : err(status::io);
}

expected file_writer::set_mtime(std::int64_t mtime_unix) {
  if (!impl_) return err(status::invalid_arg);
  const FILETIME ft = filetime_from_unix(mtime_unix);
  return ::SetFileTime(impl_->h, nullptr, nullptr, &ft) ? expected{} : err(status::io);
}

expected file_writer::close() {
  if (!impl_) return {};
  HANDLE h = impl_->h;
  impl_->h = INVALID_HANDLE_VALUE;
  impl_.reset();
  return ::CloseHandle(h) ? expected{} : err(status::io);
}

// ---------------------------------------------------------------------------
namespace {

bool walk_dir(const std::wstring& dir, const std::string& rel, int depth, int max_depth,
              std::vector<tree_entry>& out) {
  std::wstring glob = dir;
  if (!glob.empty() && glob.back() != L'\\' && glob.back() != L'/') glob.push_back(L'\\');
  const std::wstring prefix = glob;
  glob.push_back(L'*');
  WIN32_FIND_DATAW fd{};
  HANDLE find = ::FindFirstFileExW(glob.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) return depth > 0;
  std::vector<std::wstring> subdirs;
  do {
    const std::wstring_view name(fd.cFileName);
    if (name == L"." || name == L"..") continue;
    if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
    // Dot names are hidden on the Mac side of a card and are skipped there
    // (AppleDouble "._IMG_0001.JPG", ".Trashes", ".Spotlight-V100"): the same
    // card must list the same files on both platforms.
    if (name.front() == L'.') continue;
    // Junctions and symlinks are never followed: a card has none, and a share
    // that loops back on itself must not walk forever.
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (depth < max_depth) subdirs.emplace_back(name);
      continue;
    }
    const std::string name8 = narrow(name);
    tree_entry e;
    e.path_utf8 = narrow(prefix + std::wstring(name));
    e.relative_utf8 = rel.empty() ? name8 : rel + "/" + name8;
    e.name_utf8 = name8;
    e.size = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    e.mtime_unix = unix_from_filetime(fd.ftLastWriteTime);
    out.push_back(std::move(e));
  } while (::FindNextFileW(find, &fd));
  ::FindClose(find);
  for (const std::wstring& sub : subdirs) {
    const std::string sub8 = narrow(sub);
    walk_dir(prefix + sub, rel.empty() ? sub8 : rel + "/" + sub8, depth + 1, max_depth, out);
  }
  return true;
}

bool walk_all_dir(const std::wstring& dir, const std::string& rel, int depth, int max_depth,
                  const std::function<bool(std::string_view, entry_kind)>& visit, bool& stopped) {
  WIN32_FIND_DATAW fd{};
  HANDLE find = ::FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return false;
  std::vector<std::wstring> subdirs;
  do {
    const std::wstring_view name(fd.cFileName);
    if (name == L"." || name == L"..") continue;
    const std::string name8 = narrow(name);
    const std::string child_rel = rel.empty() ? name8 : rel + "/" + name8;
    // Hidden and system entries are reported like any other; a reparse point
    // (symlink, junction) is never followed.
    entry_kind kind = entry_kind::other;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        kind = entry_kind::file;
      } else if (depth < max_depth) {
        kind = entry_kind::directory;
      }
    }
    if (!visit(child_rel, kind)) {
      stopped = true;
      break;
    }
    if (kind == entry_kind::directory) subdirs.emplace_back(name);
  } while (::FindNextFileW(find, &fd));
  ::FindClose(find);
  bool ok = true;
  for (const std::wstring& sub : subdirs) {
    if (stopped) break;
    const std::string sub8 = narrow(sub);
    ok = walk_all_dir(dir + L"\\" + sub, rel.empty() ? sub8 : rel + "/" + sub8, depth + 1,
                      max_depth, visit, stopped) &&
         ok;
  }
  return ok;
}

}  // namespace

expected walk_all_entries(std::string_view utf8_root, int max_depth,
                          const std::function<bool(std::string_view, entry_kind)>& visit) {
  std::wstring root = wide(utf8_root);
  if (root.empty()) return err(status::invalid_arg);
  while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
  bool stopped = false;
  const bool ok = walk_all_dir(root, std::string(), 0, max_depth, visit, stopped);
  if (stopped) return err(status::cancelled);
  return ok ? expected{} : err(status::io);
}

result<std::vector<std::string>> child_directories(std::string_view utf8_dir) {
  std::wstring dir = wide(utf8_dir);
  if (dir.empty()) return err(status::invalid_arg);
  if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
  WIN32_FIND_DATAW fd{};
  HANDLE find = ::FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) return err(status::io);
  std::vector<std::string> out;
  do {
    const std::wstring_view name(fd.cFileName);
    if (name == L"." || name == L".." || name.empty() || name.front() == L'.') continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (fd.dwFileAttributes &
        (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) {
      continue;
    }
    out.push_back(narrow(name));
  } while (::FindNextFileW(find, &fd));
  ::FindClose(find);
  std::sort(out.begin(), out.end());
  return out;
}

expected walk_files(std::string_view utf8_root, int max_depth,
                    const std::function<bool(const tree_entry&)>& visit) {
  const std::wstring root = wide(utf8_root);
  if (root.empty()) return err(status::invalid_arg);
  std::vector<tree_entry> entries;
  if (!walk_dir(root, std::string(), 0, max_depth, entries)) return err(status::io);
  std::sort(entries.begin(), entries.end(), [](const tree_entry& a, const tree_entry& b) {
    return a.relative_utf8 < b.relative_utf8;
  });
  for (const tree_entry& e : entries) {
    if (!visit(e)) return err(status::cancelled);
  }
  return {};
}

}  // namespace mv::io
