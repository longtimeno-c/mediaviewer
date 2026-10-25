# Portable core build

`cmake/portable` builds the parts of the core that need no window or GPU —
io's file / volume / verified-copy ports in their POSIX form, the add-on host
(`src/addon`), the Import engine (`src/addons/import`), Driftwire's engine, its
add-on and its command line (`src/addons/driftwire`, `driftwire` in
`build-portable/bin`) and, when FFmpeg,
libspng and libjpeg are installed (pkg-config), the PR 13 / 14 clip core
(`src/edit/clip*`, `src/abi/clip_session`, `src/shell/trim_state`) — and runs
their suites on Linux or any POSIX machine. The clip suite synthesises its
clips with FFmpeg's own MPEG-4 and MP2 encoders; a headless build has no
hardware encoder (`hwencode_none.cpp`), so Path 2 is driven through a test
hook there. `mv_clip_tests "[bench]"` times a keyframe trim of a ~1 GB MP4
(or `MV_CLIP_BENCH_FILE`). It is the shared core's test build, not a
product build: MediaViewer ships on Windows and macOS today (docs/design/12,
2026-09-24), and Linux is to be a third host (D9 as the owner amended it on
2026-10-23). Linux's first product, the `driftwire` command line, is packaged
by its own project, `cmake/driftwire-cli` (docs/design/12, 2026-10-23;
docs/DEVELOPMENT.md "Linux packages").

```
cmake -S cmake/portable -B build-portable \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_DIR=tools/portable
cmake --build build-portable && ctest --test-dir build-portable
```

`-DMV_ASAN=ON` adds ASan + UBSan. Without vcpkg, system SQLite works, with
`-DMV_BLAKE3_SOURCE_DIR=<BLAKE3>/c`, `-DMV_CATCH2_AMALGAMATED_DIR=<Catch2>/extras`
and `-DMV_FTXUI_SOURCE_DIR=<FTXUI>`. libsodium must be 1.0.21 or newer
(Driftwire's ML-KEM-768; configure says so): a distro's 1.0.18 is too old, so
build it from source and point `PKG_CONFIG_PATH` at it. Use GCC: clang with
libstdc++ rejects `core/json.h`.

## The CI job

The `portable-core` job in `.github/workflows/ci.yml` runs this build on every
pull request (Ubuntu, the pinned vcpkg baseline, `-DMV_ASAN=ON`): the Import
suite, Driftwire's suite with the command line's (`"[driftwire]"`, its Secret
Service case against a gnome-keyring of the job's own), the add-on packer's and
the add-on SDK's cross-checks against the C++ verifier, and (Milestone E) the
clip core and trim-mode suites, with the distro's FFmpeg, libspng and libjpeg as
test-only dependencies. The `driftwire-cli-linux` job builds the command line's
packages (`cmake/driftwire-cli`).
