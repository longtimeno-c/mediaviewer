# Portable core build

`cmake/portable` builds the parts of the core that need no window or GPU —
io's file / volume / verified-copy ports in their POSIX form, the add-on host
(`src/addon`), the Import engine (`src/addons/import`) and, when FFmpeg,
libspng and libjpeg are installed (pkg-config), the PR 13 / 14 clip core
(`src/edit/clip*`, `src/abi/clip_session`, `src/shell/trim_state`) — and runs
their suites on Linux or any POSIX machine. The clip suite synthesises its
clips with FFmpeg's own MPEG-4 and MP2 encoders; a headless build has no
hardware encoder (`hwencode_none.cpp`), so Path 2 is driven through a test
hook there. `mv_clip_tests "[bench]"` times a keyframe trim of a ~1 GB MP4
(or `MV_CLIP_BENCH_FILE`). It is a test build, not a
product platform (docs/design/12, 2026-09-24).

```
cmake -S cmake/portable -B build-portable \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_DIR=tools/portable
cmake --build build-portable && ctest --test-dir build-portable
```

`-DMV_ASAN=ON` adds ASan + UBSan. Without vcpkg, system SQLite and libsodium
work, with `-DMV_BLAKE3_SOURCE_DIR=<BLAKE3>/c` and
`-DMV_CATCH2_AMALGAMATED_DIR=<Catch2>/extras`.

## The CI job

The `portable-core` job in `.github/workflows/ci.yml` runs this build on every
pull request (Ubuntu, the pinned vcpkg baseline, `-DMV_ASAN=ON`): the Import
suite, the add-on packer's and the add-on SDK's cross-checks against the C++
verifier, and (Milestone E) the clip core and trim-mode suites, with the
distro's FFmpeg, libspng and libjpeg as test-only dependencies.
