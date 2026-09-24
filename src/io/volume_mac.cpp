// SPDX-License-Identifier: GPL-2.0-or-later
// macOS half of the volume port (io/volume.h): statfs + DiskArbitration.
//
// plan/18 names NSWorkspace mount notifications for arrival. DiskArbitration's
// description-changed callback on kDADiskDescriptionVolumePathKey is the same
// event from the C API that NSWorkspace sits on, and keeps io/ free of
// Objective-C. The Linux core test build answers volume_of() from statfs and
// has no volume list, watcher or eject.
#include "io/volume.h"

#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <dispatch/dispatch.h>
#else
#include <sys/vfs.h>
#endif

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

#include "io/file_port.h"

namespace mv::io {
namespace {

std::string fsid_key(const struct statfs& fs, std::uint64_t total) {
#if defined(__APPLE__)
  const auto a = static_cast<unsigned>(fs.f_fsid.val[0]);
  const auto b = static_cast<unsigned>(fs.f_fsid.val[1]);
#else
  const auto a = static_cast<unsigned>(fs.f_fsid.__val[0]);
  const auto b = static_cast<unsigned>(fs.f_fsid.__val[1]);
#endif
  char buf[64];
  std::snprintf(buf, sizeof buf, "fsid:%08x%08x:%llu", a, b, static_cast<unsigned long long>(total));
  return buf;
}

#if defined(__APPLE__)

std::string cf_string(CFStringRef s) {
  if (!s) return {};
  const CFIndex len = CFStringGetLength(s);
  const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(max), '\0');
  if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
  out.resize(std::char_traits<char>::length(out.c_str()));
  return out;
}

bool cf_bool(CFDictionaryRef d, CFStringRef key, bool fallback) {
  const void* v = CFDictionaryGetValue(d, key);
  if (!v || CFGetTypeID(v) != CFBooleanGetTypeID()) return fallback;
  return CFBooleanGetValue(static_cast<CFBooleanRef>(v));
}

struct cf_owned {
  CFTypeRef ref = nullptr;
  cf_owned(const cf_owned&) = delete;
  cf_owned& operator=(const cf_owned&) = delete;
  explicit cf_owned(CFTypeRef r) : ref(r) {}
  ~cf_owned() {
    if (ref) CFRelease(ref);
  }
};

CFURLRef url_for(const std::string& path) {
  return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
                                                 reinterpret_cast<const UInt8*>(path.c_str()),
                                                 static_cast<CFIndex>(path.size()), true);
}

// The DiskArbitration facts for a mounted path: UUID, label, removability,
// the whole disk's BSD name. Leaves `out` as statfs filled it where DA is silent.
void describe_with_da(const std::string& root, volume_info& out) {
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  if (!session) return;
  const cf_owned s(session);
  CFURLRef url = url_for(root);
  if (!url) return;
  const cf_owned u(url);
  DADiskRef disk = DADiskCreateFromVolumePath(kCFAllocatorDefault, session, url);
  if (!disk) return;
  const cf_owned d(disk);
  CFDictionaryRef desc = DADiskCopyDescription(disk);
  if (!desc) return;
  const cf_owned dd(desc);

  if (const void* uuid = CFDictionaryGetValue(desc, kDADiskDescriptionVolumeUUIDKey);
      uuid && CFGetTypeID(uuid) == CFUUIDGetTypeID()) {
    if (CFStringRef text = CFUUIDCreateString(kCFAllocatorDefault, static_cast<CFUUIDRef>(uuid))) {
      out.volume_id = "uuid:" + cf_string(text);
      CFRelease(text);
    }
  }
  if (const void* name = CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey);
      name && CFGetTypeID(name) == CFStringGetTypeID()) {
    out.label_utf8 = cf_string(static_cast<CFStringRef>(name));
  }
  const bool removable = cf_bool(desc, kDADiskDescriptionMediaRemovableKey, false);
  const bool ejectable = cf_bool(desc, kDADiskDescriptionMediaEjectableKey, false);
  const bool internal = cf_bool(desc, kDADiskDescriptionDeviceInternalKey, true);
  out.removable = removable || ejectable || !internal;
  if (DADiskRef whole = DADiskCopyWholeDisk(disk)) {
    if (const char* bsd = DADiskGetBSDName(whole)) out.device_key = std::string("bsd:") + bsd;
    CFRelease(whole);
  }
}

#endif  // __APPLE__

}  // namespace

result<volume_info> volume_of(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  struct statfs fs{};
  if (::statfs(path.c_str(), &fs) != 0) return err(status::io);
  volume_info out;
  out.total_bytes = static_cast<std::uint64_t>(fs.f_blocks) * static_cast<std::uint64_t>(fs.f_bsize);
  out.free_bytes = static_cast<std::uint64_t>(fs.f_bavail) * static_cast<std::uint64_t>(fs.f_bsize);
  const std::string fsid = fsid_key(fs, out.total_bytes);
#if defined(__APPLE__)
  out.root_utf8 = fs.f_mntonname;
  out.device_key = std::string("dev:") + fs.f_mntfromname;
  out.network = (fs.f_flags & MNT_LOCAL) == 0;
  out.read_only = (fs.f_flags & MNT_RDONLY) != 0;
  out.label_utf8 = std::string(file_name_of(out.root_utf8));
  describe_with_da(out.root_utf8, out);
#else
  // Linux tests: no mount-table walk; the path stands in for the root.
  out.root_utf8 = path;
  out.device_key = fsid;
#endif
  if (out.volume_id.empty()) out.volume_id = fsid;
  return out;
}

result<std::vector<volume_info>> list_volumes() {
  std::vector<volume_info> out;
#if defined(__APPLE__)
  struct statfs* mounts = nullptr;
  const int n = ::getmntinfo(&mounts, MNT_NOWAIT);
  if (n <= 0 || !mounts) return err(status::io);
  for (int i = 0; i < n; ++i) {
    const struct statfs& fs = mounts[i];
    if (fs.f_flags & MNT_DONTBROWSE) continue;  // system, VM and recovery volumes
    const std::string_view on(fs.f_mntonname);
    if (on != "/" && on.rfind("/Volumes/", 0) != 0) continue;
    if (auto v = volume_of(on)) out.push_back(std::move(*v));
  }
  std::stable_sort(out.begin(), out.end(), [](const volume_info& a, const volume_info& b) {
    return a.removable && !b.removable;
  });
#endif
  return out;
}

expected eject_volume(std::string_view root_utf8) {
#if defined(__APPLE__)
  if (root_utf8.empty()) return err(status::invalid_arg);
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  if (!session) return err(status::io);
  const cf_owned s(session);
  CFURLRef url = url_for(std::string(root_utf8));
  if (!url) return err(status::invalid_arg);
  const cf_owned u(url);
  DADiskRef disk = DADiskCreateFromVolumePath(kCFAllocatorDefault, session, url);
  if (!disk) return err(status::io);
  const cf_owned d(disk);
  DADiskRef whole = DADiskCopyWholeDisk(disk);
  if (!whole) return err(status::io);
  const cf_owned w(whole);
  dispatch_queue_t queue = dispatch_queue_create("mv.io.eject", DISPATCH_QUEUE_SERIAL);
  DASessionSetDispatchQueue(session, queue);

  struct wait_state {
    dispatch_semaphore_t done;
    bool ok = false;
  } st{dispatch_semaphore_create(0)};
  const auto on_done = [](DADiskRef, DADissenterRef dissenter, void* ctx) {
    auto* state = static_cast<wait_state*>(ctx);
    state->ok = dissenter == nullptr;
    dispatch_semaphore_signal(state->done);
  };
  // Unmount every volume on the card, then eject the medium. A busy volume
  // dissents: the card stays mounted and the caller reports it.
  DADiskUnmount(whole, kDADiskUnmountOptionWhole, on_done, &st);
  dispatch_semaphore_wait(st.done, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC));
  bool ok = st.ok;
  if (ok) {
    st.ok = false;
    DADiskEject(whole, kDADiskEjectOptionDefault, on_done, &st);
    dispatch_semaphore_wait(st.done, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC));
    ok = st.ok;
  }
  DASessionSetDispatchQueue(session, nullptr);
  dispatch_release(st.done);
  dispatch_release(queue);
  return ok ? expected{} : err(status::io);
#else
  (void)root_utf8;
  return err(status::unsupported_format);
#endif
}

// ---------------------------------------------------------------------------
struct volume_watcher::impl {
#if defined(__APPLE__)
  DASessionRef session = nullptr;
  dispatch_queue_t queue = nullptr;
  callback cb = nullptr;
  void* user = nullptr;
  std::map<std::string, std::string> mounted;  // BSD name -> mount path (watch queue only)

  static std::string bsd_of(DADiskRef disk) {
    const char* bsd = DADiskGetBSDName(disk);
    return bsd ? bsd : "";
  }

  static std::string path_of(DADiskRef disk) {
    CFDictionaryRef desc = DADiskCopyDescription(disk);
    if (!desc) return {};
    const cf_owned d(desc);
    const void* v = CFDictionaryGetValue(desc, kDADiskDescriptionVolumePathKey);
    if (!v || CFGetTypeID(v) != CFURLGetTypeID()) return {};
    UInt8 buf[PATH_MAX];
    if (!CFURLGetFileSystemRepresentation(static_cast<CFURLRef>(v), true, buf, sizeof buf)) return {};
    return reinterpret_cast<const char*>(buf);
  }

  static void changed(DADiskRef disk, CFArrayRef, void* ctx) {
    auto* self = static_cast<impl*>(ctx);
    const std::string bsd = bsd_of(disk);
    const std::string path = path_of(disk);
    if (!path.empty()) {
      self->mounted[bsd] = path;
      self->cb(self->user, volume_event::arrived, path.c_str());
    } else if (auto it = self->mounted.find(bsd); it != self->mounted.end()) {
      const std::string old = it->second;
      self->mounted.erase(it);
      self->cb(self->user, volume_event::removed, old.c_str());
    }
  }

  static void disappeared(DADiskRef disk, void* ctx) {
    auto* self = static_cast<impl*>(ctx);
    if (auto it = self->mounted.find(bsd_of(disk)); it != self->mounted.end()) {
      const std::string old = it->second;
      self->mounted.erase(it);
      self->cb(self->user, volume_event::removed, old.c_str());
    }
  }
#endif
};

volume_watcher::volume_watcher() = default;
volume_watcher::~volume_watcher() { stop(); }

expected volume_watcher::start(callback cb, void* user) {
  if (!cb) return err(status::invalid_arg);
  std::lock_guard lock(mutex_);
  if (impl_) return err(status::invalid_arg);
  auto p = std::make_unique<impl>();
#if defined(__APPLE__)
  p->cb = cb;
  p->user = user;
  p->session = DASessionCreate(kCFAllocatorDefault);
  if (!p->session) return err(status::io);
  p->queue = dispatch_queue_create("mv.io.volumes", DISPATCH_QUEUE_SERIAL);
  DARegisterDiskDescriptionChangedCallback(p->session, nullptr,
                                           kDADiskDescriptionWatchVolumePath, &impl::changed,
                                           p.get());
  DARegisterDiskDisappearedCallback(p->session, nullptr, &impl::disappeared, p.get());
  DASessionSetDispatchQueue(p->session, p->queue);
#else
  (void)user;
#endif
  impl_ = std::move(p);
  return {};
}

void volume_watcher::stop() noexcept {
  std::lock_guard lock(mutex_);
  if (!impl_) return;
#if defined(__APPLE__)
  DASessionSetDispatchQueue(impl_->session, nullptr);
  // Drain anything already queued before the callbacks' context goes away.
  dispatch_sync(impl_->queue, ^{});
  DAUnregisterCallback(impl_->session, reinterpret_cast<void*>(&impl::changed), impl_.get());
  DAUnregisterCallback(impl_->session, reinterpret_cast<void*>(&impl::disappeared), impl_.get());
  CFRelease(impl_->session);
  dispatch_release(impl_->queue);
#endif
  impl_.reset();
}

}  // namespace mv::io
