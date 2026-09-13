// SPDX-License-Identifier: GPL-2.0-or-later
// Windows copy / move / Recycle Bin. I/O pool only (file_ops.h).
#include "io/file_ops.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>  // FOF_* (excluded by WIN32_LEAN_AND_MEAN)
#include <shlobj.h>
#include <shobjidl.h>

#include <string>

#include "io/collision_name.h"

namespace mv::io {
namespace {

std::wstring wide_from_utf8(std::string_view utf8) {
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

std::string utf8_from_wide(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
  return out;
}

std::wstring_view file_name_of(std::wstring_view path) noexcept {
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring_view::npos ? path : path.substr(slash + 1);
}

bool path_taken(const std::wstring& path) noexcept {
  return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool file_size(const std::wstring& path, ULONGLONG& out) noexcept {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
  out = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
  return true;
}

// Whether `file` already lives in `dir` (full paths, ASCII case and trailing
// separators ignored). Moving a file into its own folder is then a no-op.
std::wstring full_path(const std::wstring& p) {
  const DWORD need = ::GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
  std::wstring out;
  if (need > 0) {
    out.resize(need);
    const DWORD n = ::GetFullPathNameW(p.c_str(), need, out.data(), nullptr);
    out.resize(n > 0 && n < need ? n : 0);
  }
  if (out.empty()) out = p;
  while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
  return out;
}

bool same_directory(const std::wstring& file, const std::wstring& dir) {
  const auto slash = file.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return false;
  const std::wstring a = full_path(file.substr(0, slash));
  const std::wstring b = full_path(dir);
  return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool exists_error(DWORD e) noexcept { return e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS; }

// One attempt at one destination name. `exists` means "try the next name".
enum class attempt { done, exists, failed };

attempt copy_to(const std::wstring& src, const std::wstring& dest) noexcept {
  if (::CopyFileExW(src.c_str(), dest.c_str(), nullptr, nullptr, nullptr,
                    COPY_FILE_FAIL_IF_EXISTS)) {
    return attempt::done;
  }
  const DWORD e = ::GetLastError();
  if (exists_error(e)) return attempt::exists;
  // Not ours to keep: with FAIL_IF_EXISTS anything at `dest` now is a partial
  // copy this call made.
  ::DeleteFileW(dest.c_str());
  return attempt::failed;
}

attempt move_to(const std::wstring& src, const std::wstring& dest) noexcept {
  // Same volume: a rename, and never MOVEFILE_REPLACE_EXISTING.
  if (::MoveFileExW(src.c_str(), dest.c_str(), 0)) return attempt::done;
  const DWORD e = ::GetLastError();
  if (exists_error(e)) return attempt::exists;
  if (e != ERROR_NOT_SAME_DEVICE) return attempt::failed;

  // Across volumes: copy, check the copy is whole, and only then let the
  // source go. Any failure leaves the source and removes the copy.
  const attempt copied = copy_to(src, dest);
  if (copied != attempt::done) return copied;
  ULONGLONG src_size = 0;
  ULONGLONG dest_size = 0;
  if (!file_size(src, src_size) || !file_size(dest, dest_size) || src_size != dest_size) {
    ::DeleteFileW(dest.c_str());
    return attempt::failed;
  }
  if (!::DeleteFileW(src.c_str())) {
    // The source is locked or read-only: a move that leaves two copies is not
    // a move. Undo the copy and report it.
    ::DeleteFileW(dest.c_str());
    return attempt::failed;
  }
  return attempt::done;
}

// Refuses any delete the shell would not send to the Recycle Bin. Lives on the
// caller's stack for exactly one PerformOperations call.
class recycle_only_sink final : public IFileOperationProgressSink {
 public:
  bool refused = false;

  IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
      *out = static_cast<IFileOperationProgressSink*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
  IFACEMETHODIMP_(ULONG) Release() override { return 1; }

  IFACEMETHODIMP PreDeleteItem(DWORD flags, IShellItem*) override {
    if (detail::pre_delete_allowed(flags)) return S_OK;
    refused = true;
    return E_ABORT;
  }

  IFACEMETHODIMP StartOperations() override { return S_OK; }
  IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
  IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
  IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override {
    return S_OK;
  }
  IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
  IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT,
                              IShellItem*) override {
    return S_OK;
  }
  IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
  IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT,
                              IShellItem*) override {
    return S_OK;
  }
  IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override {
    return S_OK;
  }
  IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
  IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT,
                             IShellItem*) override {
    return S_OK;
  }
  IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
  IFACEMETHODIMP ResetTimer() override { return S_OK; }
  IFACEMETHODIMP PauseTimer() override { return S_OK; }
  IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};

}  // namespace

namespace detail {

bool pre_delete_allowed(std::uint32_t transfer_flags) noexcept {
  return (transfer_flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) != 0;
}

std::int32_t probe_recycle_sink(std::uint32_t transfer_flags, bool& refused) noexcept {
  recycle_only_sink sink;
  const HRESULT hr = sink.PreDeleteItem(transfer_flags, nullptr);
  refused = sink.refused;
  return static_cast<std::int32_t>(hr);
}

}  // namespace detail

result<std::string> transfer_file(std::string_view src_utf8, std::string_view dest_dir_utf8,
                                  transfer_kind kind) noexcept {
  try {
    const std::wstring src = wide_from_utf8(src_utf8);
    std::wstring dir = wide_from_utf8(dest_dir_utf8);
    if (src.empty() || dir.empty()) return err(status::invalid_arg);
    const DWORD src_attr = ::GetFileAttributesW(src.c_str());
    if (src_attr == INVALID_FILE_ATTRIBUTES || (src_attr & FILE_ATTRIBUTE_DIRECTORY)) {
      return err(status::io);
    }
    const DWORD dir_attr = ::GetFileAttributesW(dir.c_str());
    if (dir_attr == INVALID_FILE_ATTRIBUTES || !(dir_attr & FILE_ATTRIBUTE_DIRECTORY)) {
      return err(status::invalid_arg);
    }
    if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');

    // F8 into the folder the file is already in: nothing to move. (F7 there
    // is different: a copy lands beside it as `name (2).ext`, as Explorer does.)
    if (kind == transfer_kind::move && same_directory(src, dir)) return utf8_from_wide(src);

    const std::string name = utf8_from_wide(file_name_of(src));
    if (name.empty()) return err(status::invalid_arg);

    // Pick a free-looking name, then write with fail-if-exists. If something
    // takes it in between, move on to the next number: never an overwrite.
    // Copying into the source's own folder lands on `name (2).ext`.
    constexpr int kAttempts = 9999;
    for (int n = 1; n <= kAttempts; ++n) {
      const std::string candidate = n == 1 ? name : collision_name(name, n);
      const std::wstring dest = dir + wide_from_utf8(candidate);
      if (dest.size() == dir.size()) return err(status::internal);
      if (path_taken(dest)) continue;
      const attempt a = kind == transfer_kind::copy ? copy_to(src, dest) : move_to(src, dest);
      if (a == attempt::done) return utf8_from_wide(dest);
      if (a == attempt::failed) return err(status::io);
    }
    return err(status::io);
  } catch (...) {
    return err(status::out_of_memory);
  }
}

result<recycle_outcome> recycle_file(std::string_view utf8_path) noexcept {
  try {
    const std::wstring path = wide_from_utf8(utf8_path);
    if (path.empty()) return err(status::invalid_arg);
    if (!path_taken(path)) return err(status::io);

    // IFileOperation is single-threaded-apartment only; MTA callers are meant
    // to use SHFileOperation. The file-job worker runs nothing else, so it
    // takes an STA for this call and gives it back. A thread already in the
    // MTA is a caller bug, not something to carry on with.
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init)) return err(status::internal);
    struct com_scope {
      ~com_scope() { ::CoUninitialize(); }
    } const scope;

    recycle_only_sink sink;
    bool performed_ok = false;
    BOOL aborted = FALSE;
    IFileOperation* op = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(&op)))) {
      // FOFX_RECYCLEONDELETE asks for the bin; the sink refuses the delete if
      // the shell says it cannot recycle. No shell UI: the app already asked.
      if (SUCCEEDED(op->SetOperationFlags(FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION |
                                          FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(path.c_str(), nullptr,
                                                    IID_PPV_ARGS(&item)))) {
          if (SUCCEEDED(op->DeleteItem(item, &sink))) {
            performed_ok = SUCCEEDED(op->PerformOperations());
            (void)op->GetAnyOperationsAborted(&aborted);
          }
          item->Release();
        }
      }
      op->Release();
    }
    return detail::recycle_outcome_from(sink.refused, performed_ok, aborted != FALSE,
                                        path_taken(path));
  } catch (...) {
    return err(status::out_of_memory);
  }
}

}  // namespace mv::io
