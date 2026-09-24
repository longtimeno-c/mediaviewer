# Portable core build

`cmake/portable` builds the parts of Milestone G that need no window, GPU or
codec — io's file / volume / verified-copy ports in their POSIX form, the
add-on host (`src/addon`) and the Import engine (`src/addons/import`) — and
runs their suite on Linux or any POSIX machine. It is a test build, not a
product platform (plan/12, 2026-09-24).

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

`ci-portable-core.patch` adds the `portable-core` job to
`.github/workflows/ci.yml` (Ubuntu, the pinned vcpkg baseline, ASan + UBSan,
the Import suite and the add-on packer's cross-check against the C++
verifier). It is a patch rather than a change to the workflow because the
session that wrote Milestone G could not push workflow files. Apply it with
`git apply tools/portable/ci-portable-core.patch`, then delete this section and
the patch.
