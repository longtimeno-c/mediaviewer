# Audit: metadata, photo editing, tests and CI

Read-only audit, 2026-11-25, branch `claude/project-audit-performance-180749`. All paths are relative to the repo root.
"Confirmed" means I read the code path end to end. "Suspected" means the code is confirmed but the runtime effect depends on OS or filesystem behaviour I did not run.
I checked the 78 open issues; none of the findings below is filed. Overlaps are cited where they exist.

Context: there are two paths that write into a user's original by design. Both are documented as accepted exceptions to "never modify the original":
- in-place JPEG metadata edits (docs/design/06-metadata.md:358-378);
- the viewer's lossless rotate (src/io/replace.h:25-31).

Most of the high-severity items are gaps in those two paths.

---

## 1. Issues by severity

### HIGH

**H1. Lossless rotate in the viewer deletes Motion Photo video, Ultra HDR gain maps and MPF images from the user's original JPEG.** (code confirmed; effect on real files suspected)
- `edit_session::wants_write()` gates the write. It only requires `item_.jpeg`, which is decided by file extension (src/shell/edit_session.cpp:138-145; src/shell/main.cpp:2595; src/shell/main_mac.mm:9841). Nothing checks for MPF or bytes after EOI.
- `run_rotation_write` then calls `rotate_in_viewer` followed by `io::replace_atomic` over the original (src/shell/edit_session.cpp:690-697).
- When the size is MCU-aligned, `rotate_in_viewer` takes `transform()` (src/edit/lossless_jpeg.cpp:394). That rebuilds the file from DCT coefficients plus saved APPn/COM markers only (lossless_jpeg.cpp:199-202, 324-327). Typical camera sizes like 4032x3024 or 4000x3000 are aligned.
  - Anything after EOI is dropped. That includes the embedded MP4 of Pixel/Samsung Motion Photos and the secondary gain-map or depth JPEGs of iPhone/Android Ultra HDR.
  - The MPF APP2 is copied verbatim, so its offsets now point at nothing.
- The metadata writer already refuses in-file writes for exactly these files (`!l.scan.mpf && !l.scan.trailer`, src/meta/write.cpp:1181). The rotate path does not have the same guard.
- Fix: in `rotate_in_viewer`, or before `take_pending_write`, refuse `transform()` when the JPEG has an MPF segment or a trailer after EOI. Fall back to the tag-only `rewrite_orientation` (lossless_jpeg.cpp:348), which copies the whole file and preserves the trailer.
- Tests: tests/test_edit.cpp:529-734 and tests/test_edit_session.cpp:256 have no motion-photo or MPF case. Add one: rotate a JPEG with a trailer and assert the trailer bytes survive.

**H2. On macOS, the first sidecar write (and revert) fails on exFAT/FAT camera cards and probably on some SMB shares.** (code confirmed; filesystem behaviour suspected)
- `write_new_atomic` publishes the temp file with `::link(temp, path)` (src/io/replace_mac.cpp:58-60). FAT and exFAT have no hard links, so `link(2)` returns an error.
- That makes `commit_sidecar` (src/meta/write.cpp:1153) and `revert_blobs` (write.cpp:1393) return `status::io` for every RAW, HEIC or video on a card that has no `.xmp` yet.
- The same call is used for add-on settings (src/addon/settings_values.cpp:281) and src/remote/library.cpp:87.
- Windows uses `MoveFileExW` without `REPLACE_EXISTING` (src/io/replace_win.cpp:73), which works on FAT. So this is a Mac-only parity bug.
- Fix: on `ENOTSUP`/`EPERM`/`EXDEV` from `link`, fall back to `renamex_np(temp, path, RENAME_EXCL)`, the macOS no-overwrite rename. Test it with an `hdiutil` exFAT image (the memory note "Import add-on local testing" describes that recipe).

### MEDIUM

**M1. macOS `replace_atomic` drops the original's extended attributes, ACL, owner, birth time and hard links, even though the docs say it preserves the original.** (confirmed by reading)
- src/io/replace_mac.cpp:65-85 creates a new inode, copies only the mode bits, then calls `rename`.
- Lost on every in-file metadata edit, revert, or lossless rotate of a JPEG:
  - Finder tags and colour labels (`com.apple.metadata:_kMDItemUserTags`);
  - `kMDItemWhereFroms`, `com.apple.FinderInfo` and the quarantine flag;
  - the ACL;
  - the creation date, which resets to "now" and is what Finder's "Date Created" shows;
  - the group owner;
  - hard links, which are split.
- Windows' `ReplaceFileW` keeps attributes, ACL and creation time (replace_win.cpp:84-101, docs/design/06-metadata.md:376).
- The parent directory is not fsynced after `rename`. That is low risk on APFS but matters on HFS+ and external volumes.
- Fix: `fcopyfile(src_fd, tmp_fd, nullptr, COPYFILE_XATTR | COPYFILE_ACL | COPYFILE_STAT)` before the rename. `fchown` is best effort, and birth time can be set with `setattrlist(ATTR_CMN_CRTIME)`.

**M2. The rotate path follows symlinks differently from the metadata writer and detects concurrent changes more weakly.** (confirmed by reading)
- The metadata writer canonicalises the path (write.cpp:1173) and compares a size+mtime stamp just before the swap (write.cpp:1274).
- `run_rotation_write` does neither (src/shell/edit_session.cpp:690-697):
  - it compares size only;
  - it calls `replace_atomic(w.path)` on the uncanonicalised path.
- Effects:
  - If the open path is a symlink, `rename` replaces the link itself with a regular file. The link is gone, and the target keeps the unrotated image.
  - A same-size change by another app between the read and the swap (for example Lightroom flipping the orientation tag) is silently overwritten.
- Fix: reuse `load_and_plan`'s canonical path and stamp check.

**M3. Rotate rewrites the original with no check that the new pixels match, and it unlinks the original's EXIF thumbnail.** (confirmed by reading)
- The metadata writer re-reads its output and verifies the pixel digest, tags, ICC and thumbnail before swapping (write.cpp:1044-1082).
- The rotate path writes whatever `transform()` returns (edit_session.cpp:695-696). The only guard is libjpeg not erroring.
- `write_markers(..., rotated=true)` calls `exif_drop_thumbnail` on the original's own EXIF (lossless_jpeg.cpp:67). Every rotated original permanently loses its IFD1 thumbnail link.
- Fix:
  - Decode the result at 1/8 scale with `decode_jpeg(scale_denom=8)` and compare it to the rotated 1/8 decode of the source.
  - Rotate the thumbnail too, or keep it and accept the mismatch, since the orientation tag is reset anyway.

**M4. After a rotation lands on disk, Undo cannot undo it.** (confirmed by reading)
- `land()` replaces the stack with `ops_for(remaining)` plus the colour ops (edit_session.cpp:180-197).
- `undo_edit` then pops from the new stack and returns `refused` when it is empty (edit_session.cpp:619-624).
- So: press R, wait about 100 ms for the write, press Ctrl+Z, and nothing happens. The user has to rotate three more times, which causes three more rewrites of the original.
- Fix: keep a "written turns" record in the session so that undo pushes the inverse D4. That queues one more rewrite but restores the expected behaviour.

**M5. Export and print bake run single-threaded at full resolution, while `parallel_bands` exists and is already used by the detail pass.** (confirmed by reading)
- `bake()` loops over every output pixel on one thread (src/edit/bake.cpp:146-162 and 173-196). Each pixel does several `half_to_float` calls, a `gfx::adjust` and an sRGB table lookup. Straighten/resize also supersamples up to 8x8 bilinear taps.
- `edit::render` has the same serial row loops (src/edit/geometry.cpp:72, 128).
- `apply_detail` already uses `core::parallel_bands` (src/edit/detail.cpp:21, 40, 111, 126, 199).
- Rows are independent, so wrapping the y-loop in `core::parallel_bands(out.height, 16, ...)` is mechanical, and the cancellation check can move to the band. `band_threads()` caps it at 4.
- Expect about 3-4x on a colour export or print render. CLAUDE.md asks for a before/after measurement, and `[.perf-bench]` exists.

**M6. The Mac CI never runs the real-camera RAW and HEIC corpus tests, and Mac uses different LibRaw/libheif builds.** (confirmed by reading)
- The `mac-build` job does not restore the `test-media` cache and does not set `MV_REQUIRE_CORPUS` (.github/workflows/ci.yml:101-178). Only the Windows `build` job sets it (ci.yml:796).
- tests/test_raw.cpp:408-416 and tests/test_heif_avif.cpp:665 therefore SKIP on every Mac run.
- The Mac links the dynamic LGPL LibRaw/libheif from tools/mac/dependencies/vcpkg.json (vcpkg.json:151). Those Mac-specific decoder builds have no real-file coverage in CI.
- tests/test_corpus.cpp, the guard that fails when CI forgets the flag, is only compiled on Windows (CMakeLists.txt:1339; it is absent from cmake/darwin.cmake).
- Fix: make `mac-build` restore the media cache (`needs: test-media`, same key) and set `MV_REQUIRE_CORPUS=1`.

**M7. No sanitizer ever runs on the Mac-only code.** (confirmed by reading)
- Sanitizers run on Windows (`msvc-asan`, ci.yml:695) and on the Linux portable core (ci.yml:347). cmake/darwin.cmake has no `MV_ASAN` option.
- Never run under ASan/UBSan: `*_mac.mm`, io/replace_mac.cpp, the Metal backend, PDFKit and AVFoundation glue, and the dynamic Mac LibRaw/libheif.
- An ASan+UBSan Debug leg of the core tests on macos-15, run nightly on `schedule` to stay out of PR time, would close this gap.

### LOW

**L1. "Strip GPS" on the lossless export path copies Extended XMP untouched.** (confirmed by reading; real-world impact small)
- `write_markers` matches only the main XMP namespace (lossless_jpeg.cpp:72-75). An APP1 `http://ns.adobe.com/xmp/extension/` segment falls through to `jpeg_write_marker` even under `minus_gps`.
- Fix: drop the extension segments when the main packet had GPS, or always drop them under `minus_gps`.

**L2. A transposing lossless JPEG transform swaps each quant table in place in the destination.** (confirmed by reading)
- See lossless_jpeg.cpp:262-272. If two components share a table index the result is still correct, so this is not a bug. I checked it and it is fine; noting it only so nobody needs to recheck.

**L3. `text_in_image` decodes the full-resolution image only to shrink it to `kTextImageMaxEdge`.** (confirmed by reading)
- See src/shell/text_in_image.cpp:50-64. It also has no `job_context`, so it cannot be cancelled.
- `codec::decode_jpeg(..., scale_denom)` already exists (src/codec/decode.h:32-36). Picking the scale from the header would cut decode time and memory about 4-16x for OCR.
- `render_picture` for print (src/edit/export.cpp:299-322) has the same pattern.

**L4. Peak memory of a colour export is about 20 bytes per source pixel.**
- `decode_linear` produces half-float RGBA at 8 B/px. `apply_detail` makes a second full copy at 8 B/px (export.cpp:196-203). The output raster adds 4 B/px.
- That is about 480 MB for 24 MP and about 1.2 GB for 61 MP. Exporting from a RAW adds LibRaw's own buffers on top.
- Tiling the bake, or reusing the source buffer for the detail output, would halve it.

### Checked and fine (no finding)
- **Parsers:** they bound every read. `util::has` is overflow-safe (src/meta/parse_util.h:38-41). Every container has caps: C2PA depth 24 and 20k items (src/meta/c2pa.cpp:33-37); PNG text inflate 1 MiB per chunk and 8 MiB total; HEIF meta box 4 MiB; JPEG header 16 MiB (src/meta/probe.cpp:24-30; src/meta/containers.cpp:23-26, 405-406). The EXIF patchers check bounds on every IFD (src/codec/exif.cpp:77-87).
- **Exiv2:** it sees at most the capped prefix. Every entry point catches its exceptions (src/meta/still.cpp, sidecar.cpp, read.cpp:261).
- **The JPEG metadata writer** verifies thoroughly before swapping (write.cpp:1044-1082). It refuses MPF, trailer and Extended-XMP files and sends them to a sidecar instead. Revert checks the sidecar's BLAKE3 mark first.
- **Exports** never overwrite: `write_new` uses `O_EXCL`/`CREATE_NEW` and unlinks a partial file (replace_mac.cpp:34-45; replace_win.cpp:40-53).
- **Dependency pins:** the vcpkg baseline is `04a9d8e5` from 2026-09-03 (about 12 weeks old). It gives exiv2 0.28.8, libraw 0.22.2, ffmpeg 9.0.1, libjpeg-turbo 3.2.0, libheif 1.23.2 and libwebp 1.6.0. `overrides` is empty. Not far behind.

---

## 2. Performance and build opportunities (ranked)

1. **Parallelise `edit::bake` and `edit::render` (M5).** This is the largest win for export, print, flatten and copy at a small diff. Use `core::parallel_bands` on rows.
2. **No LTO/IPO anywhere.** (confirmed by reading) Neither `CMakeLists.txt` nor `cmake/*.cmake` mentions `INTERPROCEDURAL_OPTIMIZATION`, `/GL`, `/LTCG` or `-flto`.
   - The core is split into many static libraries (mv_core, mv_image, mv_edit, ...), so hot paths that cross library boundaries cannot be inlined. Examples: `half_to_float` and `gfx::adjust` called from `bake`, and the codec to image linear conversions.
   - Suggest `check_ipo_supported()` and `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON` for the shipped targets only, so Debug CI is not slowed. Measure open, navigate and export first.
3. **Scaled JPEG decode for consumers that do not need full size.** Applies to OCR, print (L3) and export with `long_edge`; export.cpp:249 decodes full size and then supersamples down.
4. **No SIMD and no CPU baseline flags.** Only src/addons/ai/platform_win.cpp mentions intrinsics or cpuid. There is no `/arch:AVX2` on Windows, so MSVC autovectorises for SSE2 only.
   - The bake/adjust kernel is plain scalar float code. `half_to_float` conversions with F16C on x64 and native fp16 on arm64 would help most.
   - Making the kernel loop vectorisable (struct-of-arrays rows, no per-pixel branches on `placed`) is a better step than raising the global baseline.
   - PGO is not used. It is low priority until LTO is in.
5. **Memory (L4):** tile the colour bake to stop holding two full linear frames.

---

## 3. Test and CI gaps

### Tests that sleep or depend on timing (project rule: none)
- **Scale:** 330 `sleep_for`/`Sleep`/`usleep` call sites across about 60 files. 123 fixed sleeps of 100 ms or more add up to about 74 s, and 8 sleeps given in whole seconds add up to 46 s. That is about 2 minutes of fixed wall-clock per full run, before the poll-loop sleeps.
- **Worst offenders:**
  - tests/test_ai_engine.cpp: 47 sites. The case "a quiet scan publishes at most hourly…" at :6724 sleeps 0.4 + 4×2.5 + 1.5 s ≈ 12 s. More fixed sleeps sit at :1702, :1837, :4603, :4638 (1.5 s each), :7032, :7222 and :7237 (2.5 s each).
  - tests/test_driftwire_session.cpp:766 sleeps a fixed 5 s, and :760 asserts a wall-clock bound (`to_bt_ms <= kSuspectMs + 3000`).
  - tests/test_phone_relay.cpp:235, :349, :398.
  - tests/test_driftwire_internet.cpp:534 (2.6 s).
  - tests/test_tree_watch.cpp:137 sleeps 1.5 s to prove that no callback arrives (a negative test that can only use time).
- **Fix:** an injectable clock for the AI engine's hourly tick and Driftwire's suspect timer. The tree watcher would need a stop-barrier hook.
- **No filed issue** covers this. #155 covers only a few small test gaps.

### Disabled or hidden tests
- 70-odd `[.tag]` cases. Almost all are intentional benches or model-dependent checks: `[.perf-bench]` ×27, `[.enhance-model]`, `[.people-*]`, `[.ocr-*]`, `[.write-seeds]` and so on. They are not hidden failures.
- `[.recycle]` is already tracked in #155.
- The Mac test step excludes one case by name: "An audio-only clip seeks, pauses in place" (ci.yml:177). The reason is that hosted runners have no audio device. That is legitimate, but the case runs nowhere in CI.

### Untested areas
- `src/abi`: 6 of 16 headers are included by any test. This is already filed as #151.
- `src/player`: 8 of 13 headers tested.
- `src/gfx`: 12 of 21 headers tested.
- No tests for the H1, H2 and M1-M3 write-path behaviours described above: motion photo, exFAT card, xattrs, symlink.

### Policy scripts
- All six `tools/*.ps1` policy checks run in the `policy` job (ci.yml:45-67). They run on windows-latest but scan the whole repo, including `.mm`/`.m` files (tools/check-module-graph.ps1:115, tools/check-hostable-core.ps1:124, 138). So Mac-only C++/Obj-C++ changes are covered.
- They do not scan Swift: src.swift and src.ios are outside every policy script. For example, the telemetry/no-paths rule is not enforced on the Swift chrome.
- `make-crash-raw.ps1` and `minidump-scan.ps1` are manual tools, not policy, so their absence from CI is fine.

### CI jobs that are slow or redundant (concrete savings)
1. **ctest runs serially everywhere.** None of the ctest invocations at ci.yml:177, 389, 515 and 811 uses `-j` or `--parallel`. Because of `catch_discover_tests`, each test case is its own ctest process: about 2,128 on the Mac, taking 15-20 minutes (ci.yml:170-172).
   - `ctest -j3` on macos-15 and `-j4` on windows-latest, with `RESOURCE_LOCK` on the few cases that bind fixed ports or share temp roots, should cut the test step by about half or more. That is roughly 7-10 minutes per Mac run and more on the four Windows legs.
2. **The Windows matrix has two MSVC Debug legs** (ci.yml:683-697: msvc Debug and msvc-asan Debug). The ASan leg runs the same Debug configuration with stricter checks. Dropping the plain msvc Debug leg, or moving it to `schedule`, saves one full Windows build plus a test run of up to 45 minutes on every PR.
3. **`mac-chrome` builds each Swift package twice** (ci.yml:81-89). `swift build -c release` runs ×3, then `swift test` (debug) runs ×3, and the release builds are thrown away. Either drop the release builds or use `swift test -c release`. That saves about 3 package compiles per run.
4. **No compiler cache.** No ccache, sccache or `CMAKE_*_COMPILER_LAUNCHER` appears in the workflows or CMake. Only vcpkg trees are cached. Adding ccache on Mac/Linux and sccache on the Windows legs, keyed on the toolchain, would make warm PR builds much faster.

### Jobs that cannot fail
- In the `fuzz` job, every metadata-parser harness is non-gating (`continue-on-error: true`, ci.yml:1243-1250): meta_c2pa, meta_jpeg, meta_png, meta_webp, meta_bmff, meta_xmp, meta_icc, meta_gpmf, meta_probe and others.
- The decode, animation, thumbnail, pdf, docx, pptx and driftwire harnesses are also non-gating (ci.yml:1229-1237).
- The comment says they move to gating "once CI has seen them start". They were added under #384 and are still non-gating, so the untrusted-input parsers have no gating fuzz coverage. Look at recent run logs and promote the ones that start cleanly.
- `present-gates` only warns on a skip unless `vars.MV_REQUIRE_PRESENT_GATES` is set (ci.yml:1143-1170). Already tracked in #149.
- The `continue-on-error` on cache-save steps (ci.yml:563, 658) is fine.

### Missing release hardening
- **Windows:** no `/guard:cf` (Control Flow Guard) and no `/CETCOMPAT` anywhere in CMake (grep has no matches). The viewer parses untrusted media in-process, and MSVC does not enable either by default. `/DYNAMICBASE`, `/NXCOMPAT`, `/HIGHENTROPYVA` and `/GS` are on by default.
- **Linux CLI:** cmake/driftwire-cli/CMakeLists.txt:75-90 is already well hardened: PIE, stack protector, `_FORTIFY_SOURCE=2`, CET/BTI, full RELRO. The Linux viewer (cmake/linux.cmake:26-37) has none of those, but it does not ship yet.
- **macOS:** the hardened runtime is applied at signing (tools/mac/macpack.py). Nothing is missing at compile level.
