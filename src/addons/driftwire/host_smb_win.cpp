// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Windows' file sharing for the LAN share (docs/plans/self-host-users-smb.md §5).
// Runs only inside the elevated step.
//
//   accounts  NetUserAdd: a local account whose comment marks it as
//             MediaViewer's, in the local group "MediaViewer share users" and
//             taken out of "Users"; denied local and Remote Desktop sign-in
//             (LsaAddAccountRights); hidden from the sign-in screen. The
//             password goes through NetUserAdd / NetUserSetInfo only.
//   shares    NetShareAdd level 502, remark "MediaViewer", a share DACL that
//             lists its users alone (no Everyone, no Guest), then
//             SHI1005_FLAGS_ENCRYPT_DATA: SMB3 with encryption. The folder's
//             NTFS DACL gets an inherited ACE per user (SetNamedSecurityInfo
//             carries it to what is inside); our users' earlier ACEs go first.
//   SMB1      refused while LanmanServer\Parameters\SMB1 is set and not 0.
//
// Nothing here logs; the why strings name no path and no password.
#include "addons/driftwire/host_smb.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <lm.h>
#include <ntsecapi.h>
#include <sddl.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <vector>

#ifndef SHI1005_FLAGS_ENCRYPT_DATA
#define SHI1005_FLAGS_ENCRYPT_DATA 0x8000
#endif

namespace mv::driftwire::host_smb {

namespace {

constexpr const wchar_t* kComment = L"MediaViewer share user";
constexpr const wchar_t* kGroup = L"MediaViewer share users";
constexpr const wchar_t* kRemark = L"MediaViewer";
constexpr const wchar_t* kHidden = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\SpecialAccounts\\UserList";

std::wstring wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const wchar_t* s) {
  if (!s || !*s) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
  return out;
}

void wipe(std::wstring& s) {
  if (!s.empty()) SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
  s.clear();
}

struct net_buffer {
  LPBYTE p = nullptr;
  ~net_buffer() {
    if (p) NetApiBufferFree(p);
  }
};

bool elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION e{};
  DWORD got = 0;
  const bool yes = GetTokenInformation(token, TokenElevation, &e, sizeof e, &got) && e.TokenIsElevated != 0;
  CloseHandle(token);
  return yes;
}

std::wstring computer_name(COMPUTER_NAME_FORMAT f) {
  DWORD n = 0;
  GetComputerNameExW(f, nullptr, &n);
  if (n == 0) return {};
  std::wstring out(n, L'\0');
  if (!GetComputerNameExW(f, out.data(), &n)) return {};
  out.resize(n);
  return out;
}

// A local user account's SID (this computer's own, never a domain's or a
// well-known group's), or empty.
std::vector<BYTE> sid_of(const std::wstring& name) {
  const std::wstring here = computer_name(ComputerNameNetBIOS);
  if (here.empty()) return {};
  const std::wstring qualified = here + L"\\" + name;
  DWORD sid_n = 0, dom_n = 0;
  SID_NAME_USE use{};
  LookupAccountNameW(nullptr, qualified.c_str(), nullptr, &sid_n, nullptr, &dom_n, &use);
  if (sid_n == 0 || dom_n == 0) return {};
  std::vector<BYTE> sid(sid_n);
  std::wstring dom(dom_n, L'\0');
  if (!LookupAccountNameW(nullptr, qualified.c_str(), sid.data(), &sid_n, dom.data(), &dom_n, &use)) return {};
  dom.resize(dom_n);
  if (use != SidTypeUser || _wcsicmp(dom.c_str(), here.c_str()) != 0) return {};
  return sid;
}

// The local "Users" group's name (it is translated).
std::wstring users_group() {
  PSID sid = nullptr;
  if (!ConvertStringSidToSidW(L"S-1-5-32-545", &sid)) return L"Users";
  wchar_t name[256], dom[256];
  DWORD nn = 256, dn = 256;
  SID_NAME_USE use{};
  std::wstring out = L"Users";
  if (LookupAccountSidW(nullptr, sid, name, &nn, dom, &dn, &use)) out = name;
  LocalFree(sid);
  return out;
}

bool in_our_group(const std::wstring& user) {
  net_buffer b;
  DWORD read = 0, total = 0;
  if (NetUserGetLocalGroups(nullptr, user.c_str(), 0, LG_INCLUDE_INDIRECT, &b.p, MAX_PREFERRED_LENGTH, &read, &total) !=
      NERR_Success) {
    return false;
  }
  const auto* g = reinterpret_cast<LOCALGROUP_USERS_INFO_0*>(b.p);
  for (DWORD i = 0; i < read; ++i) {
    if (g[i].lgrui0_name && _wcsicmp(g[i].lgrui0_name, kGroup) == 0) return true;
  }
  return false;
}

bool ensure_group() {
  LOCALGROUP_INFO_1 info{};
  info.lgrpi1_name = const_cast<LPWSTR>(kGroup);
  info.lgrpi1_comment = const_cast<LPWSTR>(L"Accounts MediaViewer made for its network share");
  const NET_API_STATUS s = NetLocalGroupAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&info), nullptr);
  return s == NERR_Success || s == NERR_GroupExists || s == ERROR_ALIAS_EXISTS;
}

bool group_member(const wchar_t* group, const std::wstring& user, bool add) {
  LOCALGROUP_MEMBERS_INFO_3 m{};
  m.lgrmi3_domainandname = const_cast<LPWSTR>(user.c_str());
  const NET_API_STATUS s = add ? NetLocalGroupAddMembers(nullptr, group, 3, reinterpret_cast<LPBYTE>(&m), 1)
                               : NetLocalGroupDelMembers(nullptr, group, 3, reinterpret_cast<LPBYTE>(&m), 1);
  return s == NERR_Success || s == ERROR_MEMBER_IN_ALIAS || s == ERROR_MEMBER_NOT_IN_ALIAS;
}

bool logon_rights(const std::vector<BYTE>& sid, bool deny) {
  LSA_OBJECT_ATTRIBUTES attrs{};
  LSA_HANDLE h = nullptr;
  if (LsaOpenPolicy(nullptr, &attrs, POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES, &h) != 0) return false;
  wchar_t a[] = L"SeDenyInteractiveLogonRight";
  wchar_t b[] = L"SeDenyRemoteInteractiveLogonRight";
  LSA_UNICODE_STRING rights[2];
  rights[0].Buffer = a;
  rights[0].Length = static_cast<USHORT>(wcslen(a) * sizeof(wchar_t));
  rights[0].MaximumLength = rights[0].Length;
  rights[1].Buffer = b;
  rights[1].Length = static_cast<USHORT>(wcslen(b) * sizeof(wchar_t));
  rights[1].MaximumLength = rights[1].Length;
  PSID p = const_cast<BYTE*>(sid.data());
  const NTSTATUS s = deny ? LsaAddAccountRights(h, p, rights, 2) : LsaRemoveAccountRights(h, p, TRUE, nullptr, 0);
  LsaClose(h);
  return s == 0;
}

void hide_from_sign_in(const std::wstring& user, bool hide) {
  if (hide) {
    const DWORD zero = 0;
    (void)RegSetKeyValueW(HKEY_LOCAL_MACHINE, kHidden, user.c_str(), REG_DWORD, &zero, sizeof zero);
  } else {
    (void)RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kHidden, user.c_str());
  }
}

// ---- permissions ----
// A DACL: `base`'s ACEs for `strip` revoked, then `members` set (inherited by what is inside when `inherit`).
PACL build_dacl(PACL base, const std::vector<std::vector<BYTE>>& strip, const std::vector<member>& members, bool share) {
  std::vector<EXPLICIT_ACCESS_W> ea;
  std::vector<std::vector<BYTE>> sids;  // kept alive for the call
  sids.reserve(strip.size() + members.size());
  for (const auto& s : strip) {
    sids.push_back(s);
    EXPLICIT_ACCESS_W e{};
    e.grfAccessMode = REVOKE_ACCESS;
    e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    e.Trustee.TrusteeType = TRUSTEE_IS_USER;
    e.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sids.back().data());
    ea.push_back(e);
  }
  PACL revoked = nullptr;
  if (!ea.empty()) {
    if (SetEntriesInAclW(static_cast<ULONG>(ea.size()), ea.data(), base, &revoked) != ERROR_SUCCESS) return nullptr;
  }
  ea.clear();
  for (const auto& m : members) {
    auto sid = sid_of(wide(m.user));
    if (sid.empty()) continue;
    sids.push_back(std::move(sid));
    EXPLICIT_ACCESS_W e{};
    e.grfAccessPermissions = share ? serve::share_users::windows_share_mask(m.r) : serve::share_users::windows_ntfs_mask(m.r);
    e.grfAccessMode = SET_ACCESS;
    e.grfInheritance = share ? NO_INHERITANCE : SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    e.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    e.Trustee.TrusteeType = TRUSTEE_IS_USER;
    e.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sids.back().data());
    ea.push_back(e);
  }
  PACL out = nullptr;
  const PACL from = revoked ? revoked : base;
  const DWORD rc = SetEntriesInAclW(static_cast<ULONG>(ea.size()), ea.empty() ? nullptr : ea.data(), from, &out);
  if (revoked) LocalFree(revoked);
  return rc == ERROR_SUCCESS ? out : nullptr;
}

std::vector<std::vector<BYTE>> sids_of(const std::vector<std::string>& names) {
  std::vector<std::vector<BYTE>> out;
  for (const auto& n : names) {
    auto s = sid_of(wide(n));
    if (!s.empty()) out.push_back(std::move(s));
  }
  return out;
}

bool set_folder(const std::wstring& path, const std::vector<std::string>& ours, const std::vector<member>& members) {
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                            &sd) != ERROR_SUCCESS) {
    return false;
  }
  PACL next = build_dacl(dacl, sids_of(ours), members, false);
  bool ok = false;
  if (next) {
    ok = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                               nullptr, next, nullptr) == ERROR_SUCCESS;
    LocalFree(next);
  }
  LocalFree(sd);
  return ok;
}

// A self-relative security descriptor for the share: its users alone.
std::vector<BYTE> share_sd(const std::vector<member>& members) {
  PACL acl = build_dacl(nullptr, {}, members, true);
  if (!acl) return {};
  SECURITY_DESCRIPTOR abs{};
  std::vector<BYTE> out;
  if (InitializeSecurityDescriptor(&abs, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&abs, TRUE, acl, FALSE)) {
    DWORD n = 0;
    MakeSelfRelativeSD(&abs, nullptr, &n);
    out.resize(n);
    if (n == 0 || !MakeSelfRelativeSD(&abs, out.data(), &n)) out.clear();
  }
  LocalFree(acl);
  return out;
}

struct share_row {
  std::wstring path, remark;
  bool found = false;
};
share_row share_info(const std::wstring& name) {
  share_row r;
  net_buffer b;
  if (NetShareGetInfo(nullptr, const_cast<LPWSTR>(name.c_str()), 2, &b.p) != NERR_Success) return r;
  const auto* i = reinterpret_cast<SHARE_INFO_2*>(b.p);
  r.found = true;
  if (i->shi2_path) r.path = i->shi2_path;
  if (i->shi2_remark) r.remark = i->shi2_remark;
  return r;
}

bool encrypt(const std::wstring& name) {
  net_buffer b;
  if (NetShareGetInfo(nullptr, const_cast<LPWSTR>(name.c_str()), 1005, &b.p) != NERR_Success) return false;
  SHARE_INFO_1005 next = *reinterpret_cast<SHARE_INFO_1005*>(b.p);
  next.shi1005_flags |= SHI1005_FLAGS_ENCRYPT_DATA;
  return NetShareSetInfo(nullptr, const_cast<LPWSTR>(name.c_str()), 1005, reinterpret_cast<LPBYTE>(&next), nullptr) ==
         NERR_Success;
}

class win_port final : public port {
 public:
  facts probe() override {
    facts f;
    f.supported = elevated();
    if (!f.supported) f.why_not = "the step needs an administrator, and was not given one";
    f.computer = narrow(computer_name(ComputerNameNetBIOS).c_str());
    const std::string dns = narrow(computer_name(ComputerNameDnsHostname).c_str());
    f.mdns_host = dns.empty() ? std::string() : dns + ".local";
    DWORD v = 0, n = sizeof v;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Parameters", L"SMB1",
                     RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS) {
      f.smb1_on = v != 0;
    }
    f.service_on = false;
    if (SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
      if (SC_HANDLE s = OpenServiceW(m, L"LanmanServer", SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS st{};
        f.service_on = QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING;
        CloseServiceHandle(s);
      }
      CloseServiceHandle(m);
    }
    return f;
  }

  owner account(const std::string& name) override {
    const std::wstring w = wide(name);
    net_buffer b;
    const NET_API_STATUS s = NetUserGetInfo(nullptr, w.c_str(), 1, &b.p);
    if (s == NERR_UserNotFound) return owner::none;
    if (s != NERR_Success) return owner::theirs;  // unknown: never treated as ours
    const auto* u = reinterpret_cast<USER_INFO_1*>(b.p);
    const bool marked = u->usri1_comment && wcscmp(u->usri1_comment, kComment) == 0;
    return marked && in_our_group(w) ? owner::ours : owner::theirs;
  }

  std::string make_account(const std::string& name, std::string_view password) override {
    if (!ensure_group()) return "could not make the group for MediaViewer's users";
    std::wstring w = wide(name), pw = wide(password);
    if (w.empty() || pw.empty()) return "the name or password could not be read";
    USER_INFO_1 u{};
    u.usri1_name = w.data();
    u.usri1_password = pw.data();
    u.usri1_priv = USER_PRIV_USER;
    u.usri1_comment = const_cast<LPWSTR>(kComment);
    u.usri1_flags = UF_SCRIPT | UF_DONT_EXPIRE_PASSWD | UF_PASSWD_CANT_CHANGE;
    DWORD bad = 0;
    const NET_API_STATUS s = NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&u), &bad);
    wipe(pw);
    if (s == NERR_PasswordTooShort) return "the password was not accepted (this computer's password rules)";
    if (s != NERR_Success) return "could not make the account";
    if (!group_member(kGroup, w, true)) {
      (void)NetUserDel(nullptr, w.c_str());
      return "could not mark the account as MediaViewer's";
    }
    (void)group_member(users_group().c_str(), w, false);
    const auto sid = sid_of(w);
    if (!sid.empty()) (void)logon_rights(sid, true);
    hide_from_sign_in(w, true);
    return {};
  }

  std::string set_password(const std::string& name, std::string_view password) override {
    if (account(name) != owner::ours) return "that account is not MediaViewer's";
    std::wstring w = wide(name), pw = wide(password);
    if (w.empty() || pw.empty()) return "the password could not be read";
    USER_INFO_1003 u{};
    u.usri1003_password = pw.data();
    const NET_API_STATUS s = NetUserSetInfo(nullptr, w.c_str(), 1003, reinterpret_cast<LPBYTE>(&u), nullptr);
    wipe(pw);
    if (s == NERR_PasswordTooShort) return "the password was not accepted (this computer's password rules)";
    return s == NERR_Success ? std::string() : "could not change the password";
  }

  std::string delete_account(const std::string& name) override {
    if (account(name) != owner::ours) return "that account is not MediaViewer's";
    const std::wstring w = wide(name);
    const auto sid = sid_of(w);
    if (!sid.empty()) (void)logon_rights(sid, false);
    hide_from_sign_in(w, false);
    return NetUserDel(nullptr, w.c_str()) == NERR_Success ? std::string() : "could not remove the account";
  }

  owner share(const std::string& name) override {
    const auto r = share_info(wide(name));
    if (!r.found) return owner::none;
    return r.remark == kRemark ? owner::ours : owner::theirs;
  }

  std::string put_share(const share_want& s, const std::vector<member>& members,
                        const std::vector<std::string>& ours) override {
    const std::wstring name = wide(s.name), path = wide(s.path);
    if (!set_folder(path, ours, members)) return "could not set who may open the folder";
    auto sd = share_sd(members);
    if (sd.empty()) return "could not set who may open the share";
    const auto there = share_info(name);
    if (there.found && _wcsicmp(there.path.c_str(), path.c_str()) != 0) {
      (void)set_folder(there.path, ours, {});
      if (NetShareDel(nullptr, const_cast<LPWSTR>(name.c_str()), 0) != NERR_Success) return "could not move the share";
    }
    if (there.found && _wcsicmp(there.path.c_str(), path.c_str()) == 0) {
      SHARE_INFO_1501 i{};
      i.shi1501_security_descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data());
      if (NetShareSetInfo(nullptr, const_cast<LPWSTR>(name.c_str()), 1501, reinterpret_cast<LPBYTE>(&i), nullptr) !=
          NERR_Success) {
        return "could not update the share";
      }
    } else {
      SHARE_INFO_502 i{};
      i.shi502_netname = const_cast<LPWSTR>(name.c_str());
      i.shi502_type = STYPE_DISKTREE;
      i.shi502_remark = const_cast<LPWSTR>(kRemark);
      i.shi502_max_uses = static_cast<DWORD>(-1);
      i.shi502_path = const_cast<LPWSTR>(path.c_str());
      i.shi502_security_descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data());
      if (NetShareAdd(nullptr, 502, reinterpret_cast<LPBYTE>(&i), nullptr) != NERR_Success) return "could not share the folder";
    }
    if (!encrypt(name)) return "the share could not be encrypted (SMB3); it was left as it was made";
    return {};
  }

  std::string remove_share(const std::string& name, const std::vector<std::string>& ours) override {
    const std::wstring w = wide(name);
    const auto there = share_info(w);
    if (!there.found || there.remark != kRemark) return {};
    if (!there.path.empty()) (void)set_folder(there.path, ours, {});
    return NetShareDel(nullptr, const_cast<LPWSTR>(w.c_str()), 0) == NERR_Success ? std::string() : "could not remove the share";
  }

  std::vector<std::string> our_shares() override {
    std::vector<std::string> out;
    net_buffer b;
    DWORD read = 0, total = 0, resume = 0;
    if (NetShareEnum(nullptr, 2, &b.p, MAX_PREFERRED_LENGTH, &read, &total, &resume) != NERR_Success) return out;
    const auto* i = reinterpret_cast<SHARE_INFO_2*>(b.p);
    for (DWORD k = 0; k < read; ++k) {
      if ((i[k].shi2_type & 0xff) == STYPE_DISKTREE && i[k].shi2_remark && wcscmp(i[k].shi2_remark, kRemark) == 0) {
        out.push_back(narrow(i[k].shi2_netname));
      }
    }
    return out;
  }

  std::vector<std::string> our_accounts() override {
    std::vector<std::string> out;
    net_buffer b;
    DWORD read = 0, total = 0;
    DWORD_PTR resume = 0;
    if (NetLocalGroupGetMembers(nullptr, kGroup, 3, &b.p, MAX_PREFERRED_LENGTH, &read, &total, &resume) != NERR_Success) {
      return out;
    }
    const auto* m = reinterpret_cast<LOCALGROUP_MEMBERS_INFO_3*>(b.p);
    for (DWORD i = 0; i < read; ++i) {
      if (!m[i].lgrmi3_domainandname) continue;
      std::wstring n = m[i].lgrmi3_domainandname;
      if (const auto at = n.find(L'\\'); at != std::wstring::npos) n = n.substr(at + 1);
      const std::string name = narrow(n.c_str());
      if (!name.empty() && account(name) == owner::ours) out.push_back(name);
    }
    return out;
  }

  std::string start_service() override {
    SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!m) return "could not turn on file sharing";
    SC_HANDLE s = OpenServiceW(m, L"LanmanServer", SERVICE_START);
    const bool ok = s && (StartServiceW(s, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING);
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(m);
    return ok ? std::string() : "could not turn on file sharing (the Server service)";
  }
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<win_port>(); }

void note_invoker() {}  // the step's token is the person's own, elevated: its user is who started it

std::string check_share_folder(const std::string& path) {
  // The system's drive is named "c:" for the pure check, whatever its letter.
  std::wstring sysdir(MAX_PATH, L'\0');
  const UINT n = GetSystemWindowsDirectoryW(sysdir.data(), MAX_PATH);
  const wchar_t sys = n > 0 ? static_cast<wchar_t>(towlower(sysdir[0])) : L'c';
  std::string as_c = path;
  if (!as_c.empty()) {
    const char d = static_cast<char>(tolower(static_cast<unsigned char>(as_c[0])));
    if (static_cast<wchar_t>(d) == sys) as_c[0] = 'c';
    else if (d == 'c') as_c[0] = '#';
  }
  if (std::string why = refuse_system_folder(as_c, true); !why.empty()) return why;
  const std::wstring w = wide(path);
  if (w.size() < 3 || w[1] != L':') return "that folder is not on a drive of this computer";
  HANDLE h = CreateFileW(w.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return "that folder is not there";
  std::string why;
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic) ||
      (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    why = "that folder is not there";
  } else if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    why = "that folder is a link; share the folder itself";
  }
  if (why.empty()) {
    // The folder as given is the folder itself: no link or junction anywhere above it.
    std::wstring finalp(32768, L'\0');
    const DWORD got = GetFinalPathNameByHandleW(h, finalp.data(), static_cast<DWORD>(finalp.size()), VOLUME_NAME_DOS);
    finalp.resize(got < finalp.size() ? got : 0);
    if (finalp.rfind(L"\\\\?\\", 0) == 0) finalp = finalp.substr(4);
    std::wstring given = w;
    for (auto& c : given) {
      if (c == L'/') c = L'\\';
    }
    while (given.size() > 3 && given.back() == L'\\') given.pop_back();
    while (finalp.size() > 3 && finalp.back() == L'\\') finalp.pop_back();
    if (finalp.empty() || _wcsicmp(finalp.c_str(), given.c_str()) != 0) why = "that folder is reached through a link; share the folder itself";
  }
  if (why.empty()) {
    const bool drive_root = w.size() <= 3;
    PSID owner_sid = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool mine = false;
    if (GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner_sid, nullptr, nullptr, nullptr, &sd) ==
        ERROR_SUCCESS) {
      HANDLE token = nullptr;
      if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len);
        if (len > 0 && GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
          mine = EqualSid(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, owner_sid) != FALSE;
        }
        CloseHandle(token);
      }
      LocalFree(sd);
    }
    // A drive of its own (not the system's: refused above) may belong to Administrators.
    if (!mine && !drive_root) why = "that folder is not yours (it belongs to another account on this PC)";
  }
  CloseHandle(h);
  return why;
}

}  // namespace mv::driftwire::host_smb
