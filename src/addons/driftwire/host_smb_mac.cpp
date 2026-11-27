// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// macOS's file sharing for the LAN share (docs/plans/self-host-users-smb.md §5).
// Runs only inside the elevated step (root).
//
//   accounts  Open Directory's local node: a sharing-only user (no shell, no
//             home, hidden), its Comment and the group `mvshare` marking it as
//             MediaViewer's; the SMB (NT) hash turned on by `pwpolicy` (a name
//             only), the password set by ODRecordChangePassword: never an argument.
//   shares    `sharing`: record "MediaViewer <name>", SMB only, no guest,
//             encrypted (-E 1). Its folder gets an ACL entry per user,
//             inherited, and the folder's contents the inherited copy (as the
//             Finder's "Apply to enclosed items"); POSIX modes never change.
//   service   `launchctl enable` + `kickstart` of com.apple.smbd when off.
//
// Nothing here logs; the why strings name no path and no password.
#include "addons/driftwire/host_smb.h"

#include <CoreFoundation/CoreFoundation.h>
#include <OpenDirectory/OpenDirectory.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <grp.h>
#include <membership.h>
#include <pwd.h>
#include <spawn.h>
#include <dirent.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <climits>
#include <optional>

#include "core/json.h"

namespace mv::driftwire::host_smb {

namespace {

constexpr const char* kComment = "MediaViewer share user";
constexpr const char* kGroup = "mvshare";
constexpr const char* kRecordPrefix = "MediaViewer ";
constexpr const char* kSmbAccessGroup = "com.apple.access_smb";

// ---- small CF helpers ----
struct cf {
  CFTypeRef p = nullptr;
  cf() = default;
  explicit cf(CFTypeRef x) : p(x) {}
  ~cf() {
    if (p) CFRelease(p);
  }
  cf(const cf&) = delete;
  cf& operator=(const cf&) = delete;
  explicit operator bool() const { return p != nullptr; }
};

CFStringRef str(const std::string& s) {
  return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.data()),
                                 static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
}

std::string utf8(CFStringRef s) {
  if (!s) return {};
  const CFIndex len = CFStringGetLength(s);
  const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(max), '\0');
  if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
  out.resize(std::strlen(out.c_str()));
  return out;
}

// A program with its arguments (never a secret among them), its output when asked.
int run(const char* program, const std::vector<std::string>& args, std::string* output = nullptr) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(program));
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  int fds[2] = {-1, -1};
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (output) {
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
  }
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  if (!output) posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  // A clean environment: the step runs as the administrator, its tools need none of the person's.
  char path_env[] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
  char* env[] = {path_env, nullptr};
  const int rc = posix_spawn(&pid, program, &fa, nullptr, argv.data(), env);
  posix_spawn_file_actions_destroy(&fa);
  if (output) close(fds[1]);
  if (rc != 0) {
    if (output) close(fds[0]);
    return -1;
  }
  if (output) {
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) {
      if (output->size() < 1024 * 1024) output->append(buf, static_cast<std::size_t>(n));
    }
    close(fds[0]);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ---- Open Directory ----
class directory {
 public:
  directory() {
    CFErrorRef err = nullptr;
    node_ = ODNodeCreateWithNodeType(kCFAllocatorDefault, kODSessionDefault, kODNodeTypeLocalNodes, &err);
    if (err) CFRelease(err);
  }
  ~directory() {
    if (node_) CFRelease(node_);
  }
  directory(const directory&) = delete;
  directory& operator=(const directory&) = delete;
  [[nodiscard]] bool ok() const { return node_ != nullptr; }

  ODRecordRef record(ODRecordType type, const std::string& name) const {
    if (!node_) return nullptr;
    cf n(str(name));
    CFErrorRef err = nullptr;
    ODRecordRef r = ODNodeCopyRecord(node_, type, static_cast<CFStringRef>(n.p), nullptr, &err);
    if (err) CFRelease(err);
    return r;
  }
  static std::string first(ODRecordRef r, ODAttributeType attr) {
    CFErrorRef err = nullptr;
    CFArrayRef v = ODRecordCopyValues(r, attr, &err);
    if (err) CFRelease(err);
    if (!v) return {};
    std::string out;
    if (CFArrayGetCount(v) > 0) {
      const void* x = CFArrayGetValueAtIndex(v, 0);
      if (x && CFGetTypeID(x) == CFStringGetTypeID()) out = utf8(static_cast<CFStringRef>(x));
    }
    CFRelease(v);
    return out;
  }
  bool member(ODRecordRef group, ODRecordRef who) const {
    CFErrorRef err = nullptr;
    const bool yes = ODRecordContainsMember(group, who, &err);
    if (err) CFRelease(err);
    return yes;
  }
  ODRecordRef create(ODRecordType type, const std::string& name, CFDictionaryRef attrs) const {
    cf n(str(name));
    CFErrorRef err = nullptr;
    ODRecordRef r = ODNodeCreateRecord(node_, type, static_cast<CFStringRef>(n.p), attrs, &err);
    if (err) CFRelease(err);
    return r;
  }

 private:
  ODNodeRef node_ = nullptr;
};

CFArrayRef one(const std::string& v) {
  cf s(str(v));
  const void* vals[] = {s.p};
  return CFArrayCreate(kCFAllocatorDefault, vals, 1, &kCFTypeArrayCallBacks);
}

template <typename Taken>
std::uint32_t free_id(std::uint32_t from, Taken taken) {
  for (std::uint32_t id = from; id < from + 400; ++id) {
    if (!taken(id)) return id;
  }
  return 0;
}

// ---- ACLs ----
// The uid of a user record's name, or nullopt.
std::optional<uid_t> uid_of(const std::string& name) {
  const passwd* pw = getpwnam(name.c_str());
  if (!pw) return std::nullopt;
  return pw->pw_uid;
}

bool entry_is_one_of(acl_entry_t e, const std::vector<uid_t>& uids) {
  void* q = acl_get_qualifier(e);
  if (!q) return false;
  uid_t id = 0;
  int type = 0;
  const bool got = mbr_uuid_to_id(*static_cast<uuid_t*>(q), &id, &type) == 0;
  acl_free(q);
  return got && type == ID_TYPE_UID && std::find(uids.begin(), uids.end(), id) != uids.end();
}

void add_perms(acl_permset_t ps, role r) {
  for (acl_perm_t p : {ACL_LIST_DIRECTORY, ACL_SEARCH, ACL_READ_ATTRIBUTES, ACL_READ_EXTATTRIBUTES, ACL_READ_SECURITY}) {
    acl_add_perm(ps, p);
  }
  if (r == role::change || r == role::admin) {
    for (acl_perm_t p : {ACL_ADD_FILE, ACL_ADD_SUBDIRECTORY, ACL_DELETE, ACL_DELETE_CHILD, ACL_WRITE_ATTRIBUTES,
                         ACL_WRITE_EXTATTRIBUTES}) {
      acl_add_perm(ps, p);
    }
  }
  if (r == role::admin) {
    acl_add_perm(ps, ACL_WRITE_SECURITY);
    acl_add_perm(ps, ACL_CHANGE_OWNER);
  }
}

// One item's extended ACL, by its open descriptor (never a path: nothing can be
// swapped for a link between the look and the write): the entries of `strip`
// gone, `members` added (inherited when `inherited`). False when it could not be written.
bool set_acl(int fd, const std::vector<uid_t>& strip, const std::vector<member>& members, bool dir, bool inherited) {
  acl_t old = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
  acl_t next = acl_init(static_cast<int>(members.size()) + 4);
  if (!next) {
    if (old) acl_free(old);
    return false;
  }
  if (old) {
    acl_entry_t e = nullptr;
    for (int id = ACL_FIRST_ENTRY; acl_get_entry(old, id, &e) == 0; id = ACL_NEXT_ENTRY) {
      if (entry_is_one_of(e, strip)) continue;
      acl_entry_t copy = nullptr;
      if (acl_create_entry(&next, &copy) == 0) (void)acl_copy_entry(copy, e);
    }
    acl_free(old);
  }
  for (const auto& m : members) {
    uuid_t uu;
    const auto uid = uid_of(m.user);
    if (!uid || mbr_uid_to_uuid(*uid, uu) != 0) continue;
    acl_entry_t e = nullptr;
    if (acl_create_entry(&next, &e) != 0) continue;
    acl_set_tag_type(e, ACL_EXTENDED_ALLOW);
    acl_set_qualifier(e, &uu);
    acl_permset_t ps = nullptr;
    acl_get_permset(e, &ps);
    add_perms(ps, m.r);
    acl_set_permset(e, ps);
    acl_flagset_t fs = nullptr;
    acl_get_flagset_np(e, &fs);
    if (dir) {
      acl_add_flag_np(fs, ACL_ENTRY_FILE_INHERIT);
      acl_add_flag_np(fs, ACL_ENTRY_DIRECTORY_INHERIT);
    }
    if (inherited) acl_add_flag_np(fs, ACL_ENTRY_INHERITED);
    acl_set_flagset_np(e, fs);
  }
  const bool ok = acl_set_fd_np(fd, next, ACL_TYPE_EXTENDED) == 0;
  acl_free(next);
  return ok;
}

// What is inside an open folder, by descriptor: links are never followed, other
// volumes never entered, only folders and plain files touched.
void set_inside(int dirfd, dev_t dev, const std::vector<uid_t>& strip, const std::vector<member>& members, int depth) {
  if (depth > 256) return;
  const int listfd = dup(dirfd);
  if (listfd < 0) return;
  DIR* d = fdopendir(listfd);
  if (!d) {
    close(listfd);
    return;
  }
  for (dirent* e; (e = readdir(d)) != nullptr;) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
    struct stat st {};
    if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (st.st_dev != dev || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) continue;
    const int flags = O_NOFOLLOW | O_CLOEXEC | (S_ISDIR(st.st_mode) ? (O_RDONLY | O_DIRECTORY) : (O_RDONLY | O_NONBLOCK));
    const int fd = openat(dirfd, e->d_name, flags);
    if (fd < 0) continue;
    struct stat now {};
    // The same item as looked at: a swap in between is skipped.
    if (fstat(fd, &now) == 0 && now.st_ino == st.st_ino && now.st_dev == st.st_dev) {
      (void)set_acl(fd, strip, members, S_ISDIR(now.st_mode), true);
      if (S_ISDIR(now.st_mode)) set_inside(fd, dev, strip, members, depth + 1);
    }
    close(fd);
  }
  closedir(d);
}

// The folder and everything in it. The folder itself must not be a link.
bool set_tree(const std::string& root, const std::vector<uid_t>& strip, const std::vector<member>& members) {
  const int fd = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  bool ok = fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && set_acl(fd, strip, members, true, false);
  if (ok) set_inside(fd, st.st_dev, strip, members, 0);
  close(fd);
  return ok;
}

// ---- `sharing -l -f json` ----
struct share_point {
  std::string record, smb_name, path;
};
std::vector<share_point> share_points() {
  std::string out;
  if (run("/usr/sbin/sharing", {"-l", "-f", "json"}, &out) != 0) return {};
  const auto doc = json::parse(out, 4);
  std::vector<share_point> list;
  if (!doc || doc->k != json::kind::object) return list;
  for (const auto& [record, v] : doc->o) {
    const std::string* n = v.str("smb_name");
    const std::string* p = v.str("path");
    list.push_back({record, n ? *n : record, p ? *p : std::string()});
  }
  return list;
}

class mac_port final : public port {
 public:
  facts probe() override {
    facts f;
    f.supported = dir_.ok() && geteuid() == 0;
    if (!f.supported) f.why_not = "the step needs an administrator, and was not given one";
    cf scutil(SCDynamicStoreCopyLocalHostName(nullptr));
    const std::string local = scutil ? utf8(static_cast<CFStringRef>(scutil.p)) : std::string();
    f.computer = local;
    f.mdns_host = local.empty() ? std::string() : local + ".local";
    std::string disabled;
    (void)run("/bin/launchctl", {"print-disabled", "system"}, &disabled);
    const auto at = disabled.find("\"com.apple.smbd\" =>");
    f.service_on = true;
    if (at != std::string::npos) {
      const std::string rest = disabled.substr(at + 19, 12);
      f.service_on = rest.find("disabled") == std::string::npos && rest.find("true") == std::string::npos;
    }
    f.smb1_on = false;  // macOS's server has no SMB1 setting MediaViewer changes (plan §5)
    return f;
  }

  owner account(const std::string& name) override {
    ODRecordRef r = dir_.record(kODRecordTypeUsers, name);
    if (!r) return owner::none;
    cf held(r);
    return ours(r) ? owner::ours : owner::theirs;
  }

  std::string make_account(const std::string& name, std::string_view password) override {
    if (getpwnam(name.c_str())) return "an account of that name is already on this computer";
    ODRecordRef group = ensure_group();
    if (!group) return "could not make the group for MediaViewer's users";
    cf g(group);
    const std::uint32_t uid = free_id(601, [](std::uint32_t id) { return getpwuid(id) != nullptr; });
    if (uid == 0) return "no free user id";
    cf attrs(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks));
    auto* d = static_cast<CFMutableDictionaryRef>(const_cast<void*>(attrs.p));
    const auto put = [&](ODAttributeType k, const std::string& v) {
      cf a(one(v));
      CFDictionarySetValue(d, k, a.p);
    };
    put(kODAttributeTypeUniqueID, std::to_string(uid));
    put(kODAttributeTypePrimaryGroupID, std::to_string(group_id_));
    put(kODAttributeTypeUserShell, "/usr/bin/false");
    put(kODAttributeTypeNFSHomeDirectory, "/var/empty");
    put(kODAttributeTypeFullName, name);
    put(kODAttributeTypeComment, kComment);
    put(CFSTR("dsAttrTypeNative:IsHidden"), "1");
    ODRecordRef r = dir_.create(kODRecordTypeUsers, name, static_cast<CFDictionaryRef>(attrs.p));
    if (!r) return "could not make the account";
    cf rec(r);
    CFErrorRef err = nullptr;
    const bool joined = ODRecordAddMember(group, r, &err) && ODRecordSynchronize(group, &err);
    if (err) CFRelease(err);
    if (!joined) {
      (void)ODRecordDelete(r, nullptr);
      return "could not mark the account as MediaViewer's";
    }
    if (std::string why = password_now(r, name, password); !why.empty()) {
      (void)ODRecordDelete(r, nullptr);
      return why;
    }
    if (ODRecordRef smb = dir_.record(kODRecordTypeGroups, kSmbAccessGroup)) {
      cf s(smb);
      (void)ODRecordAddMember(smb, r, nullptr);
      (void)ODRecordSynchronize(smb, nullptr);
    }
    return {};
  }

  std::string set_password(const std::string& name, std::string_view password) override {
    ODRecordRef r = dir_.record(kODRecordTypeUsers, name);
    if (!r) return "the account is not there";
    cf held(r);
    if (!ours(r)) return "that account is not MediaViewer's";
    return password_now(r, name, password);
  }

  std::string delete_account(const std::string& name) override {
    ODRecordRef r = dir_.record(kODRecordTypeUsers, name);
    if (!r) return {};
    cf held(r);
    if (!ours(r)) return "that account is not MediaViewer's";
    CFErrorRef err = nullptr;
    const bool gone = ODRecordDelete(r, &err);
    if (err) CFRelease(err);
    return gone ? std::string() : "could not remove the account";
  }

  owner share(const std::string& name) override {
    for (const auto& s : share_points()) {
      if (s.smb_name != name) continue;
      return s.record == std::string(kRecordPrefix) + name ? owner::ours : owner::theirs;
    }
    return owner::none;
  }

  std::string put_share(const share_want& s, const std::vector<member>& members,
                        const std::vector<std::string>& ours_list) override {
    const std::string record = std::string(kRecordPrefix) + s.name;
    const auto points = share_points();
    const auto it = std::find_if(points.begin(), points.end(), [&](const share_point& p) { return p.record == record; });
    // The folder's permissions first: a share that opens is already right.
    if (!set_tree(s.path, uids(ours_list), members)) return "could not set who may open the folder";
    if (it == points.end()) {
      if (run("/usr/sbin/sharing", {"-a", s.path, "-n", record, "-S", s.name, "-s", "001", "-g", "000", "-E", "1"}) != 0) {
        return "could not share the folder";
      }
    } else if (it->path != s.path) {
      // The label now names another folder: share that one instead.
      (void)set_tree(it->path, uids(ours_list), {});
      if (run("/usr/sbin/sharing", {"-r", record}) != 0 ||
          run("/usr/sbin/sharing", {"-a", s.path, "-n", record, "-S", s.name, "-s", "001", "-g", "000", "-E", "1"}) != 0) {
        return "could not move the share to its folder";
      }
    } else if (run("/usr/sbin/sharing", {"-e", record, "-S", s.name, "-s", "001", "-g", "000", "-E", "1"}) != 0) {
      return "could not update the share";
    }
    return {};
  }

  std::string remove_share(const std::string& name, const std::vector<std::string>& ours_list) override {
    const std::string record = std::string(kRecordPrefix) + name;
    for (const auto& p : share_points()) {
      if (p.record != record) continue;
      if (!p.path.empty()) (void)set_tree(p.path, uids(ours_list), {});
      return run("/usr/sbin/sharing", {"-r", record}) == 0 ? std::string() : "could not remove the share";
    }
    return {};
  }

  std::vector<std::string> our_shares() override {
    std::vector<std::string> out;
    const std::string prefix = kRecordPrefix;
    for (const auto& p : share_points()) {
      if (p.record.rfind(prefix, 0) == 0) out.push_back(p.smb_name);
    }
    return out;
  }

  std::vector<std::string> our_accounts() override {
    std::vector<std::string> out;
    ODRecordRef g = dir_.record(kODRecordTypeGroups, kGroup);
    if (!g) return out;
    cf held(g);
    CFErrorRef err = nullptr;
    CFArrayRef names = ODRecordCopyValues(g, kODAttributeTypeGroupMembership, &err);
    if (err) CFRelease(err);
    if (!names) return out;
    for (CFIndex i = 0; i < CFArrayGetCount(names); ++i) {
      const void* x = CFArrayGetValueAtIndex(names, i);
      if (!x || CFGetTypeID(x) != CFStringGetTypeID()) continue;
      const std::string n = utf8(static_cast<CFStringRef>(x));
      if (account(n) == owner::ours) out.push_back(n);
    }
    CFRelease(names);
    return out;
  }

  std::string start_service() override {
    if (run("/bin/launchctl", {"enable", "system/com.apple.smbd"}) != 0) return "could not turn on file sharing";
    (void)run("/bin/launchctl", {"kickstart", "system/com.apple.smbd"});
    return {};
  }

 private:
  bool ours(ODRecordRef r) {
    if (directory::first(r, kODAttributeTypeComment) != kComment) return false;
    ODRecordRef g = dir_.record(kODRecordTypeGroups, kGroup);
    if (!g) return false;
    cf held(g);
    return dir_.member(g, r);
  }

  ODRecordRef ensure_group() {
    if (ODRecordRef g = dir_.record(kODRecordTypeGroups, kGroup)) {
      const std::string id = directory::first(g, kODAttributeTypePrimaryGroupID);
      group_id_ = static_cast<std::uint32_t>(std::strtoul(id.c_str(), nullptr, 10));
      return g;
    }
    const std::uint32_t gid = free_id(601, [](std::uint32_t id) { return getgrgid(id) != nullptr; });
    if (gid == 0) return nullptr;
    cf attrs(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                       &kCFTypeDictionaryValueCallBacks));
    auto* d = static_cast<CFMutableDictionaryRef>(const_cast<void*>(attrs.p));
    cf idv(one(std::to_string(gid)));
    cf name(one("MediaViewer share users"));
    CFDictionarySetValue(d, kODAttributeTypePrimaryGroupID, idv.p);
    CFDictionarySetValue(d, kODAttributeTypeFullName, name.p);
    group_id_ = gid;
    return dir_.create(kODRecordTypeGroups, kGroup, static_cast<CFDictionaryRef>(attrs.p));
  }

  std::string password_now(ODRecordRef r, const std::string& name, std::string_view password) {
    // The NT hash SMB needs, kept from the next password set on (a name only on the command line).
    (void)run("/usr/bin/pwpolicy", {"-u", name, "-sethashtypes", "SMB-NT", "on"});
    CFStringRef pw = CFStringCreateWithBytesNoCopy(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(password.data()),
                                                   static_cast<CFIndex>(password.size()), kCFStringEncodingUTF8, false,
                                                   kCFAllocatorNull);
    if (!pw) return "the password could not be read";
    CFErrorRef err = nullptr;
    const bool ok = ODRecordChangePassword(r, nullptr, pw, &err);
    CFRelease(pw);
    if (err) CFRelease(err);
    return ok ? std::string() : "the password was not accepted (this computer's password rules)";
  }

  static std::vector<uid_t> uids(const std::vector<std::string>& names) {
    std::vector<uid_t> out;
    for (const auto& n : names) {
      if (auto u = uid_of(n)) out.push_back(*u);
    }
    return out;
  }

  directory dir_;
  std::uint32_t group_id_ = 20;
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<mac_port>(); }

namespace {
uid_t g_invoker = static_cast<uid_t>(-1);
}

void note_invoker() {
  // sudo: SUDO_UID; the authorization dialog: the real uid is still the person's.
  g_invoker = getuid();
  if (g_invoker == 0) {
    if (const char* u = std::getenv("SUDO_UID")) {
      char* end = nullptr;
      const unsigned long v = std::strtoul(u, &end, 10);
      if (end && *end == '\0' && v > 0) g_invoker = static_cast<uid_t>(v);
    }
  }
  // The dialog's tool runs with only the effective uid root; its tools want both.
  if (geteuid() == 0 && getuid() != 0) (void)setuid(0);
}

std::string check_share_folder(const std::string& path) {
  if (std::string why = refuse_system_folder(path, false); !why.empty()) return why;
  char real[PATH_MAX];
  if (!realpath(path.c_str(), real) || path != real) return "that folder is reached through a link; share the folder itself";
  struct stat st {}, parent {}, root {};
  if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return "that folder is not there";
  if (refuse_system_folder(real, false).size() != 0) return refuse_system_folder(real, false);
  if (g_invoker == static_cast<uid_t>(-1) || g_invoker == 0) return "the step could not tell who started it";
  if (st.st_uid == g_invoker) return {};
  // A drive of its own (a mount point, not the system's volume) whose files the Mac ignores owners of.
  const std::string up = std::filesystem::path(path).parent_path().string();
  if (stat(up.c_str(), &parent) == 0 && stat("/", &root) == 0 && st.st_dev != parent.st_dev && st.st_dev != root.st_dev) {
    return {};
  }
  return "that folder is not yours (it belongs to another account on this Mac)";
}

}  // namespace mv::driftwire::host_smb
