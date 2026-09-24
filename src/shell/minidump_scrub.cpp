// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/minidump_scrub.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace mv::shell {
namespace {

// --- minidump layout (MINIDUMP_* in minidumpapiset.h; little-endian) ------
constexpr std::uint32_t kSignature = 0x504D444D;  // 'MDMP'
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kDirEntryBytes = 12;
constexpr std::size_t kThreadBytes = 48;
constexpr std::size_t kMemDescBytes = 16;
constexpr std::size_t kMem64DescBytes = 16;
constexpr std::uint32_t kThreadListStream = 3;
constexpr std::uint32_t kMemoryListStream = 5;
constexpr std::uint32_t kExceptionStream = 6;
constexpr std::uint32_t kMemory64ListStream = 9;
constexpr std::size_t kHeaderChecksumOffset = 16;

std::uint32_t rd32(std::span<const std::uint8_t> d, std::size_t o) noexcept {
  if (o + 4 > d.size()) return 0;
  return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) |
         (static_cast<std::uint32_t>(d[o + 2]) << 16) | (static_cast<std::uint32_t>(d[o + 3]) << 24);
}
std::uint64_t rd64(std::span<const std::uint8_t> d, std::size_t o) noexcept {
  return static_cast<std::uint64_t>(rd32(d, o)) | (static_cast<std::uint64_t>(rd32(d, o + 4)) << 32);
}

struct interval {
  std::uint64_t begin;
  std::uint64_t end;  // exclusive
};

class byte_guard {
 public:
  explicit byte_guard(std::size_t n) : protected_(n, 0) {}
  void protect(std::uint64_t begin, std::uint64_t size) {
    const std::uint64_t n = protected_.size();
    if (begin >= n) return;
    const std::uint64_t end = std::min<std::uint64_t>(n, begin + size);
    std::fill(protected_.begin() + static_cast<std::ptrdiff_t>(begin),
              protected_.begin() + static_cast<std::ptrdiff_t>(end), std::uint8_t{1});
  }
  [[nodiscard]] bool writable(std::size_t i) const noexcept {
    return i < protected_.size() && protected_[i] == 0;
  }

 private:
  std::vector<std::uint8_t> protected_;
};

// --- text rules --------------------------------------------------------------

constexpr std::string_view kModuleExt[] = {"dll", "exe", "sys", "drv", "ocx", "cpl",
                                           "winmd", "pdb", "mui", "node", "efi",
                                           // PR 11, macOS: shared libraries, plug-ins, symbols
                                           "dylib", "so", "bundle", "appex", "dsym"};
// PR 11, macOS: the roots under which a POSIX absolute path can name a user's
// files. /System/Library, /usr/lib and /Applications hold none and are left
// alone (a module list full of them is what symbolication needs).
constexpr std::string_view kPosixRoots[] = {"Users", "Volumes", "private", "var", "tmp",
                                            "home", "Network", "mnt", "media"};
// D5 camera-dump set, RAW variants, companions, and the video containers.
constexpr std::string_view kMediaExt[] = {
    "jpg", "jpeg", "jpe", "jfif", "png", "apng", "bmp", "dib", "gif", "tif", "tiff", "webp",
    "heic", "heif", "hif", "avif", "ico", "cur", "dng", "cr2", "cr3", "crw", "nef", "nrw",
    "arw", "srf", "sr2", "raf", "orf", "rw2", "pef", "srw", "x3f", "erf", "kdc", "mos",
    "mrw", "3fr", "iiq", "rwl", "mp4", "m4v", "mov", "mkv", "webm", "avi", "ts", "mts",
    "m2ts", "mpg", "mpeg", "xmp", "aae", "thm", "lrv"};

constexpr std::uint32_t kMask = '_';

std::uint32_t lower(std::uint32_t u) noexcept { return (u >= 'A' && u <= 'Z') ? u + 32 : u; }
bool is_alpha(std::uint32_t u) noexcept { return (u | 32) >= 'a' && (u | 32) <= 'z'; }
bool is_alnum(std::uint32_t u) noexcept { return is_alpha(u) || (u >= '0' && u <= '9'); }
bool is_sep(std::uint32_t u) noexcept { return u == '\\' || u == '/'; }

// Characters that may continue a path (spaces included: real folders have them).
bool path_unit(std::uint32_t u) noexcept {
  if (u < 0x20 || u == 0x7F) return false;
  switch (u) {
    case '"': case '<': case '>': case '|': case '*': case '?': return false;
    default: return true;
  }
}
bool filename_unit(std::uint32_t u) noexcept { return path_unit(u) && !is_sep(u) && u != ':'; }

// A run of code units of one width over a byte span.
class units {
 public:
  units(std::span<std::uint8_t> bytes, std::size_t base, std::size_t width, const byte_guard* guard)
      : bytes_(bytes), base_(base), width_(width), guard_(guard) {
    count_ = bytes.size() > base ? (bytes.size() - base) / width : 0;
  }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::uint32_t at(std::size_t k) const noexcept {
    const std::size_t o = base_ + k * width_;
    if (width_ == 1) return bytes_[o];
    return static_cast<std::uint32_t>(bytes_[o]) | (static_cast<std::uint32_t>(bytes_[o + 1]) << 8);
  }
  void mask(std::size_t k) noexcept {
    const std::size_t o = base_ + k * width_;
    for (std::size_t j = 0; j < width_; ++j) {
      if (guard_ && !guard_->writable(o + j)) return;
    }
    bytes_[o] = static_cast<std::uint8_t>(kMask);
    if (width_ == 2) bytes_[o + 1] = 0;
  }
  [[nodiscard]] bool equals_ci(std::size_t k, std::string_view ascii) const noexcept {
    if (k + ascii.size() > count_) return false;
    for (std::size_t i = 0; i < ascii.size(); ++i) {
      if (lower(at(k + i)) != lower(static_cast<std::uint8_t>(ascii[i]))) return false;
    }
    return true;
  }

 private:
  std::span<std::uint8_t> bytes_;
  std::size_t base_;
  std::size_t width_;
  std::size_t count_ = 0;
  const byte_guard* guard_;
};

bool ext_in(const units& u, std::size_t begin, std::size_t end,
            std::span<const std::string_view> list) noexcept {
  const std::size_t n = end - begin;
  for (const auto ext : list) {
    if (ext.size() == n && u.equals_ci(begin, ext)) return true;
  }
  return false;
}

struct identity_units {
  std::vector<std::uint32_t> narrow;  // UTF-8 bytes
  std::vector<std::uint32_t> wide;    // UTF-16 code units
};

std::vector<identity_units> prepare_identities(const scrub_options& options) {
  std::vector<identity_units> out;
  for (const auto& id : options.identities) {
    if (id.size() < 3) continue;
    identity_units iu;
    for (unsigned char c : id) iu.narrow.push_back(c);
    // UTF-8 → UTF-16 (BMP + surrogates), minimal.
    for (std::size_t i = 0; i < id.size();) {
      const unsigned char c = static_cast<unsigned char>(id[i]);
      std::uint32_t cp = 0;
      std::size_t len = 1;
      if (c < 0x80) cp = c;
      else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
      else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
      else { cp = c & 0x07; len = 4; }
      for (std::size_t j = 1; j < len && i + j < id.size(); ++j) {
        cp = (cp << 6) | (static_cast<unsigned char>(id[i + j]) & 0x3F);
      }
      i += len;
      if (cp >= 0x10000) {
        cp -= 0x10000;
        iu.wide.push_back(0xD800 + (cp >> 10));
        iu.wide.push_back(0xDC00 + (cp & 0x3FF));
      } else {
        iu.wide.push_back(cp);
      }
    }
    out.push_back(std::move(iu));
  }
  return out;
}

bool identity_at(const units& u, std::size_t k, const std::vector<std::uint32_t>& id) noexcept {
  if (id.empty() || k + id.size() > u.size()) return false;
  for (std::size_t i = 0; i < id.size(); ++i) {
    if (lower(u.at(k + i)) != lower(id[i])) return false;
  }
  return true;
}

bool component_is_identity(const units& u, std::size_t b, std::size_t e,
                           const std::vector<identity_units>& ids, bool wide) noexcept {
  for (const auto& id : ids) {
    const auto& v = wide ? id.wide : id.narrow;
    if (v.size() == e - b && identity_at(u, b, v)) return true;
  }
  return false;
}

// A rooted path starting at k: `end` is past its last unit, or k when there
// is none. `keep` is where masking starts for a data path (the drive or share
// prefix, or a POSIX path's first component, "/Users/"); `walk` is where a
// module path's components start ("C:\", "/").
struct rooted_path {
  std::size_t end = 0;
  std::size_t keep = 0;
  std::size_t walk = 0;
  bool posix = false;
};

rooted_path path_at(const units& u, std::size_t k) noexcept {
  const std::size_t n = u.size();
  rooted_path p{k, k, k, false};
  if (k > 0 && is_alnum(u.at(k - 1))) return p;
  std::size_t root = 0;
  if (k + 3 < n && is_alpha(u.at(k)) && u.at(k + 1) == ':' && is_sep(u.at(k + 2))) {
    root = 3;
  } else if (k + 3 < n && u.at(k) == '\\' && u.at(k + 1) == '\\' &&
             (filename_unit(u.at(k + 2)) || u.at(k + 2) == '?' || u.at(k + 2) == '.')) {
    root = 2;
  } else if (u.at(k) == '/' && (k == 0 || (!path_unit(u.at(k - 1)) || u.at(k - 1) == ' ' ||
                                           u.at(k - 1) == '=' || u.at(k - 1) == '(' ||
                                           u.at(k - 1) == '\'' || u.at(k - 1) == ','))) {
    // PR 11, macOS: "/Users/<name>/...", "/Volumes/<card>/...", "/private/var/...".
    // Not "//" (a URL's authority) and not a relative "a/b".
    for (const auto r : kPosixRoots) {
      if (k + 2 + r.size() < n && u.equals_ci(k + 1, r) && u.at(k + 1 + r.size()) == '/' &&
          path_unit(u.at(k + 2 + r.size())) && !is_sep(u.at(k + 2 + r.size()))) {
        root = 2 + r.size();
        p.posix = true;
        break;
      }
    }
    if (root == 0) return p;
  } else {
    return p;
  }
  std::size_t e = k + root;
  if (!p.posix && root == 2 && e < n && u.at(e) == '?') ++e;  // \\?\ prefix
  while (e < n && path_unit(u.at(e))) ++e;
  if (e <= k + root) return p;
  p.end = e;
  p.keep = k + root;
  p.walk = p.posix ? k + 1 : k + root;
  return p;
}

// A macOS module has no extension of its own inside a bundle:
// "…/MediaViewer.app/Contents/MacOS/MediaViewer", "…/Sparkle.framework/Versions/B/Sparkle".
bool posix_bundle_module(const units& u, std::size_t b, std::size_t e) noexcept {
  for (std::size_t i = b; i + 1 < e; ++i) {
    if (u.at(i) != '.') continue;
    if (u.equals_ci(i, ".app/contents/") || u.equals_ci(i, ".framework/") ||
        u.equals_ci(i, ".appex/contents/") || u.equals_ci(i, ".bundle/contents/")) {
      return true;
    }
  }
  return false;
}

void scrub_units(units& u, const std::vector<identity_units>& ids, bool wide,
                 scrub_report& report) {
  const std::size_t n = u.size();
  std::size_t k = 0;
  while (k < n) {
    const std::uint32_t c = u.at(k);

    // 1. Rooted paths (Windows drive / UNC; PR 11: POSIX under a user root).
    if (c == '\\' || c == ':' || c == '/' || is_alpha(c)) {
      const rooted_path rp = path_at(u, k);
      std::size_t e = rp.end;
      if (e > k) {
        std::size_t last = k;
        for (std::size_t i = k; i < e; ++i) {
          if (is_sep(u.at(i))) last = i + 1;
        }
        // Spaces are legal in paths, so a run can swallow the prose after it
        // ("...IMG_1.HEIC (0x80070002)"). End at the first NAME.ext + space.
        for (std::size_t i = last; i < e; ++i) {
          if (u.at(i) != '.') continue;
          std::size_t x = i + 1;
          while (x < e && x - i <= 6 && is_alnum(u.at(x))) ++x;
          if (x > i + 1 && x < e && u.at(x) == ' ') { e = x; break; }
        }
        std::size_t dot = e;
        for (std::size_t i = e; i > last; --i) {
          if (u.at(i - 1) == '.') { dot = i - 1; break; }
        }
        const bool module = (dot < e && ext_in(u, dot + 1, e, kModuleExt)) ||
                            (rp.posix && posix_bundle_module(u, k, e));
        const std::size_t root_end = module ? rp.walk : rp.keep;
        if (module) {
          // Keep the layout for symbolication; drop the profile name and any
          // identity token standing as a component. ("Users" is a component
          // on both systems: C:\Users\name, /Users/name.)
          std::size_t b = root_end;
          bool after_users = false;
          for (std::size_t i = root_end; i <= e; ++i) {
            if (i == e || is_sep(u.at(i))) {
              const bool users = (i - b == 5) && u.equals_ci(b, "users");
              if ((after_users || component_is_identity(u, b, i, ids, wide)) && i > b) {
                for (std::size_t j = b; j < i; ++j) u.mask(j);
                ++report.identities_masked;
              }
              after_users = users;
              b = i + 1;
            }
          }
        } else {
          for (std::size_t j = root_end; j < e; ++j) {
            if (!is_sep(u.at(j))) u.mask(j);
          }
          ++report.paths_masked;
        }
        k = e;
        continue;
      }
    }

    // 2. Bare media filenames: NAME.ext followed by a terminator.
    if (c == '.' && k > 0 && filename_unit(u.at(k - 1)) && u.at(k - 1) != ' ') {
      std::size_t e = k + 1;
      while (e < n && e - k <= 5 && is_alnum(u.at(e))) ++e;
      const bool terminated = e == n || !is_alnum(u.at(e));
      if (terminated && e > k + 1 && ext_in(u, k + 1, e, kMediaExt)) {
        std::size_t b = k;
        while (b > 0 && k - b < 255 && filename_unit(u.at(b - 1))) --b;
        for (std::size_t j = b; j < k; ++j) u.mask(j);
        ++report.names_masked;
        k = e;
        continue;
      }
    }

    // 3. Identity tokens.
    if (k == 0 || !is_alnum(u.at(k - 1))) {
      bool hit = false;
      for (const auto& id : ids) {
        const auto& v = wide ? id.wide : id.narrow;
        if (identity_at(u, k, v) && (k + v.size() == n || !is_alnum(u.at(k + v.size())))) {
          for (std::size_t j = 0; j < v.size(); ++j) u.mask(k + j);
          ++report.identities_masked;
          k += v.size();
          hit = true;
          break;
        }
      }
      if (hit) continue;
    }
    ++k;
  }
}

void scrub_all_encodings(std::span<std::uint8_t> bytes, const byte_guard* guard,
                         const std::vector<identity_units>& ids, scrub_report& report) {
  units narrow(bytes, 0, 1, guard);
  scrub_units(narrow, ids, false, report);
  for (std::size_t parity = 0; parity < 2; ++parity) {
    units wide(bytes, parity, 2, guard);
    scrub_units(wide, ids, true, report);
  }
}

}  // namespace

bool minidump_is_scrubbed(std::span<const std::uint8_t> dump) noexcept {
  return dump.size() >= kHeaderBytes && rd32(dump, 0) == kSignature &&
         rd32(dump, kHeaderChecksumOffset) == kScrubMarker;
}

scrub_report scrub_minidump(std::span<std::uint8_t> dump, const scrub_options& options) {
  scrub_report report;
  const std::span<const std::uint8_t> d = dump;
  if (d.size() < kHeaderBytes || rd32(d, 0) != kSignature) return report;
  const std::uint32_t streams = rd32(d, 8);
  const std::uint32_t dir = rd32(d, 12);
  if (static_cast<std::uint64_t>(dir) + static_cast<std::uint64_t>(streams) * kDirEntryBytes >
      d.size()) {
    return report;
  }
  report.valid = true;
  report.already_scrubbed = rd32(d, kHeaderChecksumOffset) == kScrubMarker;

  byte_guard guard(d.size());
  guard.protect(0, kHeaderBytes);
  guard.protect(dir, static_cast<std::uint64_t>(streams) * kDirEntryBytes);

  std::vector<interval> stacks;
  struct mem_range {
    std::uint64_t va;
    std::uint64_t size;
    std::uint64_t rva;
  };
  std::vector<mem_range> ranges;

  for (std::uint32_t s = 0; s < streams; ++s) {
    const std::size_t e = dir + s * kDirEntryBytes;
    const std::uint32_t type = rd32(d, e);
    const std::uint32_t size = rd32(d, e + 4);
    const std::uint32_t rva = rd32(d, e + 8);
    if (static_cast<std::uint64_t>(rva) + size > d.size()) continue;

    if (type == kThreadListStream) {
      guard.protect(rva, size);
      const std::uint32_t count = rd32(d, rva);
      for (std::uint32_t t = 0; t < count; ++t) {
        const std::size_t th = rva + 4 + t * kThreadBytes;
        if (th + kThreadBytes > static_cast<std::size_t>(rva) + size) break;
        const std::uint64_t stack_va = rd64(d, th + 24);
        const std::uint32_t stack_size = rd32(d, th + 32);
        stacks.push_back({stack_va, stack_va + stack_size});
        guard.protect(rd32(d, th + 44), rd32(d, th + 40));  // thread CONTEXT
      }
    } else if (type == kExceptionStream) {
      guard.protect(rva, size);
      // MINIDUMP_EXCEPTION_STREAM: ThreadId, align, EXCEPTION (152), CONTEXT loc.
      guard.protect(rd32(d, rva + 164), rd32(d, rva + 160));
    } else if (type == kMemoryListStream) {
      guard.protect(rva, size);
      const std::uint32_t count = rd32(d, rva);
      for (std::uint32_t m = 0; m < count; ++m) {
        const std::size_t md = rva + 4 + m * kMemDescBytes;
        if (md + kMemDescBytes > static_cast<std::size_t>(rva) + size) break;
        ranges.push_back({rd64(d, md), rd32(d, md + 8), rd32(d, md + 12)});
      }
    } else if (type == kMemory64ListStream) {
      guard.protect(rva, size);
      const std::uint64_t count = rd64(d, rva);
      std::uint64_t data = rd64(d, rva + 8);
      for (std::uint64_t m = 0; m < count; ++m) {
        const std::size_t md = rva + 16 + static_cast<std::size_t>(m) * kMem64DescBytes;
        if (md + kMem64DescBytes > static_cast<std::size_t>(rva) + size) break;
        const std::uint64_t va = rd64(d, md);
        const std::uint64_t sz = rd64(d, md + 8);
        ranges.push_back({va, sz, data});
        data += sz;
      }
    }
  }

  // 1. Zero every captured byte outside a thread stack.
  std::sort(stacks.begin(), stacks.end(),
            [](const interval& a, const interval& b) { return a.begin < b.begin; });
  for (const auto& r : ranges) {
    if (r.rva >= d.size()) continue;
    const std::uint64_t size = std::min<std::uint64_t>(r.size, d.size() - r.rva);
    ++report.memory_ranges;
    for (std::uint64_t off = 0; off < size;) {
      const std::uint64_t va = r.va + off;
      // Inside a stack? Skip to its end.
      const interval* in = nullptr;
      std::uint64_t next_stack = UINT64_MAX;
      for (const auto& st : stacks) {
        if (va >= st.begin && va < st.end) { in = &st; break; }
        if (st.begin > va) { next_stack = std::min(next_stack, st.begin); }
      }
      if (in) {
        off += in->end - va;
        continue;
      }
      const std::uint64_t run_end = std::min(size, next_stack == UINT64_MAX
                                                       ? size
                                                       : off + (next_stack - va));
      for (std::uint64_t i = off; i < run_end; ++i) {
        const std::size_t at = static_cast<std::size_t>(r.rva + i);
        if (dump[at] != 0 && guard.writable(at)) {
          dump[at] = 0;
          ++report.zeroed_bytes;
        }
      }
      off = run_end;
    }
  }

  // 2. Text everywhere else.
  const auto ids = prepare_identities(options);
  scrub_all_encodings(dump, &guard, ids, report);

  // Marker last, directly (the header is guarded against the text pass).
  dump[kHeaderChecksumOffset] = static_cast<std::uint8_t>(kScrubMarker);
  dump[kHeaderChecksumOffset + 1] = static_cast<std::uint8_t>(kScrubMarker >> 8);
  dump[kHeaderChecksumOffset + 2] = static_cast<std::uint8_t>(kScrubMarker >> 16);
  dump[kHeaderChecksumOffset + 3] = static_cast<std::uint8_t>(kScrubMarker >> 24);
  return report;
}

std::string scrub_text(std::string text, const scrub_options& options) {
  scrub_report report;
  const auto ids = prepare_identities(options);
  std::span<std::uint8_t> bytes(reinterpret_cast<std::uint8_t*>(text.data()), text.size());
  units narrow(bytes, 0, 1, nullptr);
  scrub_units(narrow, ids, false, report);
  return text;
}

}  // namespace mv::shell
