// SPDX-License-Identifier: GPL-2.0-or-later
// Minidump privacy scrub — plan/13 Part 2, rule 6.
//
// Crashpad's Windows handler cannot be told to leave things out, and a stock
// dump of this app contains, besides stacks and contexts:
//   - the PEB and RTL_USER_PROCESS_PARAMETERS: the full command line (a photo
//     path passed on argv), the environment (USERPROFILE, USERNAME), the
//     current directory;
//   - every TEB (StaticUnicodeString: the last path a Win32 API converted);
//   - 512 bytes around every register of the crashing thread — a pixel row
//     when the fault is mid-memcpy, a path when rcx is a c_str();
//   - the module list, with full DLL paths under C:\Users\<name>\.
//
// This pass rewrites a finished dump in place, same length, before any send
// is even offered:
//   1. Memory: every captured byte whose virtual address is NOT inside a
//      thread stack is zeroed. Stacks, thread contexts, the exception record
//      and the module list survive; that is what symbolicated triage needs.
//      Heap is never captured (indirect-memory gathering is off), so decoded
//      image buffers are excluded structurally rather than by allocation tag.
//   2. Text, over the rest of the file (stacks included) in 8-bit and UTF-16LE
//      at both byte parities:
//        - a drive/UNC path (X:\..., \\server\..., \\?\...) is masked after
//          its root, unless it names a module (.dll/.exe/.pdb/...), in which
//          case only the user-profile component and identity tokens are;
//        - a bare filename ending in a camera-dump extension is masked up to
//          the extension;
//        - identity tokens (username, computer name) are masked wherever they
//          stand as a whole token.
//      Masked code units become '_', so structure offsets never move.
//
// Residual risk (documented in plan/13): a bare folder name with no drive
// root and no media extension on a live stack frame; a filename stem without
// its extension; non-drive-rooted relative paths; pixel bytes a decoder keeps
// in a stack-local array. The verify (tools/minidump-scan.ps1) checks for all
// of these on a real crash.
//
// Portable: no Windows headers, so the mac host (Milestone F) reuses it.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mv::shell {

struct scrub_options {
  // UTF-8. Username, computer name. Tokens shorter than 3 bytes are ignored
  // (masking every "ab" in a dump would destroy it and protect nothing).
  std::vector<std::string> identities;
};

struct scrub_report {
  bool valid = false;           // false: not a minidump; nothing was written
  bool already_scrubbed = false;  // marker was set before this pass
  std::uint32_t memory_ranges = 0;
  std::uint64_t zeroed_bytes = 0;
  std::uint32_t paths_masked = 0;
  std::uint32_t names_masked = 0;
  std::uint32_t identities_masked = 0;
};

// Header CheckSum field value written by scrub_minidump (unused by every
// minidump consumer we know of). Lets the app skip a dump it already did.
inline constexpr std::uint32_t kScrubMarker = 0x4353564D;  // 'MVSC'

[[nodiscard]] bool minidump_is_scrubbed(std::span<const std::uint8_t> dump) noexcept;

// In place. Idempotent. Never changes the size.
scrub_report scrub_minidump(std::span<std::uint8_t> dump, const scrub_options& options);

// Text-only mask of a managed report / log line (UTF-8), same rules as step 2.
std::string scrub_text(std::string text, const scrub_options& options);

}  // namespace mv::shell
