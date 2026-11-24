Build, run and test recipes. The product overview is in the [README](../README.md); how things
work is in [docs/design/](design/README.md). Links are relative to the repo root.

# MediaViewer: development notes

Long file: search for the section you need rather than reading it whole.

## Build

You need **Visual Studio 2022** (or Build Tools) with the C++ workload, the **Windows 10/11
SDK**, **CMake ≥ 3.28**, **vcpkg**, the **.NET 8 SDK**, and the **Windows App SDK 2.4
runtime** (the command-bar island is unpackaged). The native core still builds and tests
with no .NET present; without `dotnet` on `PATH` the lab runs as it did in PR 2
(`--no-chrome`). The build compiles the blit shaders (`src/gfx/*.hlsl`) with the SDK's `fxc.exe`,
found in the targeted SDK's `bin` without a developer shell; pass `-DMV_FXC=<path>` to override.

```powershell
# once
git clone https://github.com/microsoft/vcpkg $env:USERPROFILE\vcpkg
& $env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
./tools/install-windows-app-runtime.ps1   # unpackaged WinUI 2.4 runtime

# configure and build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

CMake finds vcpkg from `VCPKG_ROOT`, or from `%USERPROFILE%\vcpkg`, or from an explicit
`-DCMAKE_TOOLCHAIN_FILE`. Through PR 5 the manifest pulls `imgui`, `catch2`,
`libjpeg-turbo`, `libspng`, `lcms`, `sqlite3` and `ffmpeg` (LGPL only —
`avcodec`, `avformat`, `avfilter`, `swresample`, `swscale`, `dav1d`; no
`--enable-gpl`, no x264/x265, enforced by `tools/licence-check.ps1`). The rest of
the v1 set arrives with the PR that needs it, listed in [`vcpkg.json`](../vcpkg.json).
The first configure after PR 5 builds FFmpeg, which is not quick.

Other configurations:

```powershell
cmake -S . -B build-asan -A x64 -DMV_ASAN=ON     # AddressSanitizer
cmake -S . -B build-clang -A x64 -T ClangCL      # clang-cl, the CI second opinion
```

`-DMV_ASAN=ON` needs the **C++ AddressSanitizer** component
(`Microsoft.VisualStudio.Component.VC.ASAN`) in the Visual Studio Installer — the
ASan runtime is a DLL that ships beside `cl.exe` and nowhere else. The build
copies it next to every executable, so `ctest` and a double-click both work
outside a developer prompt. Configure says so and stops if the component is
missing, rather than producing a tree whose every test hangs for its full
timeout with nothing in the log.

### macOS (Mac PRs 1–6)

Apple Silicon or Intel, macOS 14+, CMake ≥ 3.28, vcpkg, a full Xcode install (Command Line
Tools alone are not enough — `swift build`'s SwiftUI target and `xcrun metal` both
need it), Swift 6. Build natively on the Mac you are on: the snippets below use the
`arm64-osx` triplets; on an Intel Mac use `x64-osx` and `x64-osx-dynamic` instead. The release
is one universal app made by building both and joining them (RELEASING.md). Intel builds
launch but the frame-pacing gate has not been measured on Intel hardware yet (docs/design/12,
2026-09-24). This path builds the native Mac app and its dynamic FFmpeg libraries; it does
not build WinUI or the Windows lab.

```sh
export VCPKG_ROOT=/path/to/vcpkg   # bootstrapped
# LibRaw's OpenMP runtime, built for macOS 14 (Homebrew's targets the build Mac's OS).
python3 tools/mac/build_libomp.py --prefix "$PWD/build/libomp/arm64" --arch arm64
export MV_LIBOMP_ROOT="$PWD/build/libomp/arm64"
# Dynamic LGPL codecs use a separate manifest/tree; CMake installs the static
# permissive dependencies from the root manifest. See RELEASING.md.
# tools/mac/dependencies/vcpkg-configuration.json adds the repo's overlay ports
# (LibRaw's OpenMP colour patch); re-running this on an older tree rebuilds LibRaw.
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic \
  --overlay-triplets="$PWD/tools/mac/triplets" \
  --x-manifest-root="$PWD/tools/mac/dependencies" \
  --x-install-root="$PWD/build/vcpkg_dynamic"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DMV_VCPKG_DYNAMIC_PREFIX="$PWD/build/vcpkg_dynamic/arm64-osx-dynamic"
cmake --build build
./build/bin/mediaviewer_lab --open some.jpg
./build/bin/frametime --seconds 60 --lab ./build/bin/mediaviewer_lab
ctest --test-dir build --output-on-failure
```

This has been built and linked for real (not just reviewed) with `cmake` + `ninja` +
a manifest-mode `vcpkg` install (`imgui[metal-binding]`, `libjpeg-turbo`, `libspng`,
`lcms`, `sqlite3`, `catch2`) and `swift build` for the SwiftUI chrome — `mv_tests`
passes (210 assertions, 60 cases). **The macOS SDK matters**: once the filmstrip/gallery
pulled in SwiftUI's `Lazy*Stack`, linking against an SDK that doesn't match the Swift
toolchain's own SDK failed with "cannot link directly with 'SwiftUICore'" — use
`xcrun --show-sdk-path` (or the SDK your installed Xcode ships) rather than an older one
you may have lying around for a lower deployment target; `CMAKE_OSX_DEPLOYMENT_TARGET`
stays 14.0 either way. On a real display (2026-09-19) `frametime --seconds 60` passes with the SwiftUI chrome
on screen (3600 frames, 0 dropped, p99 16.9 ms, idle 0.14 % of one core, 0 presents). Not yet
exercised by hand: drag-and-drop, copy/move/Trash and slideshow on real folders — do a manual
pass before trusting them. Thumbnails are cached in `~/Library/Caches/MediaViewer/thumbs`,
never in the folder being browsed.

The whole D5 still set decodes on Mac: JPEG, PNG, BMP, GIF, APNG, TIFF, WebP, ICO, HEIC/HEIF,
AVIF, and camera RAW (CR2/CR3/NEF/ARW/DNG through LibRaw). Verified on real files (the CC0
RAW set in `tools/testmedia/raw-manifest.json`, the libheif example HEIC) plus generated
samples: every format thumbnails and opens on the canvas, and `mv_tests` runs the codec tests
including the RAW ones ("original bytes unchanged", cancel latency) when
`tools/testmedia/raw/` and `heif/` are populated (they skip otherwise; `fetch-raw.ps1` is the
Windows fetcher, the manifests are plain JSON). A JPEG or RAW shows its preview first (JPEG
DCT 1/4, or the RAW's embedded JPEG: about 100 ms on a 42 MP ARW) and the full decode
(about 5 s for that ARW) replaces it in place. HEIC always uses the bundled libheif here; an
ImageIO fast path is a later change to `codec/os_decode_mac.cpp`.

The Mac has a real menu bar (File / View / Go / Window / Help), `?` opens a shortcuts sheet,
and in the gallery `↑`/`↓`/`W`/`S` move by row, `Enter` opens the selection and `+`/`-` resize the
thumbnails.

**Video on Mac (Mac PR 5).** FFmpeg is LGPL and dynamic-link only. The dynamic manifest
installed above includes it alongside libheif/LibRaw; no extra install is needed.

Open a folder with clips in it. `Space`/`K` play/pause, `,` `.` frame step, `Q`/`E` ±2 s,
`J`/`L` ±10 s, `Shift+Q`/`Shift+E` speed 0.25–4×, `Shift+M` mute (`?` lists them; the transport
bar floats above the filmstrip while a clip is on screen and fades after 2.5 s of playback
with no activity — see the transport paragraph below). `F3` names the decoder that is
*actually* running (`SOFTWARE` is spelled out, never silent), the clock source, the A/V error
and the counters. H.264 and HEVC (8- and 10-bit) decode in hardware; MPEG-2 and MPEG-4 fall
back to software on Apple Silicon and say so.

`playprobe` is the headless pipeline check — it plays a clip on the system `MTLDevice` against
a 60 Hz timer and prints the decoder used, the presenter counters and the drift slope:

```sh
./build-darwin/bin/playprobe clip.mov --seconds 30 --expect hw --mute
```

Verified on an Apple Silicon Mac with generated clips (H.264, 4K60 10-bit HEVC, an HLG-tagged
HEVC, MPEG-2 TS, MKV, AVI, no audio): 4K60 10-bit HEVC plays at full rate through VideoToolbox
(P010 path) with 0 dropped frames; skip and frame-step land on the exact frame against a
burned-in timecode; repeated photo ↔ video navigation leaves memory and thread count flat.
Not yet verified: an iPhone HLG capture (only a synthetic HLG-tagged clip), VP9/AV1/WebM (no
encoder in the LGPL build to make a clip), and audio-device hot-swap.

`frametime` on Darwin requires `drop_source` `Metal display-link`. Copying a
Windows DXGI JSON report over is a failed gate, not a pass.

`F3` toggles the overlay, `Space` advances the folder (or pauses a running slideshow),
`R` resets the frame-time measurement, `Esc` closes the gallery / leaves fullscreen or
slideshow / quits, `0`–`4` are the zoom presets, `T`/`G` toggle the filmstrip/gallery,
`F11`/`F` fullscreen, `F5` starts a stills slideshow. Idle (`--static`) must park the
cursor off the window.

### iOS (the search and Driftwire engines)

The engines the iPhone app links statically (`mv_ai_engine`, `mv_infer`, `mv_driftwire_engine`), from the
root `CMakeLists.txt` through `cmake/ios.cmake`, with their own small manifest (`tools/ios/vcpkg.json`) and the
`arm64-ios` / `arm64-ios-simulator` triplets. The chrome in `src.ios` will link these when it lands. Xcode with the iOS SDK and a
Simulator runtime, CMake ≥ 3.28, Ninja, vcpkg. The minimum iOS is set in the root `CMakeLists.txt` and both
triplets, so change them together.

```sh
cmake -S . -B build-ios-sim -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-ios-sim
tools/ios/run-simulator-smoke.sh "$PWD/build-ios-sim/bin/mv_ios_core_smoke"
```

`CMAKE_OSX_SYSROOT=iphoneos` builds for a device. The smoke runs in a Simulator of its own, which it deletes:
ONNX Runtime without `dlopen` on a real graph, an index and a text search through a fake model, the Driftwire
engine and its ports over loopback.

The default target runs `tools/ios/licence_check.py` on the installed ports, ONNX Runtime's licence file and the
smoke program. It fails on a GPL-family or non-permissive licence, an undeclared one, a non-system library or a known
(L)GPL symbol. It does not see the app until the app's build calls it: `build-ios-*/ios-licence-check.sh --binary
MediaViewer.app`, before the app is stripped. CI's `ios-core` job builds both SDKs and runs the smoke.

ONNX Runtime's static build carries Microsoft's telemetry uploader. `src/infer/ort_static_ios.cpp` sets
`ORT_DISABLE_TELEMETRY` before the runtime starts so the uploader is never made. `tools/ios/ort_telemetry_pin.py`
fails the build if the collector addresses or that variable's name change, or if `MV_ORT_VERSION` moves: on a bump,
re-read `core/platform/posix/telemetry.cc` and update the pin.

### Linux (Linux PR 1)

The present lab, `frametime` and the portable tests, from the root `CMakeLists.txt` (it includes
`cmake/linux.cmake`). GCC 13 or newer, CMake ≥ 3.28, Ninja, pkg-config, and the Wayland, xkbcommon
and glvnd development packages; Weston and Mesa to run the smoke headless. The protocol XML is
vendored (`tools/linux/protocols/`), so the distribution's wayland-protocols is not needed.

```sh
# Ubuntu 24.04 (Debian 12's CMake 3.25 and GCC 12 are too old)
sudo apt-get install build-essential cmake ninja-build pkg-config libwayland-dev libxkbcommon-dev \
  libegl-dev libgl-dev libopengl-dev weston libegl-mesa0 libgl1-mesa-dri

# with vcpkg (as CI): the root manifest installs catch2 and imgui[opengl3-binding] only
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
# or without vcpkg: an imgui 1.92.8 checkout and an installed Catch2 3
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMV_IMGUI_SOURCE_DIR=$HOME/src/imgui -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure   # mv_tests, frametime_harness, and linux_lab_smoke when weston exists
```

`-DMV_ASAN=ON` with `-DCMAKE_BUILD_TYPE=Debug` is the ASan + UBSan configuration. The lab runs
in a Wayland session only; it exits 2 without `WAYLAND_DISPLAY` (X11 is not built yet).

```sh
./build-linux/bin/mediaviewer_lab                    # Space sweep, F3 or F overlay, R reset, Esc quit
./build-linux/bin/mediaviewer_lab --soak 60 --json report.json --gate
./build-linux/bin/mediaviewer_lab --soak 60 --static --json idle.json --gate
./build-linux/bin/frametime --seconds 60             # both soaks, the gate, the baseline check
```

`frametime` on Linux requires `drop_source` `Wayland presentation feedback`, which only feedback
with `VSYNC` and `HW_CLOCK` and a refresh count (an msc, or KWin's hardware timestamps over a
constant refresh) earns; headless Weston's is labelled
`clock intervals (weaker)` and fails, as it must. `--baseline` / `--update-baseline` work as on
Windows. For the gate, run it in a real session on a quiet machine with the display awake
(`systemd-inhibit --what=idle:sleep:handle-lid-switch ./build-linux/bin/frametime --seconds 60`)
and the pointer off the window for the idle soak.

The headless smoke by hand (what CI's `linux-lab-smoke` runs; `MV_WESTON_RENDERER=pixman` for
the other renderer):

```sh
bash tools/linux/lab_smoke.sh build-linux/bin/mediaviewer_lab build-linux/bin/frametime \
  build-linux/bin/libmv_egl_swap_fault.so   # optional: one failed swap, the lab must go on
# or by hand:
export XDG_RUNTIME_DIR=$(mktemp -d); chmod 700 "$XDG_RUNTIME_DIR"
weston --backend=headless --renderer=gl --socket=wl-test --idle-time=0 &
WAYLAND_DISPLAY=wl-test ./build-linux/bin/mediaviewer_lab --soak 10 --json /tmp/r.json
```

Start Weston with `--debug` to take a picture of it with `weston-screenshooter`.

### MediaViewer.app and a shippable Mac build (Mac PR 8)

`mediaviewer_lab` stays the bare instrument `frametime` drives. The same sources also
build `MediaViewer`, the executable inside **MediaViewer.app**:

```sh
# a local, ad-hoc-signed bundle (no updater unless a key is given)
cmake --build build-darwin --target mediaviewer_app
open build-darwin/MediaViewer.app
```

The bundle holds the app, `MediaViewerThumbnails.appex` (Finder thumbnails for the D5
still set and for MKV, WebM, AVI and TS clips, run by Quick Look in its own sandboxed process, never inside Finder), the LGPL
dylibs in `Contents/Frameworks`, and, when configured, Sparkle. It registers the D5 still
and video types at rank *Alternate*: MediaViewer shows up in Finder's **Open With** and never
makes itself the default merely by being installed. First launch shows a setup sheet with
*Use MediaViewer for all supported photos and videos* checked. Continue applies that choice
through macOS; untick it or choose Not Now to keep current defaults. The MediaViewer menu
has the same command for later, and updates preserve the previous setup choice.

#### Runbook: build, sign, release, update (macOS)

Everything below was run end to end on Apple Silicon except notarization and Sparkle,
which need your Apple credentials — those steps say so. `docs/design/13` is the design; this is
the procedure.

**Universal (Apple Silicon + Intel).** The shipped app is universal, but each build below is
for the Mac you run it on. The release workflow builds once on each architecture and joins
the two apps with `python3 tools/mac/lipo_merge.py --arm64 <arm64 app> --x86_64 <x86_64 app>
--out <universal app>`, then runs `macpack.py release` on the result (RELEASING.md). To
reproduce that locally you need one build per architecture; a single build stays single-arch.
Intel has been built but not measured for frame pacing (docs/design/12, 2026-09-24).

**0. Prerequisites (once per machine)**

| Need | Notes |
|---|---|
| Full Xcode, macOS 14+ | Command Line Tools alone are not enough (Swift/SwiftUI, `xcrun metal`, `notarytool`) |
| CMake ≥ 3.28 | `python3 -m pip install --user cmake` puts it in `~/Library/Python/3.x/bin` — add that to `PATH` |
| Ninja | vcpkg downloads one under `$VCPKG_ROOT/downloads/tools`; or `brew install ninja` |
| OpenMP runtime | `tools/mac/build_libomp.py` builds LLVM's libomp for the app's macOS 14.0 (hash-pinned sources); export `MV_LIBOMP_ROOT` to its prefix before the vcpkg install and configure. The dynamic codec triplets use it for bounded parallel RAW decoding, and packaging bundles it. Homebrew's `libomp` is only a fallback when `MV_LIBOMP_ROOT` is unset: its dylib declares the build Mac's macOS as its minimum, so `tools/mac/check_minos.py build-darwin/MediaViewer.app` (run by the release) fails such a bundle. |
| vcpkg, bootstrapped | `export VCPKG_ROOT=…`. The baseline is pinned in `vcpkg.json`; do not float it |
| Python 3.10+ | for `dmgbuild==1.6.7` (`pip install -r tools/mac/requirements.txt`). On 3.9, `dmgbuild==1.6.5` works for a local dry run only |
| Apple Developer Program | needed for Developer ID signing and notarization; without it you can only build the ad-hoc bundle |

**1. Build the app**

```sh
export VCPKG_ROOT=/path/to/vcpkg
python3 tools/mac/build_libomp.py --prefix "$PWD/build-darwin/libomp/arm64" --arch arm64
export MV_LIBOMP_ROOT="$PWD/build-darwin/libomp/arm64"
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic \
  --overlay-triplets="$PWD/tools/mac/triplets" \
  --x-manifest-root="$PWD/tools/mac/dependencies" \
  --x-install-root="$PWD/build-darwin/vcpkg_dynamic"

cmake -S . -B build-darwin -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DMV_VCPKG_DYNAMIC_PREFIX="$PWD/build-darwin/vcpkg_dynamic/arm64-osx-dynamic"
cmake --build build-darwin --target mediaviewer_app
open build-darwin/MediaViewer.app
```

That is an ad-hoc-signed bundle: fine on this Mac, but **it will not launch once
re-signed ad hoc with the hardened runtime** (dyld rejects the bundled dylibs with
"different Team IDs"). Only a real Developer ID signature, which gives every binary in the
bundle the same Team ID, satisfies that check. `python3 tools/mac/check_plists.py`,
`python3 tools/mac/test_macpack.py` and `python3 tools/mac/test_check_minos.py` need no Mac and
run anywhere. `python3 tools/mac/check_minos.py build-darwin/MediaViewer.app` fails when any
bundled binary declares a minimum macOS above the app's `LSMinimumSystemVersion`.

**2. One-time signing and update setup**

1. *Developer ID Application certificate.* In Xcode: Settings → Accounts → your team →
   Manage Certificates → **+** → *Developer ID Application*. (By hand: create a CSR in
   Keychain Access, upload it at developer.apple.com → Certificates choosing **Developer ID
   Application** and the **G2 Sub-CA**, then double-click the downloaded `.cer`.) Keep the
   private key on this Mac. Never commit `.cer`, `.p12` or CSR files (they are gitignored).
2. *Apple's G2 intermediate.* If `security find-identity -v -p codesigning` shows "0 valid
   identities" although the certificate is installed, the chain is missing:
   `curl -LO https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer` and
   `security import DeveloperIDG2CA.cer -k ~/Library/Keychains/login.keychain-db`.
   The identity string it then prints — `Developer ID Application: <Team name> (<TEAMID>)` —
   is what `--identity` takes.
3. *Notarization profile.* Create an app-specific password at appleid.apple.com, then
   `xcrun notarytool store-credentials mediaviewer-notary --apple-id <id> --team-id <TEAMID>`
   (it prompts for the password, so it never lands in shell history).
4. *Sparkle keys.* Download the Sparkle 2.9.6 release tarball, run
   `./Sparkle-2.9.6/bin/generate_keys`, and keep the printed **public key**. The private key
   lives in your login keychain: **back it up** (`generate_keys -x file`). Lose it and no
   installed copy can ever accept another update.

**3. Cut a release build**

Reconfigure with the updater's key, and raise the build number every release — Sparkle
compares `CFBundleVersion` (`MV_MAC_BUILD_NUMBER`, default = the project version), so a
build that does not increase is never offered:

```sh
cmake -S . -B build-darwin -DMV_SPARKLE_PUBLIC_ED_KEY=<public key> -DMV_MAC_BUILD_NUMBER=<n>
cmake --build build-darwin --target mediaviewer_app     # fetches Sparkle into the bundle

# safe local rehearsal: signs and builds the .dmg/.zip, sends nothing to Apple
python3 tools/mac/macpack.py release --app build-darwin/MediaViewer.app \
    --identity "Developer ID Application: … (TEAMID)" --skip-notarize --allow-no-updater \
    --out-dir /tmp/mv-rehearsal

# the real thing: notarizes and staples the app and the image, and signs the appcast
cmake --build build-darwin --target mediaviewer_app     # release re-signs the app in place: rebuild first
python3 tools/mac/macpack.py release --app build-darwin/MediaViewer.app \
    --identity "Developer ID Application: … (TEAMID)" --notary-profile mediaviewer-notary \
    --sparkle-bin build-darwin/_deps/sparkle-2.9.6/bin \
    --download-url-prefix https://github.com/longtimeno-c/mediaviewer/releases/download/v<version>/
```

Output in `build/release` (or `--out-dir`): `MediaViewer-<version>.dmg` (drag to
Applications, the GPL shown on mount; opening the app straight from the image offers to
move it to Applications, relaunch it and eject the image), `updates/MediaViewer-<version>.zip` and
`updates/appcast.xml`. `--phased-rollout-seconds` spreads an update over time.
Notarization uploads the app and image to Apple and takes a few minutes; without a
`--sparkle-bin` no appcast is written.

On first launch the app asks once whether to become the default for every type in its
`CFBundleDocumentTypes` (photos and videos). It records what it offered
(`MVDefaultViewerTypes`) and whether the user took it (`MVDefaultViewerChosen`). When a
release adds a type and the user had said yes, the next launch sets that type too, off the
main thread; a type the user later moved to another app in Finder is left alone. Installs
from before the record count as a yes if MediaViewer still opens one of their photo types.

**4. Verify the artefacts**

```sh
codesign --verify --deep --strict --verbose=2 build-darwin/MediaViewer.app
spctl --assess --type execute --verbose=2 build-darwin/MediaViewer.app     # "Notarized Developer ID"
xcrun stapler validate build/release/MediaViewer-<version>.dmg
# the image has a licence page, so a headless mount must accept it:
mkdir /tmp/mvdmg && yes | hdiutil attach -nobrowse -readonly -mountpoint /tmp/mvdmg build/release/*.dmg
ls /tmp/mvdmg            # MediaViewer.app  Applications
hdiutil detach /tmp/mvdmg
```

Copy the app out with `ditto --norsrc --noextattr`, not `cp`/`ditto` defaults; Finder
metadata copied along invalidates the signature. Then open a photo and a clip from the
copy, and check Finder's **Open With** and a Quick Look thumbnail.

**5. Publish, and later update**

1. Tag and create a GitHub release `v<version>` and mark it **latest**: the app's feed is
   `…/releases/latest/download/appcast.xml`. Upload the `.dmg`, the `.zip` and
   `appcast.xml`.
2. To ship a new version: bump `project(… VERSION x.y.z)` in `CMakeLists.txt` (the marketing
   version), raise `MV_MAC_BUILD_NUMBER`, rebuild `mediaviewer_app`, rerun step 3 with the
   new `v<version>` in `--download-url-prefix`, and upload the new files to a new release.
   Keep earlier zips in `updates/` so `generate_appcast` keeps them in the feed.
3. Test the update path before announcing: install the *previous* release from its `.dmg`,
   publish the new one, and confirm the old build's command bar, right after About, shows
   *Checking for updates…*, then *Downloading update x.y.z…* with a bar, then **Update ready
   — restart**.
   Installed copies accept only a feed and archive signed with the key baked in at build
   time.
4. First install is the disk image; every update after that is the zip, applied by Sparkle
   on **Update ready — restart** or on quit.

`python3 tools/mac/check_plists.py` and `python3 tools/mac/test_macpack.py` need no Mac and run anywhere.

### Phone access (plans: `docs/plans/mobile.md`, `docs/plans/phone-access-addon.md`)

An add-on since 2026-11-14 (`phone-access`, `src/addons/phone`, built as `build/addons/phone-access/
libmv_phone_access.dylib` / `mv_phone_access.dll`; packed by `tools/package/addon-pack.py --addon phone-access`).
Not installed by default: Settings → Add-ons → Phone access offers it (Mac `PhoneAccess.swift`, Windows
`IslandHost.PhoneAccess.cs`) and, once loaded, draws its card, which is the add-on's own view tree
(`screens.*`, its QR `qr.*` as a surface). The work is `link.*` (the state machine), `desk.*`
(claim/codes/devices/nearby), `service.*` (the one link per process; the first window's process holds
`instance.lock` in the feature's folder) and `self_host.*` (this computer as the server), over `src/wire`
(Driftwire's protocol) and `src/catalogue`. Local search's catalogue reaches it through the host table's
`catalogue_*` rows. Keys stay in the OS store under "MediaViewer Phone access"; the library's folder stays
where the app kept it (`~/Library/Application Support/MediaViewer/Phone Access`,
`%LocalAppData%\MediaViewer\phone-access`), so a library set up before the add-on keeps its id.
`MV_PHONE_ACCESS_DIR` points the add-on at another folder (the tests use it).

**This computer** (Set up the service…, [self-hosted.md](plans/self-hosted.md) §16) needs the
`driftwire` program installed (`brew install longtimeno-c/tap/driftwire`, or `install.ps1`); the add-on finds
it and runs its `host` commands. To try the card against a local build, put `build/bin/driftwire` on one of
the places it looks (`~/.local/bin/driftwire`); remove the login entry afterwards with
`driftwire host start-at-login off`. A local build cannot load its own unsigned add-on in the app; sideload it
as the AI pack's recipe does (a build with `MV_ADDON_DEV_PUBLIC_KEY`).

Tests, always filtered by tag: `mv_import_tests "[phone-addon]"` (the card's trees against the core's
vocabulary, the secure field, the catalogue rows, the manifest, the module through a signed store in a
folder of the test's own), `mv_import_tests "[phone-qr]"` (the encoder against the matrices the C# encoder
had, read by a phone scanner), `mv_import_tests "[phone-self-host]"` (the setup against the test build of
`driftwire host` on loopback, the VPN switch included), `mv_import_tests "[phone-pairing]"` (loopback, including a library set up,
claimed, paired and revoked with no Local search), `mv_import_tests "[phone-pairing-bonjour]"` (this
machine's mDNSResponder), `mv_ai_tests "[phone-chain]"`, `mv_ai_tests "[phone-link]"` (the link over a Local
search engine as its source), `swift test` in `src.swift/MediaViewerChrome` (`PhoneAccessTests`). To try it
without a Synology, run `driftwire serve` on this computer with a `serve.json` naming a folder, and give the
card that machine's address. Windows advertises the nearby service with `wire/advert_win.cpp` (DnsServiceRegister).

Not done: the add-on's card seen running in the app on either host (the Windows chrome is compiled only;
the Mac app built, the card drawn only through the core's registry in `[phone-addon]`), the add-on published
on a release channel, the phone's file mode for a library with no catalogue (step 5 of the plan), a Bonjour advert on Linux, and any
run against a real Synology or phone.

### Another computer's folders (plan: `docs/plans/desktop-client.md`, #686)

Another desktop pairs with a self-hosted computer or a Synology from its code (the phone's code, no
password) and sees its folders pinned at the top of the folder tree, read-only. The core is `src/remote`
(`key.*` the `remote:` keys, `cache.*` originals by content identity, `library.*` servers, pairing,
listings, tiles and fetches) over the phone's own link (`src/mobile/link.*`, shared through the
`mv_mobile_route` OBJECT library in `cmake/remote.cmake`, so `libmv_mobile_link.a` keeps its contents for
the iOS project). The Mac host is `main_mac.mm` (File → Connect to a Server…, `mv_chrome_remote_*`) with the
rows in `FolderTreeView.swift`; Windows is the core's session (`abi/abi.cpp`, `abi/remote_abi.cpp`,
`mv_remote_*`, ABI 0.44) with the rows in `IslandHost.Remote.cs`. Servers are in `servers.json`
(Mac `~/Library/Application Support/MediaViewer/Remote Folders`, Windows `%LocalAppData%\MediaViewer\remote`);
their keys in the OS store ("MediaViewer Driftwire", `remote_<id>`); the cache in
`~/Library/Caches/MediaViewer/Remote` / `%LocalAppData%\MediaViewer\remote-cache`.

To try it on one Mac: run `driftwire host` (or `driftwire serve` with a `serve.json`) with a folder shared,
take the code from `driftwire host pair` or the Phone access card, and paste it in File → Connect to a Server….

```bash
build/bin/mv_tests "[desktop-client]"          # loopback: pairing, listing, tiles, fetches, unreachable / offline / revoked
build/bin/mv_tests "[.desktop-client-perf]"    # remote first pixel, full resolution, arrow-to-next over loopback
```

`MV_REMOTE_BENCH_JSON=path` writes the timings. They are also in `"[.perf-bench]"`.

### iPhone and iPad interface (`src.ios/`, plan: `docs/plans/mobile.md`)

Screens are in `src.ios/MobilePackage` (`MobileKit` holds the protocols, `MobileUI` the screens,
`MobileCore` and `MobileLink` the real library, search and link over `src/mobile`). Two apps share
them. `MediaViewer` is the real one and links no stand-in data (`App/MediaViewer/LiveServices.swift`).
`MediaViewerFixtures` runs the screens on generated pictures and a generated clip; its
`MobileFixtures` package does not compile outside Debug, so it cannot reach a release. Backup
(release 1.1) is compiled only with the Swift condition `MV_BACKUP`, which no build sets.

Clips: AVPlayer opens MP4/MOV/M4V directly. Transport streams (`.mts`, `.m2ts`, `.m2t`, `.ts`) with
H.264 video play as HLS from a loopback server in the app (`MobileLink/TsStreams.swift`, parser and
segmenter in `MobileLinkSupport/MpegTs.swift`); iOS opens none of them as files. MPEG-2 video, HEVC in
TS, LPCM sound, MKV, WebM, AVI and MPG play when the server converts them (`docs/plans/phone-clip-convert.md`:
the service lists `convert`, and `MobileLink/ConvertedClips.swift` serves its 4 s segments through the same
loopback server); a server without it leaves them "can't play here", with the download. Folders show as
tiles with pictures from inside (`MobileUI/FolderTiles.swift`, `LibraryStore.preview`).

The real app's version, build number, bundle id and signing are in `src.ios/Config/MediaViewer.xcconfig`;
`DEVELOPMENT_TEAM` is empty there. Put your own in `src.ios/Config/Signing.local.xcconfig` (git
ignores it), for example `DEVELOPMENT_TEAM = ABCDE12345`. Build a Simulator copy with signing on
(the Keychain refuses an app without entitlements, and pairing needs it), and check a release
archive without a team:

```bash
cd src.ios
xcodebuild -project MediaViewerMobile.xcodeproj -scheme MediaViewer -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' build
xcodebuild archive -project MediaViewerMobile.xcodeproj -scheme MediaViewer \
  -destination 'generic/platform=iOS' -archivePath /tmp/MediaViewer.xcarchive CODE_SIGNING_ALLOWED=NO
```

`ITSAppUsesNonExemptEncryption` is false. The link uses standard algorithms (Noise over libsodium), and
the App Store Connect encryption answers (standard algorithms, not distributed in France) need no
documents. Before the app is offered in France, a French declaration has to be filed and those answers
changed. The Acknowledgements screen reads
`App/MediaViewer/Acknowledgements.txt`; after a dependency changes, regenerate it from the device
core build with `python3 tools/ios/acknowledgements.py build-ios-iphoneos src.ios/App/MediaViewer/Acknowledgements.txt`.

To walk the real app by hand, `tools/ios/demo-service.sh <folder> [desktop build dir]` makes a
small sample library, publishes it with `mv_publish_test_library`, runs `driftwire serve
--test-loopback` on 127.0.0.1:18452 (never the public DHT) and writes the pairing link and its QR
picture. Add the picture to the Simulator's Photos and use "Choose a picture", or open the link
with `xcrun simctl openurl booted "$(cat <folder>/pair-link.txt)"`. `demo-service.sh <folder> stop`
stops it. The code lasts 15 minutes and works once. With `DEMO_FILE_MODE=1` the share keeps only
`library.json`, so the phone opens the library in file mode (`docs/plans/phone-in-the-app.md` §4);
`mv_publish_test_library photos nas work 2` in that folder then publishes a catalogue with the same
key, which the phone takes on its next sync. File mode's core tests: `mv_mobile_link_tests "[mobile-files]"`.

The demo's `photos/Tapes` holds an AVI (MPEG-4 Part 2, MP3), an MKV (VP9, Opus) and an MPG (MPEG-2,
MP2) that the phone can't play itself. With the desktop build's `bin/MediaViewerClipJob` there, the
service converts them on this Mac's hardware encoder as the phone plays them (`DEMO_CONVERT=0` leaves
them as downloads). The conversion's own timings, with the real helper behind the real service:

```bash
MV_CONVERT_BENCH_HELPER=$PWD/build/bin/MediaViewerClipJob MV_CONVERT_BENCH_DIR=<folder of clips> \
  build/bin/mv_import_tests "[.convert-bench]"
```

The fixtures app:

```bash
cd src.ios
xcodebuild -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPhone 18 Pro' CODE_SIGNING_ALLOWED=NO build
```

Every state in the plan's §11 has a name (`FixtureState` in `FixtureStates.swift`). In the running
fixtures app the ladybug button lists them. To start in one, and without the button for screenshots:

```bash
xcrun simctl launch --terminate-running-process booted io.github.longtimeno-c.mediaviewer.mobile.fixtures -fixtureState library.stale -fixtureChrome NO
```

The Simulator has no camera, so the fixtures app's scan screen lists the codes it could have read.

### The `driftwire` command line (docs/design/28)

Every build that includes `cmake/driftwire.cmake` builds it: the Windows root, the Mac build and
`cmake/portable`. FTXUI (MIT) comes from vcpkg (`vcpkg.json` for Windows and macOS,
`tools/portable/vcpkg.json` for Linux), or from a checkout of its pinned release with
`-DMV_FTXUI_SOURCE_DIR`.

```sh
cmake --build build --target mv_driftwire_cli        # Windows / macOS: build/bin/<cfg>/driftwire.exe, build/bin/driftwire
cmake --build build-portable --target mv_driftwire_cli   # Linux, with the rest of cmake/portable
build/bin/driftwire --help
build/bin/driftwire                       # the session; Ctrl+C twice or /quit closes it
build/bin/driftwire send ~/Pictures/a.heic --to "Ann's MacBook"
build/bin/driftwire receive --once --save-to /tmp/in --plain
build/bin/driftwire update                # a build of your own: says it cannot, exits 1
build/bin/mv_driftwire_cli_gallery --width 80          # every card and state, made-up data (never packaged)
build/bin/mv_driftwire_cli_gallery --animate sending   # waiting, sending, receiving, bluetooth, internet, dht, relay, reconnecting, code
```

Its own data folder is `${XDG_STATE_HOME:-~/.local/state}/driftwire` (Linux), `~/Library/Application
Support/Driftwire` (macOS) or `%LOCALAPPDATA%\Driftwire` (Windows): settings, the resume file, the
DHT's table, the session lock and `driftwire_update.json` (the update check's switch and last
answer). Its identity and remembered devices are not there but under
"Driftwire CLI" in the Secret Service (Linux) or Credential Manager (Windows); macOS keeps none.
`mv_driftwire_cli_test` is the same program with `MV_DRIFTWIRE_CLI_TEST_HOOKS` (it runs from a
pipe, `--test-loopback`, `--test-data DIR`, `--test-events`), for the end-to-end test only.

The update check ([docs/design/28](design/28-driftwire.md#updates-in-the-terminal)) runs only in a
copy an installer put in place (`%LOCALAPPDATA%\Programs\Driftwire`, a Homebrew Cellar,
`/usr/bin` on Linux, or an `install.sh` archive prefix with `share/driftwire/install-manifest` on
Linux or macOS), so a build of your own
never asks the site and `driftwire update` refuses there. It reads `<site>/latest.json`; the site
is `MV_DRIFTWIRE_SITE_URL` (CMake cache, the Pages address by default) or `DRIFTWIRE_REPO_URL`
(https, or http on localhost). `DRIFTWIRE_NO_UPDATE_CHECK=1` skips it. Its tests are
`tests/test_driftwire_cli_update.cpp` and the session case "a newer driftwire is said once" in
`"[cli]"`, with a stand-in for the network.

On Linux the packaged program is built by its own small project, `cmake/driftwire-cli`
([Linux packages](#linux-packages-driftwire)). Without vcpkg (as in a container that cannot
download), a prefix with libsodium 1.0.21+ built `--disable-shared`, BLAKE3, Catch2 and FTXUI works:

```sh
PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig cmake -S cmake/driftwire-cli -B build-cli -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$PREFIX -DMV_DRIFTWIRE_CLI_TESTS=ON
cmake --build build-cli && ctest --test-dir build-cli --output-on-failure
```

Use GCC there (12 or newer): clang with libstdc++ rejects `core/json.h`.

#### Hosting from this computer (`driftwire host`, plan: `docs/plans/self-hosted.md`)

The service and a relay in one process on your own PC or Mac (slice 2; [docs/design/28](design/28-driftwire.md#on-your-own-computer-driftwire-host)).
Its folder is `<data dir>/host`; `--data DIR` uses `DIR/host` instead.

```sh
build/bin/driftwire host setup                               # the guided run: DuckDNS, a folder, the router, a phone's QR, the VPN
build/bin/driftwire host share ~/Pictures --label Photos     # what phones see; --backup lets them back up into it
build/bin/driftwire host                                     # runs until Ctrl-C: the service on 7878, the relay on 47614
build/bin/driftwire host status                              # from another terminal
build/bin/driftwire host start-at-login on                   # LaunchAgent / systemd --user / Scheduled Task, no admin
build/bin/driftwire host stay-awake off                      # a laptop: let it sleep
build/bin/driftwire host name duckdns                        # asks for the subdomain and the token (never arguments)
build/bin/driftwire host pair                                # a phone's pairing QR (needs a window ~100 columns wide)
build/bin/driftwire host vpn on                              # prints the relay line for the laptop's Relay (your own server)
build/bin/mv_import_tests "[host]"                           # loopback only; the supervisor case spawns mv_driftwire_cli_test
build/bin/mv_import_tests "[host-setup]"                     # setup, pairing redeemed, DuckDNS and router keepers, the VPN through the relay
build/bin/driftwire host pause                               # Hosting off (the server menu's toggle); resume, quit (start at login stays)
build/bin/driftwire host status --watch                      # usage today, phones connected, again on each change; Ctrl-C ends it
build/bin/mv_import_tests "[host-tray]"                      # the server menu, its numbers, the registry entry, the watcher, pause/quit on loopback
```

**Phone route bench** (the phone's reads from a host on this computer, VPN off and on alternated; one JSON
line per measurement, loopback, so run base and new alternated as the performance suite says):

```sh
MV_HOST_BENCH_MB=256 MV_HOST_BENCH_ROUNDS=4 build/bin/mv_import_tests "[.phone-host-bench]"
```

It reads a 256 MB file whole (8 MB window bulk, and 1 MB) and 200 ranges of 256 KB at random offsets,
from a session opened after each switch, and prints ms to the first byte, MB/s and p50/p95.

**VPN host bench** (a host's relay with the VPN on and the shipped forwarding, a laptop's Tunnel proxy with
its line, curl to a public URL through it and directly, alternated; this one reaches the internet):

```sh
MV_VPN_BENCH_ROUNDS=5 build/bin/mv_import_tests "[.vpn-host-bench]"     # MV_VPN_BENCH_URL to fetch another
build/bin/mv_import_tests "[nas-vpn]"                                     # the NAS as a VPN host, the code, Connect
```

While a host runs, MediaViewer shows its own server menu-bar item / notification-area icon
(plan "Tray"); the app must be running (the host's start at login also turns on the app's own
login item, hidden). A soak or harness run of the Mac app never starts the watcher.

Try the guided run without the internet or a router with the test build:
`printf '%s\n' ~/Pictures "" "" "" "" n n n | build/bin/mv_driftwire_cli_test host setup --test-data /tmp/hs --test-loopback`
(it never reaches duckdns.org or asks a router, and refuses start at login).

#### Users and the LAN share (`driftwire host users`, plan: `docs/plans/self-host-users-smb.md`)

```bash
build/bin/driftwire host users                               # the list: names and roles (no sign-in)
build/bin/driftwire host users add anna --role view          # asks the admin passphrase, then anna's password (echo off)
build/bin/driftwire host users role anna change --folders all
build/bin/driftwire host users password anna
build/bin/driftwire host users remove anna
build/bin/driftwire host users apply                         # a user still waiting for an administrator
build/bin/mv_import_tests "[host-users]"                     # stand-in OS port; loopback only
```

Each change ends in one elevated step (`driftwire host smb-apply`, never typed): sudo at a terminal
or the authorization dialog from the app on macOS, UAC on Windows. It makes sharing-only accounts
and SMB3-encrypted shares with no guest access, and never changes an account or share it did not
make. Linux says it is not offered yet (Samba later). The ports (`host_smb_mac.cpp`,
`host_smb_win.cpp`) have never been run against a real OS: the tests use a recording stand-in and
never make an account or a share. The Windows C++ only compiles in CI. `host_ports` refuses to
forward 137–139, 445 and 3389, a test proves it, and `host status` shows the share's addresses and
the users.

`start-at-login on` from a build of your own points the entry at that build; turn it off before
deleting the build (`host start-at-login off`). The test build refuses it. The running host asks the
router for its ports and keeps a DuckDNS name updated; neither has been run against a real router or
duckdns.org yet (the tests use stand-ins).

### The phone's library and search (`src/mobile`, `MobileCore`; plan: `docs/plans/mobile.md` M5, M6)

The real app links `src/mobile` through `tools/ios/build-core.sh`, a build phase that makes
`build-ios-<sdk>/` from CMake. It reads an installed generation from
`Application Support/library/gen/<epoch>-<generation>/`, the one `library/installed.json` names, with
the manifest's shared files beside `gen/`.

**Live, end to end.** `tools/ios/search-live.sh` runs the same comparison through the real app. It
copies an indexed folder's stills and short clips onto a disk image of their own (the "NAS"), points a
copy of the index at it, and publishes with the desktop engine's own `phone_publish` and the installed
AI pack. That is the code the Mac's Publish runs, so the catalogue carries the phone's real tower. It
then asks that engine every query (the Mac's answers), serves the image with `driftwire serve
--test-loopback` on 127.0.0.1, and pairs, copies and searches in the Simulator (`LiveSearchTests`).

```bash
tools/ios/search-live.sh "$HOME/Library/Application Support/MediaViewer/Add-ons/AI/data" <indexed folder> <Simulator UDID> <out>
```

Build the app for the Simulator ad hoc signed (`CODE_SIGN_IDENTITY=- CODE_SIGNING_REQUIRED=NO`) and
copy it to `<out>/MediaViewer.app`. Without a signature it has no keychain and cannot pair. The real
index is only read. The Mac's side runs with the index's own compute and quality (`MV_LIVE_CPU=1`:
the CPU at quality High), and needs every AI piece installed. It answers `place:`
with the Places add-on's own rule (`place_query`); clip routes are not walked. Tiles come from
ImageIO and AVFoundation, which cannot read every clip. With `MV_LIVE_SOUND=<folder>` (the output of
`tools/ios/clap_text_phone.py`, below) the folder is served on 127.0.0.1, a Debug app downloads sound
search's model from it (`MV_SOUND_BASE`) and the queries wait until Settings says Sound search is On;
without it the phone has no sound tower, and a query the Mac answers by sound differs. The script detaches its disk image on the way out (not with `MV_LIVE_REUSE=1`); afterwards delete
`<out>` (the image holds copies of the files, `data/` a copy of the index and faces) with
`hdiutil detach <out>/mnt; rm -rf <out>`. `tools/ios/small-screen.sh <out>` runs every screen on an iPhone SE
(3rd generation) Simulator at the largest text size.

The parity test runs the desktop engine, over the real AI pack, and the phone engine, in a Simulator
of its own, on one published generation, and compares rows and order:

```bash
cmake --build build --target mv_mobile_parity_desktop
ninja -C build-ios-sim mv_mobile_parity
build/bin/mv_mobile_parity_desktop publish <copy of the index folder> <root> <pack> <fetched> <places.mvgeo> <work> <nas>
tools/ios/run-parity.sh <nas>/.mediaviewer/gen/000001 <nas>/.mediaviewer <pack> <Add-ons> \
  build/bin/mv_mobile_parity_desktop build-ios-sim/bin/mv_mobile_parity tools/ios/parity_queries.txt <work>/queries.local.txt -- <out>
```

`publish` makes `queries.local.txt` from the catalogue's own names, places and cameras, so those
stay out of the repo. Numbers from this are Simulator numbers, not phone numbers. Both sides run
without the sound text tower by default; `MV_PARITY_WITH_SOUND=1` keeps it on the desktop side, and
`SIMCTL_CHILD_MV_PARITY_SOUND_DIR=<folder>` gives the phone engine sound search's model (a folder the
phone's download would have verified: the pinned files and `verified.txt`). `mv_mobile_parity` also
prints the process's physical footprint, the figure iOS holds an app to. The run fails below 200 queries, on a query listed twice, and when the
two desktop runs disagree. `place:` is covered only through the labels the catalogue stores: the
desktop app answers it through the Places add-on, which neither half loads. `RealCatalogueTests` drives the real app and is skipped unless
`TEST_RUNNER_MV_REAL_CATALOGUE` names a folder for its screenshots. MobileCore's own tests (generation
swaps, tiles and warm-up, over generations the publisher builds) run in a Simulator of their own:

```bash
tools/ios/run-mobilecore-tests.sh
```

### Sound search's model for the phone (plan: `docs/plans/mobile.md` §9, "Sound search")

The phone downloads the text half of the Mac's CLAP once, from the GitHub release `clap-text-phone-1`,
and keeps it only if each file matches the size and SHA-256 in `src/mobile/sound_tower.h`. To make the
files again (onnxruntime 1.30, onnx and numpy, in a venv of their own; it downloads the pinned 501 MB
fp32 export from Hugging Face unless `--fp32` names a copy), and print the pin lines:

```bash
python3 tools/ios/clap_text_phone.py --out <folder>
```

The same input gives the same bytes. A new export is a new pin: a new release tag, the lines in
`sound_tower.h`, and an app update; the old release stays for the builds that name it.

### The sample library (code `SAMPLE`; plan: `docs/plans/mobile.md` §11.1)

Every build of the iPhone app opens a sample library from the GitHub release `sample-library` when
someone enters `SAMPLE` on the Scan screen ("Enter a code") or opens `mediaviewer://sample`. It is
App Review's way in (`src.ios/AppStore/review-notes.md`). The pictures are listed with their authors
and licences in `tools/ios/sample-library/SOURCES.md` (made from `sources.json` by
`python3 tools/ios/sample_library.py table`).

To make it again, and to put it on the release (the second needs `gh` signed in to the repository):

```bash
cmake --build build --target mv_mobile_parity_desktop
```

```bash
tools/ios/sample-library.sh <work folder> <generation>
```

```bash
tools/ios/sample-library.sh <work folder> <generation> --upload
```

`<generation>` must be above the one on the release, or phones that have the sample keep theirs. The
script downloads each original from Wikimedia Commons (its SHA-1 pinned, its licence rechecked),
makes 2048 px JPEGs and 1280 px H.264 clips (the OS's encoder, no x264), indexes them on a disk image
of their own with the AI pack's B/32 on the CPU, publishes with `phone_publish`, refuses a
catalogue that names this Mac's home folder or user, and signs `files.json` (every original's and
the JPEG-512 pack's SHA-256) with `mv_mobile_parity_desktop sample-sign`. It signs with
`~/Dev/.mv-toolchain/devkey/sample-library-publisher.key` (`MV_SAMPLE_KEY`), whose public half is
pinned in `src/mobile/sample.h`: losing it means a new key and an app update, so keep a copy. The
upload puts the files first, then `files.json` (the list and its signature in one asset), then `current.json`. It leaves older generations' files on the release; delete them
by hand once the new one has been out for a day.

In the Simulator, with a Debug build ad hoc signed as for `search-live.sh`: the folder is served on
127.0.0.1 with ranges, the app is pointed at it (`MV_SAMPLE_BASE`, Debug builds only), and
`SampleLibraryTests` walks a wrong code, `SAMPLE`, the copy, six searches, a photo, a clip and Forget;
then the script checks each search's first three answers against the pictures' tags, that Forget
left nothing in the app's container, and that requests carried no header about the person:

```bash
tools/ios/sample-library-sim.sh <work folder> <Simulator UDID> <MediaViewer.app> <out>
```

`MV_SAMPLE_FILE=1` serves it as a `file://` folder instead. The core's side, over a folder of a
publisher's real `deliver()`, is in `mv_mobile_link_tests "sample*"`.

The same run checks phone backup (1.1) in the sample library: it grants Photos and adds 12 generated
plain-colour pictures to the Simulator (never anyone's own photos), `test4b` backs them up to "Sample
NAS on this iPhone", and the script checks that folder (each picture once, whole, no `.mvpart`, the
ledger copy there) before Forget and that Forget removes it. The backup core alone:
`mv_mobile_link_tests "backup:*"`; the service's write grant: `mv_import_tests "serve backup*"`.

Typing a search, recorded, so two builds can be compared doing the same thing (the grid flicker,
2026-11-12). `SearchSmoothnessTests` opens the sample library from the release if the app has none,
types three queries a letter at a time and refines each; the script records the screen
(`typing.mp4`), the time from each query's last key to its answer (`latency.txt`), the app's memory
afterwards (`footprint.txt`) and an Instruments trace. Build each app Release, ad hoc signed as
above, alternate the runs (base, new, base, new), and keep other work off the Mac: under load
XCUITest times out synthesizing keys. In the Simulator pass `none` or single instruments
(`"+SwiftUI,Hangs"`); the Animation Hitches and SwiftUI templates carry the Hitches instrument,
which runs only on a phone. The fixtures app's frame meter counts hitches while typing in the
Simulator: `ScrollHitchTests/testSearchTypingHitches`.

```bash
tools/ios/search-smoothness.sh <Simulator UDID> <MediaViewer.app> <out> none
```

## Run

```powershell
.\build\bin\Release\mediaviewer_lab.exe
.\build\bin\Release\mediaviewer_lab.exe path\to\photo.jpg
.\build\bin\Release\mediaviewer_lab.exe path\to\folder
```

| Key | |
|---|---|
| `F11` / `F` | fullscreen on the window's monitor; also available from View → Full screen. Hides the command bar and filmstrip; a clip's transport still floats and auto-hides, and the pointer hides with it over the video. `Esc` leaves |
| `F3` | frame-time overlay — off at launch on Windows and macOS unless a soak is running |
| Empty-window runner | `Space` starts/jumps/retries; `3` switches between the default 2D view and a shaded 3D view; `Esc` leaves with a short outro. Switching views keeps your run and score |
| `Space` / `Backspace` | next / previous. On a clip, `Space` is play/pause. It is no longer the lab sweep |
| `Home` / `End` | first / last in the folder |
| `PageUp` / `PageDown` | back / forward ten |
| `Ctrl+PageUp` / `Ctrl+PageDown` | previous / next page of a TIFF, PDF, DOCX or PPTX, size of an ICO, or image of a multi-image HEIC (`⌘` on the Mac). Within a page: zoom (wheel, `+` `-`, `1`–`4`) and drag to pan. On a PDF or DOCX with the filmstrip hidden the wheel / trackpad scrolls (a notch glides, a trackpad follows the fingers) and turns pages as soon as it pushes past an edge, momentum running on into the next page; `⌘`/`Ctrl`+wheel or a pinch zooms. With the filmstrip shown the notice line says "Press T to hide the filmstrip and scroll" until `T` has been used on a document |
| `Enter` on a PDF, DOCX or PPTX | open it in its default app (the bar's **Open in <app>**, ▾ for the others) |
| `Shift+T` | page sidebar on / off: page thumbnails down the left of a multi-page PDF, DOCX or PPTX while the filmstrip is hidden; a click turns to that page (remembered; View ▸ Page Sidebar on the Mac) |
| `J` / `K` / `L` | clip transport: −10 s / pause / +10 s ([docs/design/16](design/16-commands.md)) |
| `,` / `.` | frame step back / forward while paused |
| `A` / `D` | previous / next beside the arrows, in every mode including on a clip |
| `Q` / `E` | on a clip, two commands on one key: **tap** skips ±2 s, **hold** skims ±2 s per key repeat and settles on an exact seek when released. `Shift+Q` / `Shift+E` step playback speed. Off a clip they do nothing |
| `Left` / `Right` on a clip | walk the folder while the filmstrip is showing, paused or playing; with the filmstrip hidden (`T`) they are `Q` / `E` (tap ±2 s, hold skims). Stills always walk |
| `R` | reset the measurement window |
| `0` | fit to window |
| `1` | 100 % |
| `2` / `3` | 200 % / 400 % |
| `4` | fill: the image covers the canvas |
| `Ctrl+0` | reset pan and zoom (fit) |
| `Up` / `Down`, `Shift+arrows` | pan when zoomed in (`Left` / `Right` alone still walk the folder) |
| `S` | sticky zoom: keep zoom and position when you move to the next item (off by default) |
| hold `Z` | loupe: 100 % (or 2x the current zoom) around the cursor |
| hold `\` | show the previous image, for picking between burst frames |
| `\|` | two-up compare: the previous image beside the current one, panned and zoomed together (stills; `--compare` starts with it on) |
| `C` | clipping blinkies (red highlights, blue shadows). The canvas keeps presenting while on |
| `O` | info line: file name, position in the folder, size, zoom |
| `Ctrl+Shift+A` | always on top |
| `Insert` / `Shift+Space` | mark or unmark the current item. `Ctrl+A` marks all, `Ctrl+D` clears |
| Keypad `0`–`5` / `Ctrl+Shift+0`–`5` | rate the item on screen (0 clears) |
| `X` | reject the item on screen (rating −1, "Rejected" in Lightroom and Bridge); `X` again un-rejects |
| Keypad `6`–`9` / `Ctrl+Shift+6`–`9` | colour label Red / Yellow / Green / Blue (Lightroom's keys; `⌘⇧` on the Mac); the same key again clears it. `U` clears the label. Written as XMP `xmp:Label`: in a plain JPEG, else in its `.xmp` sidecar. The metadata pane's Label row has a swatch per colour |
| View ▸ **Show** | All / Photos / Videos / RAW / Marked: only that kind in the filmstrip, gallery and arrow keys, for this run (not saved). RAW is a RAW or a RAW+JPEG pair; Marked is the marks when you chose it. The stop on screen moves to the nearest one shown. Also in the command bar's View menu on the Mac. Below them, **Picks** (1★ or more) / **Not rejected** / **Red** … **Purple** filter on what `X` and the rating and label keys wrote (read on a worker; the stop on screen stays when you rate it; choose the filter again to re-apply). **Show next kind** cycles them all; it has no key until you give it one in Settings |
| `F5` | slideshow (fullscreen). In it: `Space` pause, `+` / `-` interval, `.` blackout, `R` shuffle, `Esc` leave. A clip plays to its end before advancing |
| `F7` / `F8` | copy / move the marked items (or the current one) to the last folder used. `Shift+F7` / `Shift+F8` pick a folder. Never overwrites: a taken name becomes `name (2).ext` |
| `Delete` | move the marked items (or the current one) to the Recycle Bin, after asking. A drive with no Recycle Bin is refused, never deleted permanently |
| `F2` | rename the current file (File ▸ Rename… on the Mac). Type the name without its extension; `Enter` renames. Its RAW+JPEG or Live Photo partner and its `.xmp` sidecars are renamed with it. A taken name becomes `name (2)`; nothing is overwritten |
| `Ctrl+P` | print (`⌘P`, File ▸ Print… on the Mac): the photo on screen with its edits, fit or fill; with two or more marks, a contact sheet (2×2, 3×4 or 4×5, file names optional). The options are in the print dialog |

Drop files or a folder on the window, the gallery, or the filmstrip. Drag a
thumbnail (gallery, filmstrip, or a Local search result) or the current image
(at fit) out to Explorer or another app, such as a video editor: the drag
carries the original file itself (`CF_HDROP` on Windows, `public.file-url` on
the Mac), copy-only and read-only, never a copy of it; on Windows a thumbnail
brings its RAW / Live Photo pair. Dragging a marked thumbnail drags every
marked item, in folder order; an unmarked one drags itself (both platforms).
A drag of ours let go over our own window, gallery or filmstrip is refused
rather than reopening the folder. Pass paths on the command line: the first
one that exists opens (a file opens its
folder with that file selected).
Files added to or removed from the open folder show up without a restart.

A one-pixel grid appears at 400 % and above.
| `+` / `-` | zoom in / out (`=` and the numpad keys too) |
| `Ctrl+O` | open a photo or a clip (JPEG/PNG/BMP/GIF/WebP/TIFF/ICO/HEIC/AVIF/RAW, MP4/MOV/MKV/WebM/AVI/TS/MTS/MPG) |
| `Ctrl+Shift+O` | open a folder |
| `Ctrl+E` | show the current file in Explorer, selected (`⌘E`: Finder; a Photos library item opens in Photos, selected). Open menu: **Show in Explorer** (Mac: **Show in Finder**, or **Open in Photos** while a Photos library item is current; `?` and Settings use the same words) |
| Open ▸ **Recent folders** | the folders you opened lately, the jump list's (File ▸ Open Recent on the Mac); pinned folders first (#617: right-click a welcome-card row to pin or unpin). From the keyboard: `Tab` to the command bar, `Enter` on Open, arrow to Recent folders, `Right`. A folder that has gone beeps and leaves the list, as a welcome-card row does. The welcome card writes your profile folder as `~` (`~\Pictures`). With OneDrive set up, a **OneDrive** row leads the card, above Recent folders, as iCloud Photos does on the Mac: it opens OneDrive's Pictures folder (OneDrive itself if there is none) |
| `Space` / `,` / `.` on an animation | play or pause (a finished one plays again) / previous frame / next frame, like a clip. Delays follow browsers: 10 ms or less plays as 100 ms |
| `?` | the shortcuts for what you are doing right now. Also the `?` button on the right of the command bar |
| `Ctrl+,` | Settings: **General** has grouped preferences with aligned switches and automatic saving; **Keyboard shortcuts** has the searchable remapping list. Both pages scroll independently of the header and Done button. Search the list by command or shortcut. Choose a shortcut and press its replacement; viewer shortcuts are suspended while Settings is open. Escape or Cancel change cancels capture; Escape otherwise closes Settings. Conflicts swap shortcuts, and Reset to default restores the map. `?` lists whatever you bind |
| `Ctrl+G` | go to an item by its number in the folder (Windows; not on the Mac yet) |
| `/` | find an item by name. With the filmstrip or gallery focused, just type |
| `I` | metadata pane: summary card, searchable tag tree, and for clips the per-stream inspector (PR 9). Focuses the pane; `Esc` returns to the photo, a second `Esc` closes it |
| `Ctrl+Shift+E` | folder tree, rooted at the open folder (PR 9). Focuses it: arrows walk, Right / Left open and close a folder, `Enter` opens it, `Esc` returns to the photo. It follows the folder as subfolders come and go |
| `O` / `Shift+O` / `Shift+I` | info overlay with exposure lines / AF points / eyedropper (PR 9) |
| `Shift+Z` | zoom to the AF point: 100 % centred on the in-focus point, for a focus check (issue #209) |
| `Ctrl+C` | copy the eyedropper colour if it is on, otherwise the marked (or current) file(s) |

The title bar shows the current file, its position in the folder, its size and
the zoom. Arrow keys, `Space` and the slideshow wrap from the last item to the
first; turn that off under Settings.
| `Ctrl+Shift+O` | open a folder |
| `Left` / `Right` | previous / next in the folder |
| `G` | gallery: thumbnail grid of the folder. A folder of only folders uses big tiles; a mixed folder keeps a short chip row above the photos. Covers and counts show on the tiles; the folder path sits in the command bar just left of `?`. The Open, View, Settings and `?` buttons keep their width; the trail is what shrinks and scrolls. **Up** (↑) and **Root** (house) buttons stay outside the scrolling trail: Up opens the enclosing folder; Root returns to the highest folder reached in this browsing session (the first breadcrumb). The `…` menu opens hidden parent folders directly. Full paths are available on hover. These controls remain available, including while a photo is open. `Ctrl+Up` goes up and selects the folder you left; `Ctrl+Left` / `Ctrl+Right` open the sibling beside it. A search icon at the right end of the path (and beside "Search: …" on a result list) does what `Ctrl+F` / `⌘F` does: with Local search installed it opens the search panel (the first search of a session starts the pack, which loads nothing at launch) (where `file:name` searches file names); without it, **file search**, a field over the grid that filters this folder's tiles by name as you type (no index; `Esc` clears, then closes); the command bar stays above the gallery on both hosts, so the path and the icon are there over the grid too. `/` on the folder row finds a tile by name. `W` / `S` or Up / Down move between rows and cross from folders to images; `A` / `D` or Left / Right move between items. `+` / `-` enlarge / shrink thumbnails (`=` also enlarges). `Enter` opens a folder or the selected image. A click does the same; `Esc` leaves |
| `T` | filmstrip show/hide, for the mode you are in (folder open or single image) |
| `Tab` | focus the command bar island |
| `Esc` | walks out one level: gallery, fullscreen, then island focus back to the canvas, and last, in a search result list, **Back to folder**. It never quits |
| `Ctrl+W` / `Alt+F4` | close the window |
| `Ctrl+N` (`⌘N`) | a new window: its own process on the empty window; add-ons stay in the first window's process, except those that only fill screens and slots (Places), which every window loads (#402) |

**Editing (PR 10, Windows and macOS).** Edits are kept per file for the session; the
original is only ever rewritten by a lossless JPEG rotate / flip.

| Key | Does |
|---|---|
| `[` / `]` | rotate left / right. On a JPEG with no other edit, the file is rewritten losslessly 0.4 s after the last press |
| `H` / `V` | flip horizontal / vertical (same lossless rule) |
| `Shift+C` | crop / straighten. Drag the rect's handles (or inside it) on the photo, or arrows move the crop and `Shift`+arrows resize it; `,` / `.` straighten by 0.5°, `Enter` applies, `Esc` cancels. Tests: `mv_tests "[crop-drag]"` |
| `Ctrl+Z` / `Ctrl+R` | undo the last edit / reset to the original |
| `Ctrl+S` | export dialog: JPEG / PNG, quality, long edge, metadata (all / no GPS / none). `↑` `↓` choose, `←` `→` change, `Enter` exports to `<name>-edit.jpg` beside the original; never overwrites |
| `Shift+A` | adjust pane (PR 11, grown by [edit-adjust](plans/edit-adjust.md) slice 1): Light (exposure, highlights, shadows, whites, blacks, contrast), Colour (saturation, vibrance, temperature, tint), Black & White and Vignette sections, each with an Auto (not the vignette) and a reset, histogram and clipping. It takes the keyboard: `↑` `↓` pick a slider of an open section, `←` `→` step it (`⇧` ×10), `0` zeroes it, `A` runs its section's Auto, `R` resets all. `Esc` hands the keyboard back to the photo; `Shift+A` again closes the pane. A slider drag is one `Ctrl+Z`. Colour never rewrites the file: a JPEG with a colour edit keeps `[` `]` in the stack for export |

**Clips (PR 13 / 14, Windows and macOS).** Nothing here changes the
clip: every result is a new file beside it, and a cancelled job leaves nothing behind.

| Key | Does |
|---|---|
| `Ctrl+T` | trim mode on the clip on screen. Transport keys keep working; `Esc` leaves it (markers are kept for that clip) |
| `[` / `]` | in / out marker at the playhead. The scrub bar shows the markers, the keyframe grid and the range the instant cut keeps |
| `P` | preview the cut: loops exactly what `Enter` will write |
| `Enter` / `Shift+Enter` | save the keyframe cut (instant, lossless, snapped to keyframes) / the frame-accurate re-encode on the hardware encoder (slower) |
| `Ctrl+X` | save a copy without the in–out range |
| `Ctrl+←` / `Ctrl+→` | previous / next keyframe (in trim mode) |
| `Backspace` / `Delete` | clear the markers (in trim mode `Delete` never trashes the clip) |
| `Ctrl+S` | clip tools: rotate losslessly, split, save the frame (PNG / JPEG), extract audio (copy / WAV / FLAC), convert MP4 ↔ MKV, GIF / WebP of in–out (or 5 s from the playhead) |
| `Ctrl+B` | split at the nearest keyframe to the playhead |
| `Ctrl+J` | Jobs pane: progress and time left; `↑` `↓` choose, `Delete` cancels, `R` retries, `Enter` shows the output in Explorer / Finder |

On a Mac these are the `⌘` chords.

Colour edits are worked in linear light (an FP16 working image, D6) and shown through the
same shader on both platforms; `C` blinks what the edit clips. A RAW's sliders wait for
LibRaw's full linear develop (seconds on a large file) rather than editing the embedded
JPEG, so what you adjust is what exports.

Keys go through one router and one table (`src/shell/commands.h`,
[docs/design/16](design/16-commands.md)). Symbol keys (`?`, `+`, `\`) follow your
keyboard layout, not a US key position.

Canvas tools (docs/plans/edit-refresh.md S3, [docs/design/20](design/20-edit-workspace.md)
"Canvas tools"): `B` on a still arms the Remove brush; red-eye is on the Edit rail, the Adjust
pane and (Mac) Edit ▸ Red-Eye. While one is armed a bar below the photo shows its controls, `[`
`]` size the ring (Alt / ⌥ + wheel too), Alt / ⌥ held erases, `Space` held pans, `Ctrl/⌘+Z`
undoes a stroke or fix (`Ctrl/⌘+Shift+Z` redoes; Clear is undoable too), `Enter` removes, `Esc`
puts the tool down. Rotate, flip, crop and Delete are refused meanwhile, and `A` / `D` while strokes
are pending; rotate and flip stay refused while strokes are kept with the tool down. Tests: `mv_tests
"[canvas-tool],[remove-brush],[tool]"`; the brush's snapshot publish and tint build time:
`mv_tests "perf: the brush's snapshot publish and tint build"` (in `"[.perf-bench]"`, S4a). Red-eye's snap and Find eyes need the AI pack with
People on. **Extend** (S6; the Enhance tab's card, Edit ▸ Extend Edges): drag the frame's
edges past the photo (25 % a side warns, 50 % stops), chips for the add-on's frames and a
Ratio menu with Crop's ratios and its portrait swap (drags then keep the ratio), `Enter`
fills (the part over the photo is a crop; Enhance fills the rest at once, one click), `Esc`
cancels. Tests: `mv_tests "[extend]"`, `mv_import_tests "[enhance][extend]"`. **Compare** (the Edit header's
and rail's; [docs/design/20](design/20-edit-workspace.md) "Compare"): a before/after split with a
draggable divider, the before being the photo with no edit or, on an Enhance result, its source
photo; `Y` held shows the before alone; disabled, with the reason, when there is nothing to compare.
Tests: `mv_tests "[compare-source]"`.

Faces (S8): the rail's Faces, the Enhance tab's Faces card or (Mac) Edit ▸ Faces arm the Faces
tool: a box per face on the photo (a click chooses it or leaves it out) and the Faces panel first
in the Enhance tab (each face's thumbnail, People's name, its Open eyes / Improve face, Do all).
It needs the AI pack with People on; without them the panel says what to turn on. Results are new
files beside the photo (`-eyes.jpg`, `-faces.jpg`). Tests: `mv_tests "[faces-tool],[eyes]"` (the
cards, choosing, the boxes, shut-eye detection, the edit region) and `mv_ai_tests "[faces-mask]"`
(only the chosen faces change: 0 pixels differ outside their boxes; the canvas's boxes matched
to the run's answer) and `mv_ai_tests "[eyes]"` (the pack's names and counts).

Wheel zooms toward the cursor; drag pans. Zoom-out floors at 50 % (Fit can
still go smaller on a huge image) and rubber-bands a little past that, then
springs back to centre. Drop a file on the window. A pinch (touchscreen,
touchpad, trackpad) zooms like the wheel; a sideways swipe or mouse tilt steps
next / previous; a finger drag pans once zoomed
([docs/design/16](design/16-commands.md) "Touch and trackpad"). The gesture
maths has unit tests (`mv_tests "[gesture]"`); real touch hardware is not in
any automated run.

Command line: `--soak <seconds>`, `--json <path>`, `--gate` (non-zero exit if the verify
line fails), `--no-overlay`, `--static`, `--no-chrome`, `--open <path>`, `--boot-report <path>`
(#486: write the boot timeline as JSON once the launch is ready and close; leaves the recent
folders alone; on Windows it implies `--new-instance`, on the Mac it shows the window without
taking the keyboard, as the soaks do; see "Performance suite"), `--background` (#452:
the sign-in start: the window is never shown, only the tray / menu-bar icon once Driftwire keeps
the process; it quits after 60 s if nothing does),
`--browse-soak` (with `--json` and a folder: time each arrow to the next still, and jumps
past the prefetched neighbours; 3 s dwell between arrows; the macOS lab has it too, and takes
`--edit-tab N` to walk with the Edit workspace open on tab N, 5 being Enhance, whose cards measure
each photo for their time estimate. `--work-glow` (macOS lab): open the Edit workspace on the first still and hold
one-click Enhance's working glow on, to look at it or to run `--soak 60 --gate` with it up. The
Windows lab adds `quick` steps — Right as soon as the last photo is on screen — and one
`held` run — a Right every 50 ms, then the last photo timed to full resolution, with the
run's decode job counts under `"held"`; every step carries `full_ms`. Those are the cases
the in-flight decode hand-off is for, and `perf-browse.svg` ignores them),
`--page-soak <path>` (macOS lab, issue #429: with a PDF, DOCX or PPTX, scroll the page sidebar
from the first page to the last in 1.5 s, as a hard flick does, then go to pages 250, 10, 150, the
last, 1, 250 and 100 the way a thumbnail click does, each timed to its first image, the present
showing it and the full-size page; add `--soak N` to have the canvas pacing measured meanwhile),
`--av-soak <seconds> --csv <path>` (headless A/V drift soak on a clip — see
[Test](#test)), `--pan-soak` (with `--soak` and `--open`: pan the still at 100 % across
the whole frame on a fixed path, to measure cached-image and tiled-pyramid pan),
`--pane-drag-soak` (both labs, docs/plans/edit-refresh.md S1: with `--json PATH` and a still, not
`--soak`: opens the Edit workspace and drags the right column's edge 280 ↔ 720 and back every
4 s for 20 s through the drag's own code, while the still pans at 100 % for 10 s and then refits
live. The JSON has the present intervals during the drag (`frames`, `p50_ms`, `p99_ms`,
`max_ms`, `over_2x`), the UI thread's layout time per step (`layout_p95_ms`; Windows also says
`guide_line_fallback` when that is over 12 ms), the Mac's drawable-size assignments during the
drag (must be 0) and `presents_after_settle` (2 s after a 1.5 s settle, must be 0); `pass` is
none over 2× refresh and 0 presents after settle. Compare its `p99_ms` with a base `--pan-soak`
run's by hand. The width it drags to is never saved),
`--compare` (start in the two-up compare; add it to `--pan-soak` to pan both halves, the
left one drawing the same still again since one file has no previous, or to `--browse-soak`
to walk a folder with the split up; `compare_frames` in the soak JSON counts the two-half
presents), or a positional file or folder.

A still's first pixel is a preview (JPEG DCT 1/4, or a RAW's embedded JPEG); the full
decode then *refines* the same item rather than replacing it: the view you have — fit,
or a zoom and pan made while it loaded — is kept as a fraction of the image, and the
full texture cross-fades in over 80 ms (up to 250 ms when the preview and the full
render differ a lot in brightness, as a RAW's embedded JPEG and LibRaw's render do).
Images above ~64 MP, or wider or taller than 16384 px, are shown as a tiled pyramid: a
≤ 2048 px overview is always loaded, and 256 px tiles for the part on screen are
created a few per refresh and drawn over it, so a fast zoom or pan is blurry for a
moment, never blank. `F` / `F3` shows the tile LOD, resident tiles and VRAM, create
times, and refinement counters. Its `texture` line times the whole-texture creates of
untiled stills and counts the neighbour creates held back while the camera moved (issue
#166). The soak JSON carries the same numbers: `texture_*` keys, including
`texture_creates_deferred` and `texture_max_defer_ms`. The command bar is also on the island: Open (**Media…** or **Folder…** — the picker
takes photos and clips), View (zoom in/out,
fit, 50 / 100 / 200 / 400 %, gallery, filmstrip, overlay),
a `?` shortcuts button, Settings (`Ctrl+,`: view defaults and remappable keys), About.
The filmstrip along
the bottom and the gallery grid are both `ItemsRepeater` islands over the same listing; the gallery also shows child-folder tiles (or a chip strip in a mixed folder) and a path bar (`Ctrl+Up` goes up, `Ctrl+Left`/`Ctrl+Right` the sibling). Thumbs are JPEG files from `%LocalAppData%\MediaViewer\thumbs`. Clips get a
thumbnail too — a poster frame from about 10 % into the clip, in that same cache — so a
camera dump does not show blanks where the video is. With no
folder open the canvas shows a welcome card (drop target, open shortcut, formats, key legend) reading *Drop photos, videos or a folder here*, not the
present-lab sweep. **Space** on that empty view starts a small runner game (an intro animation, then
Space to jump, `Esc` to leave with a short outro); it stops presenting once the welcome card is back. The frame-time
soak (`--soak`) keeps the old sweep, which the present-loop gate measures. The gallery and filmstrip accept the same drop, and you can drag a
thumbnail or the fitted image out to Explorer.

The playback transport is a **third island**: a centred bar floating over the bottom of the
video, above the filmstrip, that appears with a clip and goes away with it. As on the Mac it is
44 DIPs tall and up to 720 wide: back / play / forward, elapsed time, a scrubber that takes the
spare width, duration, the speed menu (`1x`), mute and More. It works the
same way on Windows and macOS (`src/shell/transport_autohide.h` holds the one rule both use):

- While the clip **plays**, the bar hides after 2.5 s with no activity. Moving the pointer,
  clicking, scrolling, a transport key (`Space`, `K`, `J`/`L`, `Q`/`E`, `,`/`.`, speed, mute,
  `↑`/`↓` volume) or `Tab` brings it straight back.
- It **stays up** while the clip is paused or has ended, while the pointer is over it, during
  a scrub, while its More / speed menu is open, while it has keyboard focus, and whenever a
  screen reader (Narrator, VoiceOver) is running. It is never shown over the gallery or
  Settings.
- In **fullscreen** the pointer hides with the bar while it is over the video. Windowed, the
  system pointer is never hidden.
- Hiding is **only visual**: the canvas does not refit, decode and audio carry on, and keys
  keep working. A click where the hidden bar was lands on the video, not on a button you
  cannot see. Idle playback adds one one-shot timer, not a repaint.

The Mac bar fades; on Windows the island is an opaque child window, so it is moved
off-screen instead. The bar covering the video bottom is a reversal of the old reserved
strip ([docs/design/12](design/12-decision-log.md) 2026-09-26). Speed is owned by the core, so the
dropdown and the keyboard cannot disagree.

Opening a single image lists its folder too, so `Left` / `Right` and the gallery work on the
files beside it. Whether the filmstrip comes with it is a preference: **Settings** has
*Filmstrip when opening a folder* (on by default) and *Filmstrip when opening an image* (off),
persisted to `%LocalAppData%\MediaViewer\settings.ini` (also wrap, sticky zoom, canvas
background, and key remaps). `T` toggles the one for the mode you are in. Both take
effect immediately — no restart. The Settings screen groups these defaults under General,
with switches aligned on the right and descriptions on the left. Keyboard shortcuts has
its own searchable tab, so neither page is squeezed into a narrow column. Both platforms
keep the header and Done button visible while the content scrolls.

For the settings and path-bar smoke checks on Windows and macOS, see
[the UI verification checklist](settings-navigation-verify.md).

### Local search (the AI pack) from a source build

Developer builds only; a release build refuses a dev-signed pack. Numbers, state and what is
owed: [docs/design/17](design/17-local-ai-search.md); the Mac checklist is
`src.swift/AIChrome/MAC-VALIDATION.md`.

1. Stage the models (pinned revisions and SHA-256s):
   `python3 tools/package/ai-models.py stage --piece ai --out <staged>/ai` (and `ai-audio`,
   `ai-faces`, `ai-large`). Staging `ai` also writes `models/tiers.json`, the model tiers the
   add-on reads (#482, [docs/plans/model-tiers.md](plans/model-tiers.md)). A pack staged
   before that has no such file and opens the Default tier's folders. The default,
   `--upstream bundle`, stages every file (a sideloaded dev pack carries its models); releases
   pass `--upstream remote`, which lists upstream files in `remote.json` for the add-on to
   fetch from Hugging Face on install. Staging the ViT-H/14 (`ai-large`) and AdaFace
   (`ai-faces`) exports needs `requirements-export.txt` (torch, transformers); pass
   `--skip-exports` to leave them out. A sideloaded pack starts with no tier chosen: pick one
   in the picker, or write `{"tier":"default"}` to the add-on's `data/settings.json`.
2. Configure with `-DMV_ADDON_DEV_PUBLIC_KEY=<your key's public half>` and build `mv_ai`,
   `mv_ai_chrome` (Mac) or the WinUI chrome, `mv_ai_tests` and `ai-bench`. On the Mac the AI
   targets exist on arm64 only.
3. Pack, sign and install into a scratch folder:
   `python3 tools/package/ai-sideload.py --build <build> --models <staged> --key <key file>
   --addons <folder> --platform win-x64|macos --pieces ai,ai-audio,ai-faces`.
   NVIDIA acceleration (Windows): configure with `-DMV_AI_CUDA_PIECE=ON` to stage ORT's CUDA 13
   build in `build/addons/ai-cuda`, and add `ai-cuda` to `--pieces`. It needs NVIDIA's CUDA 13
   and cuDNN 9 on `PATH` (never shipped; docs/design/12 2026-10-03); without them the panel says
   "CUDA 13 or cuDNN 9 not found — using CPU". Stable releases publish this piece.
4. Run the app or `ai-bench --addons <folder> --index <media folder> --query "a dog"` with
   `MV_DEV_ADDONS_DIR=<folder>` and `MV_DEV_THUMBS_DIR=<another folder>`, so a real install
   is untouched. `--documents` turns on File types "Show documents" for the run, so the
   folder's PDFs and DOCX files are read for their words (#354); the status lines then carry
   `"docs":[done,total,locked,failed]`. The pack stages PDFium beside `mv_ai` from its pinned
   download (`cmake/ai.cmake`); set `MV_ORT_ARCHIVE_DIR` to a folder holding
   `pdfium-<platform>-8066.tgz` (and `microsoft.ml.onnxruntime.1.30.0.nupkg`) to configure offline.
5. Quit's cost with a pack loaded: `ai-bench --addons <folder> --query dog --quit-after 1`
   quits the way both hosts do (the pack gets half a second to stop, then the exit skips static
   destructors; `src/addon/host.h`, "Quit") and prints the time it took; `--quit-legacy` is the
   old wait of up to 5 s, `--quit-hash` re-verifies the pack meanwhile as Settings does, and a
   larger `--quit-after` quits once the pack is idle.

**Indexed folders are watched** (docs/design/17 "The host watches each folder root"). New,
removed and moved-in files are found without a rescan. Tests, no pack needed:
`mv_tests "[tree_watch]"` (the host's `io::tree_watcher`, on real files) and
`mv_ai_tests "[watch]"` (the engine through the real host table: a file added three folders
down walks only its folder; removed files and folders moved in or out; a paused folder is not
watched; a Photos asset that came only in iCloud this session indexes once Photos downloads it).
In the app, with a sideloaded pack: index a folder, copy a photo into a subfolder of it, and
search for it within a few seconds without opening that folder.

**An unfinished index waits for Resume** (issue #299; docs/design/17 "Unfinished roots wait for
Resume"). A root left mid-index is held at the next start, and the window asks "Resume indexing
…?" in a bottom-right notice without loading the pack. Tests, no pack needed:
`mv_ai_tests "an index left unfinished waits for Resume after a restart"` (the engine: held,
`resume.json`, `resume_root`) and `mv_import_tests "[ai-resume]"` (the host's read of
`resume.json` and the question's wording). To see the notice in the app, use a sideloaded pack
(above): index a large folder, quit part-way, start again. The notice should appear once the pack
has verified, with nothing of it loaded (no ONNX Runtime in the process) until **Resume**. Or,
with the pack installed, drop a `resume.json` by hand into its data folder (the one that holds
`index.db`, `data` beside the pack's version folder):
`{"version":1,"roots":[{"id":1,"path":"/some/folder","kind":"folder","done":1,"total":2,"held":true}]}`.

`mv_ai_tests "[refine]"` runs the People refinement (docs/design/17 "People refinement") on synthetic
face vectors and a temporary faces.db; it needs no pack. In the app it runs only from
**Refine faces** on a person under Settings → People, and library-wide from **Merge
duplicates** above the grid (docs/design/17 "Merge duplicates": the same check for everyone, then people
who are the same person merged; never two named differently, never a split pair).

The People embedder is AdaFace IR-50 (docs/design/17 "People model"). It has no upstream ONNX: staging
`ai-faces` runs `tools/package/face-export.py` on the pinned safetensors, so the pack builder needs
`pip install -r tools/package/requirements-export.txt` (torch, onnx); `ai-models.py check` does
not. **Re-analyse faces** (Settings → People;
`people_reanalyse`) runs every photo through the People pass again and keeps the user's people;
a pack with a new face model does the same by itself. Tests: `mv_ai_tests "[rerun]"` (no pack);
`"[people]"` with `MV_AI_PACK_DIR` (the ORT folder), `MV_AI_FACES_DIR` (a staged ai-faces) and
`MV_AI_FACE_SET` (a `<person>/<photo>` folder such as LFW, kept out of git); and
`"[.people-bench]"` with `MV_FACE_EVAL` (a vector file: uint32 n, uint32 dim, n int32 labels,
n x dim float32) and optionally `MV_FACE_TUNING="same keep keep_weak margin ambiguous merge"`,
which runs real vectors through faces.db's online clustering and the re-run's settle and prints
BCubed precision and recall. Then a corrections phase: after 60 % of the faces, it merges each
person's fragments and names everyone with 3+ photos, and files the remaining 40 % online
(`corrected`, `new 40%`).
LFW has only clear faces, so check a clustering or People-speed change on a real library too:
copy its `faces.db` (`sqlite3 "file:<faces.db>?mode=ro" ".backup <copy>"`; opening migrates, so
never the live one), then `MV_FACES_DB=<copy> mv_ai_tests "[.people-settle]"` (the settle, with
people listed before and after, and the counts that must be 0: faces filed where they were
rejected, rejections of what is gone, two people of one frame in one person) or `"[.people-api]"`
(what each People read the chrome makes costs, the sheet's review, suggestions and unknown faces
included, then the copy's unfiled faces offered again through `add`: ms per face and where they
went; it writes, so use a fresh copy per run) or `"[.people-heldout]"` (a tenth of the named
people's clear faces taken out and filed again: how many come back to their person;
`MV_HELDOUT_WEAK=1` the weak ones, `MV_HELDOUT_SETTLE=1` then settles), `"[.people-unknown-order]"`
(the unknown-faces queue's right and wrong one-click suggestions on held-out faces) or
`"[.people-why]"` with `MV_PEOPLE_NAME` (a named person's settle verdicts, tallied). Compare
with the base commit's `mv_ai_tests`
built in its own worktree, runs alternated. Delete the copy after: it is face data.

The Faces tool's eyes (docs/design/20, *Eyes closed*): `mv_tests "[eye-state]"` checks the C++
eye model against its ONNX original; `"[.eye-state-bench]"` with `MV_EYE_STATE_LIST` (lines of
`"path" x0 y0 x1 y1`, the eye centres in pixels) prints each eye's reading on real photos. A fix on
a real photo with the pack's own detector: `mv_ai_tests "[.faces-real-bench]"` with
`MV_AI_PACK_DIR`, `MV_AI_FACES_DIR` (a folder with `models/faces`: `model.json`, `yunet.onnx`,
`adaface.onnx`), `MV_FACES_PHOTO` (a copy: the result is written beside it), `MV_FACES_PACK` (a
saved eyes_json answer), `MV_FACES_FIX`, `MV_FACES_MASK`, and `MV_FACES_MOMENT_OF` (the photo's
real path) to add its same-moment frames as the pack would. The eye model's weights are
regenerated with `tools/eyes/export_eye_state.py` (needs the `onnx` package).

The People grid (Settings → People, on both platforms) follows the folder
the viewer has open: **People in · This folder | + Subfolders**, + Subfolders by default;
everyone shows only when no folder is open (docs/design/17 "People in the open folder", amended
2026-10-03; `people_in_json` in `mediaviewer_ai.h`).
`mv_ai_tests "[faces]"` covers the scoping with the fake face model.

Tests with the real pack: `MV_AI_PACK_DIR`, `MV_AI_AUDIO_DIR`, `MV_AI_SPEECH_CLIP`,
`MV_AI_EVAL_DIR` (a folder of photos plus a COCO-style `labels.json`) and `MV_AI_GREY_JPEG`
make `mv_ai_tests` run its model cases; `"[.bench]"` prints CPU / Core ML timings and
`"[.calibration]"` the held-out "nothing found" rates and, per Precision level (Settings →
Local search), what captions, nonsense, "helicopter" and near-miss category queries return
(docs/design/17 "Precision scale"). By default it indexes 300 and 1,000 photos per tower; for library
sizes (issue #85) give `MV_AI_CALIBRATION_SIZES=1000,5000,10000,25000` and an eval folder
with that many labelled photos (COCO 2017 val2017 then train2017 by image id, captions as `labels.json`), and
`MV_AI_CALIBRATION_TOWERS=clip-b32` (or `clip-l14`) to run one tower. The image tower runs on
Core ML on a Mac (about 500 img/s B/32 and 25 img/s L/14 on an M-series, CUDA with `MV_AI_CUDA_DIR`, CPU otherwise;
`MV_AI_CALIBRATION_CPU=1` forces CPU), decoding on several threads. Set
`MV_AI_CALIBRATION_CACHE=<folder>` to keep the embeddings (and Core ML's compiled towers)
between runs: the cache is append-only, so a stopped run resumes, and one made for 25,000 photos
serves every smaller size. `MV_AI_CALIBRATION_OUT=<folder>/{tower}.jsonl` writes one line per
tower, size, level and query (rows returned, relevance, and the rule's inputs: `top10_z`, its noise
expectation, `over_margin`, the best margin, `stands_out`, `margin_needed`) and, beside it, every
query's embedding (`queries-<tower>.f32` / `.json`) for replaying a candidate rule offline; the
binary formats are described above the test in `tests/test_ai_infer.cpp`. The rule's label vocabulary
(`src/addons/ai/vocabulary.*`) is on by default; `MV_AI_CALIBRATION_NO_LABELS=1` measures the rule
without it, and `MV_AI_CALIBRATION_VOCAB=<file>` (with `MV_AI_CALIBRATION_OUT`) only embeds that
file's lines as `vocab-<tower>.f32` for trying a list offline. The engine keeps its tower's label
embeddings in `labels.f32` in the AI data folder; delete it to re-embed. On a Mac the first Core ML
open of a tower compiles for 1–5 minutes; the app searches on CPU meanwhile.

Settings → Local search → **Import and export** writes the index of chosen folders to a
`.mvindex` file (SQLite; paths relative to each folder, optional People and cached thumbnails)
and merges one back, each folder pointed at where its files are on this machine; the folders are
then rescanned and anything whose size or date differs is indexed again (docs/design/17 "Sharing an
index"). `mv_ai_tests "[transfer]"` runs two engines as two machines over one library copied
elsewhere: nothing embedded twice, an edited file re-embedded, an empty index adopting the
file's Quality, a used one skipping another model's vectors, People and thumbnails only when
ticked, a non-index file refused. Thumbnails cross the host table as bytes (`thumbnail_jpeg`,
`thumbnail_store_jpeg`, appended to v2; the host decodes what it stores). An export with
thumbnails makes any the viewer's cache does not hold yet, so it can take a while on a library
that was never browsed.
With the real pack, two sideloaded add-on folders are two machines:
`ai-bench --addons <A> --index <library> --make-thumbs <library> --export <file> --export-flags 2`,
then `ai-bench --addons <B> --import <file> --import-to <copy elsewhere> --import-flags 2
--count-thumbs <copy> --query …` (each with its own `MV_DEV_THUMBS_DIR`); `assets_per_s` staying
0 after the import says nothing was embedded again.

The search field's query language (`Tristan beach`, `Tristan "hello"`, `Tristan or Aaryan`,
`@tri`, `-beach`, `beach video`, `in:2024`, `before:2025-06`; docs/design/17 "Query syntax") is parsed
in the pack by `src/addons/ai/query.*`, so both chromes share it. `mv_ai_tests "[query]"` covers
the parser, names, suggestions and the singular / plural pair with no models; it is pure C++20
and also compiles on its own (`clang++ -std=c++20 tests/test_ai_query.cpp
src/addons/ai/query.cpp` with `-Isrc -Itests` and any Catch2).

**The Mac AI chrome's own measurements** (`src.swift/AIChrome/Tests`, not part of the cmake
build): `cd src.swift/AIChrome && swift test -c release -Xswiftc -enable-testing --filter
PeopleGridBench` lays the Settings People grid out at 200 people with an empty table (no pack)
and reports CPU time for the grid at rest, one layout pass, a status-line publish, and a people
update with and without a re-sort. Compare runs of the same build alternated with the base;
the 2026-10-03 numbers are in the PR that added it (a publish 51 → 8 ms, a people update
500 → 46 ms of CPU).
`--filter SearchModelTests` checks the search panel's model against a fake table (issue #191):
a search re-asks when another folder opens under "Folder & subfolders", and Tab only takes a
name looked up for the words as typed. `--filter StatusLineTests` checks the footer's status
line from a status struct (issue #417): with iCloud clips queued the pictures count and ETA are
named as this Mac's files and the clips as the rest of the run; with the queue empty it is the
plain pictures / sound / speech line.

**The Photos library source (Mac, issue #72; docs/design/17 "Photos library source").** Engine
behaviour is tested with a fake PhotoKit: `mv_ai_tests "[photos]"`. On a real library, two
tools each wrap themselves in a throwaway `.app`, because PhotoKit's permission prompt needs
`NSPhotoLibraryUsageDescription`. Each asks once for its own access, and prints counts and
timings only, never an identifier or a name:

- `tools/ai/photos-spike.sh [--stills N] [--videos N] [--opens N] [--edge 448] [--threads 2]`
  measures the source alone, with no model and no index. It reports:
  - enumeration time, and the local-rendition rate at the indexer's size;
  - how many stills and clips are only in iCloud;
  - the cost of opening a result (resolve plus `clonefile`).
- `tools/ai-bench/photos-bench.sh <build> --addons <dev add-ons> [--query "a dog"]...` runs the
  sideloaded pack over the library, as `ai-bench --index` does over a folder. It keeps
  thumbnails in its own scratch folder.

A local app build that should try the library in Settings needs the new `Info.plist` key and the
`packaging/macos/MediaViewer.entitlements` entitlement. `mediaviewer_app` adds both. Configure
with `-DMV_MAC_BUNDLE_ID=io.github.longtimeno-c.mediaviewer.dev`, so that the dev app's Photos
permission and preferences stay apart from an installed MediaViewer.

### The phone's catalogue

`mv_ai_tests "[phone]"` publishes to a local folder and reads the result back as a phone would
(docs/design/17 "The phone's catalogue"); no NAS is needed. `mv_ai_tests "[.perf-bench][phone-cost]"`
prints the publisher's idle cost and scan throughput, and `[.perf-bench][phone-scale]` its per-tile
and per-scan costs (`MV_TILE_DIR` points it at a folder of real JPEG-512s). The NFC table is
generated: `python3 tools/fold/gen_nfc_table.py > src/addons/ai/nfc_table.inc`, with the newest
Python, whose `unicodedata` sets the Unicode version.

### Local search from Final Cut Pro and FCPXML export

Status and verify lines: [docs/design/23](design/23-nle-search.md) (issue #71). Phase 1 (the search
agent) is built and measured; Phase 0's hands-on run in Final Cut Pro is owed.

- **Both platforms:** `mv_nle` (`cmake/nle.cmake`, included by `cmake/ai.cmake`) and
  `mv-nle-export`, which runs one search through the installed pack's read-only reader and
  writes FCPXML (Final Cut Pro, DaVinci Resolve and Premiere Pro import it):
  `mv-nle-export "birthday cake" --out cake.fcpxml` (`--json` prints the rows; `--videos`,
  `--photos`, `--scope-dir`, `--max`, `--no-keyword`, `--similar FILE --at MS`). It needs a pack
  new enough to have `mv_ai_reader_get`; with a dev-key build, point it at a sideloaded pack with
  `MV_DEV_ADDONS_DIR`.
- **Tests:** `mv_ai_tests "[search-agent],[nle]"`: the reader's top-K against the app engine's on
  the same index, the index byte-identical after a reader session, catch-up, the wire format's
  bounds, the FCPXML, and the thumbnail cache read without a write.
- **Mac only, in MediaViewer.app:** on an arm64 build, `mediaviewer_app` puts the workflow
  extension in `Contents/PlugIns/MediaViewerSearch.appex` and the agent's launchd job in
  `Contents/Library/LaunchAgents`. The agent is the app's own executable run as
  `MediaViewer --search-agent`.
  Configure with `-DMV_FCP_TEAM_ID=<team>` (the team the app is signed by; it prefixes the Mach
  service and the extension's app group). A dev-key build's agent accepts unsigned clients, so
  an ad hoc `mv-search-client` can reach it. `assemble`
  signs ad hoc, which FCP and the agent's peer check refuse; to try it for real, sign with
  `python3 tools/mac/macpack.py release --app build/MediaViewer.app --identity "Developer ID
  Application: …" --skip-notarize --allow-no-updater`, which keeps the extension's entitlements.
  To try the agent without turning it on, bootstrap a launchd job whose `Program` is the app's
  `Contents/MacOS/MediaViewer`, whose `ProgramArguments` are `MediaViewer --search-agent`, whose
  `MachServices` names `<team>.io.github.longtimeno-c.mediaviewer.fcp.search`, and (dev) whose
  `EnvironmentVariables` set `MV_DEV_ADDONS_DIR`; then, with `mv-search-client` signed by the same
  team: `mv-search-client mountain --thumbs`, `mv-search-client --bench 20 mountain beach dog`
  (agent vs in-process p50/p95, same results) and `mv-search-client --wait-exit 90` (seconds until
  the idle agent has gone). `launchctl bootout gui/$(id -u)/<label>` removes the job.
- **In Final Cut Pro:** with Local search Core installed, open Settings ▸ Local search ▸ Final
  Cut Pro ▸ **Turn on** (it registers the agent with `SMAppService` and elects the extension in
  with `pluginkit`; allow MediaViewer under Login Items if asked), then Extensions ▸ MediaViewer
  Search. **Turn off** reverses both. `pluginkit -m -i io.github.longtimeno-c.mediaviewer.finalcut`
  shows the election (`+` in, `-` out); a fresh install elects it out 10 s after first launch.
  The panel searches the open library's folder by default; the scope picker switches to any
  indexed folder or all of them. Space plays the selected result with sound, N / Shift+N step
  through its matches, and hovering a tile scrubs it. The options menu sets the drag's clip
  handles and the keyword collection, and shows the Phase 0 test drag (`~/Movies/test`).
  The panel logs to the `io.github.longtimeno-c.mediaviewer.fcp` subsystem. Read it with
  `/usr/bin/log show --predicate 'subsystem == "io.github.longtimeno-c.mediaviewer.fcp"'`; in zsh,
  a bare `log` is a shell builtin.

**The Photos library as a folder, and its backup (Mac; docs/design/26, 2026-10-03).** Once the library
was added in Local search's Settings, the folder tree (`⌘⇧E`) shows a *Photos Library* row and
File gains *Open Photos Library*: the host lists the library as **virtual items** (`photos:<id>`,
`shell/folder_model_mac.h` `list_entry::is_virtual`) and resolves each to a file as it is shown
(`shell/photos_items_mac.h`). Search results take the same path. Settings → *Photos Library* →
**Back Up Now** runs `shell/photos_backup.h` over PhotoKit into a chosen folder. Tests, no library
needed: `mv_tests "[folder][photos]"` (virtual entries and tiles over a fake provider),
`"[photos_backup]"` (the engine over a fake library: each folder layout, verified copies, the
manifest across a layout change and a remount, a collision, a cancel, a deleted file held until Copy
again or Leave deleted, Leave deleted by hash), `mv_import_tests
"[check_library]"` (Check library over the real engine and `import.db`: a folder against a library
with renamed copies, duplicates, deletions, an offline share and library scope; the Photos backup
over the same engine through `mv.import.1` with a fake Photos source; its cost on 20,000 files is
the hidden `"[.perf-bench][check_library]"`, `MV_CHECK_BENCH_FILES` to change the count),
`"[write_guard]"` and `"[poster]"` (a clip's display matrix turns its
poster; the thumbnail spec is `jpg512.4`). Edits of a library photo (docs/design/26 "Edits of a library
photo", docs/plans/edit-refresh.md S0): `mv_ai_tests "[photos]"`, with the writer audit
`"[photos][audit]"` (Detail, Remove, Extend, Open closed eyes, Improve faces and Save copy on a
`photos:` key with fake hooks: one add each, staged outside the library, nothing beside the
source, the staging folder `$TMPDIR/MediaViewer Saves` left empty), and `mv_tests "[photos]"`. On
a real library the result lands next to its source in the list and the notice says so; an
iCloud-only original shows *Downloading from iCloud… N %* while it comes. On Windows a OneDrive
placeholder shows *Downloading this file from the cloud…* in the title while Save copy or the
Edit pane reads it. A dev build shows the row and the section only when
its own bundle id has Photos access and the flag `mv.photosLibrary.added` is set in its
defaults; the flag is set by the pack's Settings when the library is a root, so a dev build
without the pack loaded can be pointed at it by hand (`defaults write
io.github.longtimeno-c.mediaviewer.dev mv.photosLibrary.added -bool true`) once macOS has
granted that bundle Photos access. The listing's time is logged as `photos: listed N assets in
M ms`: `log stream --predicate 'subsystem == "org.mediaviewer" AND category == "photos"'` (in zsh,
`/usr/bin/log`: `log` is a builtin). `tools/photos/name_probe.m` times the walk and its names
outside the app and checks them against the resources (counts only; its header says how to give
it Photos access). The viewer's on-view downloads and previews live in
`~/Library/Caches/MediaViewer/Photos Library/` (write-protected, emptied at launch); a backup's
fetched originals wait in `~/Library/Caches/MediaViewer/Photos Backup/` and are removed when
the run ends.

## Test

```powershell
# the video test corpus — REQUIRED for anything PR 5 claims to prove.
# The clips are gitignored (docs/design/09); only the generator is in the repo.
# Needs an ffmpeg on PATH (or $env:FFMPEG); NVENC is used when present,
# libx264/libx265/SVT-AV1 otherwise. ~1.5 GB, and the 31-minute clip is slow.
bash tools/testmedia/generate.sh          # --list to see what it makes
ctest --test-dir build -C Release -R corpus --output-on-failure   # confirm

# native unit tests and synthetic harness regression tests
ctest --test-dir build -C Release --output-on-failure

# ...and as CI runs it, where an absent corpus must fail rather than skip. CI's
# test-media job makes the clips and fetches the RAW/HEIC samples once (cached);
# every Build leg restores them and sets this. With CI set, test_corpus.cpp fails
# if MV_REQUIRE_CORPUS is unset ('0' is an explicit opt-out, as release.yml uses).
$env:MV_REQUIRE_CORPUS = "1"; ctest --test-dir build -C Release --output-on-failure

# the ABI, end to end from C#: SafeHandle, struct layout, completion drain. The
# second argument is mv_abi_layout's dump: the C# mirrors of the AI structs and function
# table are then compared field for field with the native layout (skipped without it; CI passes it)
cmake --build build --config Release --target mv_abi_layout
build\bin\Release\mv_abi_layout.exe > build\abi-layout.txt
dotnet build src.managed\MediaViewer.AbiSmokeTest\MediaViewer.AbiSmokeTest.csproj -c Release
dotnet src.managed\MediaViewer.AbiSmokeTest\bin\Release\net8.0-windows\MediaViewer.AbiSmokeTest.dll build\bin\Release build\abi-layout.txt

# WinUI chrome (also published beside mediaviewer_lab.exe by the CMake build)
dotnet publish src.managed\MediaViewer.Chrome\MediaViewer.Chrome.csproj -c Release -r win-x64 --no-self-contained

# Issue #153, the chromes' headless tests (no core build needed; CI's "WinUI chrome"
# and "SwiftUI chrome" jobs run them). They read the chromes' sources as text and fail
# when a copy of a command id, a captured key code, the rebind packing or a menu's
# printed shortcut drifts from src/shell or from tests/data/command_table.snapshot.
dotnet test src.managed\MediaViewer.Chrome.Tests\MediaViewer.Chrome.Tests.csproj -c Release
# macOS: the same checks for the SwiftUI chrome, Import's model, AI's search model
swift test --package-path src.swift/MediaViewerChrome
swift test --package-path src.swift/ImportChrome
swift test --package-path src.swift/AIChrome --filter SearchModelTests
# The native half (part of plain ctest): key remaps survive persist and reload, and
# describe_commands() by mode matches tests/data/command_table.snapshot. A change to
# the default key map fails it on purpose; regenerate, then read the diff:
$env:MV_UPDATE_SNAPSHOTS = "1"; .\build\bin\Release\mv_tests.exe "[snapshot]"; Remove-Item Env:MV_UPDATE_SNAPSHOTS

# Milestone G: the Import add-on's suite (built with the core, both platforms)
# Its verified copies flush durably and read back uncached in scratch folders
# under %TEMP%, so they run at the speed of that disk; CI points TMP/TEMP at
# the runner's local disk (runner.temp), not C:.
ctest --test-dir build -C Release -R import_ --output-on-failure
# PR 54 find duplicates: time a first scan and a cached rescan (synthetic tree,
# or MV_DUP_BENCH_DIR=<folder>, read only; the hash cache is a scratch import.db)
build/bin/mv_import_tests "[.perf-bench]"

# docs/design/25, open add-ons: their C++ cases are part of the suite above
# ([open-addon]); this is the author's tool, which packs with Python and
# asks the app's own reader (mv_addon_verify --open) about every package
ctest --test-dir build -C Release -R addon_sdk --output-on-failure
python tools/addon-sdk/test_mvaddon.py        # the tool alone, no build needed
# PR 56: settings pages, keymap packs and theme shape, and keymaps on the live table
build/bin/mv_import_tests "[addon]"
build/bin/mv_tests "[keymap]"

# docs/design/25 PR 57, the script sandbox: escapes, limits, permissions absent
# from the environment, script manifests and packages, the host's set of
# script add-ons (also in cmake/portable)
ctest --test-dir build -C Release -R script_ --output-on-failure

# ...and the same engine headless on Linux or any POSIX machine (CI's
# portable-core job, Ubuntu under ASan + UBSan; SQLite, libsodium, BLAKE3 and Catch2 from vcpkg via
# tools/portable/vcpkg.json, or the system). With FFmpeg, libspng and libjpeg
# installed (pkg-config) it also builds the PR 13 / 14 clip suites,
# mv_clip_tests and mv_trim_tests; with libspng and libjpeg alone, #288's
# Adjust Size maths and resolution writers, mv_adjust_size_tests:
#   cmake -S cmake/portable -B build-portable \
#     -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
#     -DVCPKG_MANIFEST_DIR=tools/portable
#   cmake --build build-portable && ctest --test-dir build-portable
#   build-portable/bin/mv_clip_tests "[bench]"   # keyframe trim of a ~1 GB MP4 (or MV_CLIP_BENCH_FILE)

# PR 13 / 14 clip editing: the same suites inside the Windows / Mac mv_tests
ctest --test-dir build -C Release -R "clip|trim" --output-on-failure
# #288 Adjust Size: the maths, the stack's stated resolution, and export
# writing it (JFIF, pHYs, EXIF), inside the Windows / Mac mv_tests
build/bin/mv_tests "[adjust_size]"

# policy gates (all run in CI on every push)
.\tools\check-module-graph.ps1     # dependencies point downward only
.\tools\check-hostable-core.ps1    # D9: no windows.h / d3d11.h, nor Cocoa / Metal, above gfx/ outside the ports
.\tools\check-vcpkg-baseline.ps1   # the five vcpkg baseline pins agree
.\tools\check-winui-controls.ps1   # no ProgressBar / ProgressRing / DropDownButton / InfoBar / TextBox...: they
                                   # fail-fast in the island host (use JobBar, Shared\FlatBar.cs, a Button + Flyout,
                                   # Shared\FakeInput.cs)
.\tools\licence-check.ps1          # no GPL FFmpeg, no software HEVC/AAC encoder
.\tools\telemetry-schema-check.ps1 # telemetry payload stays fixed vocabulary: no path/filename field

# PR 7 broken-file corpus: every seed in tests/data/seeds truncated, stomped,
# bit-flipped and given absurd dimensions, plus tests/data/broken, through every
# decode entry point. Fails on a crash, an escaped exception, a call over 5 s
# (60 s for each family's first-use warm-up), or runaway memory. Part of plain
# ctest; this runs just that suite.
ctest --test-dir build -C Release -L broken --output-on-failure
$env:MV_BROKEN_FULL = "1"          # exhaustive sweep (nightly CI)
$env:MV_BROKEN_DUMP = "broken-dump" # write each failing input here
$env:MV_BROKEN_TIMEOUT_MS = "5000"  # per-call budget (60000 in Debug/ASan)
$env:MV_BROKEN_FIRST_USE_MS = "60000" # each family's warm-up call
# seeds are synthetic and committed; regenerate with (needs Pillow, pillow-heif):
python tools\testmedia\make-seeds.py

# libFuzzer harnesses, one per decoder entry point (clang-cl + ASan), and
# fuzz_script: arbitrary text as an add-on script under tight limits (PR 57;
# non-gating in CI until its first runs there, seeds in tests/data/seeds/script)
cmake -S . -B build-fuzz -A x64 -T ClangCL -DMV_FUZZ=ON -DMV_BUILD_TESTS=OFF
cmake --build build-fuzz --config Release --target mv_fuzzers
.\tools\fuzz\run.ps1 -BuildDir build-fuzz -Seconds 60      # -Harness png,gif to pick
# CI (#384) gates on the eleven single-format image harnesses and runs the rest
# non-gating, 30 s each: decode animation thumbnail script pdf docx pptx in one step;
# meta_c2pa meta_jpeg meta_png meta_webp meta_bmff meta_xmp meta_icc meta_location
# meta_probe meta_search geo places_map addon_contributions ai_query in another.
# meta_search (#291, links mv_meta) and ai_query (#291, the Local search query
# language, seeds tests/data/seeds/query) are in mv_fuzzers like the rest.
# #290: the metadata readers (meta/parse.h) have harnesses too: meta_c2pa, meta_jpeg,
# meta_png, meta_webp, meta_bmff, meta_xmp, meta_icc, and (#345) meta_probe, the AI badge's
# provenance probe (add src/meta/probe.cpp src/codec/probe.cpp to the line below, seeds
# tests/data/seeds/probe). They link mv_meta_parse alone, so on
# a Mac they build with Homebrew LLVM (Apple clang ships no libFuzzer), e.g.:
#   /opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -fsanitize=fuzzer,address,undefined -fno-rtti -Isrc \
#     -Itools/fuzz tools/fuzz/fuzz_meta_png.cpp src/meta/{parse_common,c2pa,containers,xmp_raw,generators}.cpp \
#     src/codec/xml.cpp -lz -o fuzz_meta_png
#   ASAN_OPTIONS=detect_leaks=0 ./fuzz_meta_png -max_total_time=600 corpus tests/data/seeds/png tests/data/seeds/c2pa
# Their seeds (tests/data/seeds/{c2pa,xmp,icc}) come from the test builders:
#   MV_SEED_DIR=tests/data/seeds mv_tests "[.write-seeds]"
# #352: fuzz_meta_location (meta::parse_location, the Location editor) is pure and builds
# from one file:
#   /opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -fsanitize=fuzzer,address,undefined -fno-rtti -Isrc \
#     -Itools/fuzz tools/fuzz/fuzz_meta_location.cpp src/meta/location_edit.cpp -o fuzz_meta_location
#   ASAN_OPTIONS=detect_leaks=0 ./fuzz_meta_location -max_total_time=600 -max_len=512 corpus tests/data/seeds/location
# #606: fuzz_meta_gpmf (a GoPro GPMF packet and a DJI subtitle cue) is pure too:
#   clang++ -std=c++20 -fsanitize=fuzzer,address,undefined -fno-rtti -Isrc -Itools/fuzz \
#     tools/fuzz/fuzz_meta_gpmf.cpp src/meta/gpmf.cpp -o fuzz_meta_gpmf
#   ASAN_OPTIONS=detect_leaks=0 ./fuzz_meta_gpmf -max_total_time=600 corpus tests/data/seeds/gpmf
# On a Mac, Homebrew's LLVM has libFuzzer (Apple clang does not): build
# tools/fuzz/fuzz_script.cpp, third_party/lua/src/*.c (as C++),
# src/addon/script_runtime.cpp, script_thread_posix.cpp, open_manifest.cpp and
# manifest.cpp with -fsanitize=fuzzer-no-link,address,undefined, then link with
# -fsanitize=fuzzer,address,undefined, libsodium.a and build/libmv_io.a.

# PR 7 clean-VM gate: a HEIC decodes with MV_OS_CODEC=0, and the process has
# loaded libheif + libde265 and NO Media Foundation, WIC codec extension, or
# \WindowsApps\ module. Its own executable, because once mfplat.dll is in a
# process it never leaves and the assertion could not be made again.
ctest --test-dir build -C Release -L cleanvm --output-on-failure

# the frame-time gate — 60 seconds, needs a quiet machine
.\build\bin\Release\frametime.exe --seconds 60

# PR 7 no-pop gate: open a still that has a preview (a RAW, or any JPEG), soak,
# and fail if the preview → full swap popped — a cross-fade cut short, a view
# that jumped, or a frame dropped inside the fade. PR 1's cadence is judged on
# the same run, so a short run is diagnostic only.
.\build\bin\Release\frametime.exe --no-pop tools\testmedia\raw\canon_eos7dmk2.cr2 --seconds 62

# PR 5b's A/V drift soak. Writes a CSV of position, error percentiles, the
# least-squares drift slope and the present counters, one row a second.
# Under 1800 s it exits 4 and is DIAGNOSTIC ONLY — it is not the verify.
.\build\bin\Release\mediaviewer_lab.exe --av-soak 1860 --csv drift.csv `
  tools\testmedia\soak_31min_1080p_hevc_aac.mp4
```

### Place database (Places, docs/design/27)

`src/geo` reads a place database built from pinned Natural Earth and GeoNames downloads by
`tools/geo/build_places.py`. The script uses the Python standard library only and must be run
with `-I`. The full blob is not in git (17 MB). A 111 KB test blob is:
`tests/data/geo/places-test.mvgeo`.

```sh
# the full blob, through CMake: first run downloads ~66 MB into build/geo-downloads
# (MV_GEO_DOWNLOADS to move it) and takes about a minute; later runs reuse the cache
cmake --build build --target mv_geo_data          # -> build/geo/places.mvgeo
# ...or by hand
python3 -I tools/geo/build_places.py --downloads build/geo-downloads --out build/geo/places.mvgeo
# GeoNames republishes daily under the same URL: a changed file fails the pin.
# --accept-upstream takes it and prints the hash to pin.

# the committed test blob, after changing the script or the format (src/geo/blob.h)
python3 -I tools/geo/build_places.py --downloads build/geo-downloads \
  --out tests/data/geo/places-test.mvgeo --subset test

# the fold table (shared with file search and Local search, core/fold.h), after
# changing the fold rule or for a newer Unicode
python3 -I tools/fold/gen_fold_table.py > src/core/fold_table.inc

# tests: fold, the test blob, a damaged-blob sweep; the 300-row table when the
# full blob exists (MV_GEO_BLOB=path for one built elsewhere; MV_REQUIRE_GEO=1
# fails instead of skipping, as CI does)
build/bin/mv_tests "[geo]"
# load < 30 ms, one lookup < 50 us
build/bin/mv_tests "geo: load and lookup timings"     # also part of "[.perf-bench]"
# the loader fuzzer is fuzz_geo in mv_fuzzers (run.ps1 -Harness geo)

# The Places add-on (docs/design/27): build/addons/places/(lib)mv_places.(dylib|dll)
cmake --build build --target mv_places
build/bin/mv_tests "[places]"                  # map pixels, cache, gestures, section, search
build/bin/mv_import_tests "[places]"           # its manifest, then the add-on through a signed store
build/bin/mv_tests "places: map render time at pane width x 160 pt"   # in "[.perf-bench]"
build/bin/mv_tests "places: the folder map over 600 geotagged items at 2x"   # #601, in "[.perf-bench]"
MV_PLACES_MAP_DUMP=<dir> build/bin/mv_tests "[.places-map-dump]"      # BMPs of the map to look at
# the map's fuzzer is fuzz_places_map in mv_fuzzers (run.ps1 -Harness places_map)
# Package it (release: the workflow does this after signing the code)
python3 tools/package/addon-pack.py stage-places --src build/addons/places --blob build/geo/places.mvgeo
python3 tools/package/addon-pack.py pack --addon places --platform macos --src build/addons/places \
  --version 1.0.0 --key <key file> --out dist/

# Driftwire (docs/design/28): build/addons/driftwire/(lib)mv_driftwire.(dylib|dll)
cmake --build build --target mv_driftwire
build/bin/mv_import_tests "[driftwire]"       # wire, Noise (Cacophony vectors), channel, protocol 2's session (rejoin, resume, Bluetooth <-> Wi-Fi), 2-3 engines over loopback TCP, codes, the DHT, remembered devices (a folder stands in for the Keychain), privacy (#501), the add-on through the loader
build/bin/mv_import_tests "[.driftwire-bench]"   # MB/s against a plain copy over loopback, and each side's stages (MV_BENCH_DIR, _BIG_MB, _SMALL, _SMALL_KB, _CASES, _STAGES=0 for no timers)
# #540: a transfer between two machines, no app (built beside the add-on, never shipped). The receiver
# measures its disk, prints a secret, and removes what it received after each run (--keep leaves it).
build/bin/mv-driftwire-bench receive --runs 6 --json driftwire-lan.jsonl --label gige    # --port 54540 --dir D --disk-check-mb 1024
iperf3 -s                                                                                # on the receiver too, for --iperf3
build/bin/mv-driftwire-bench send <receiver IP> 54540 <secret> --set big --iperf3 --runs 2 --json driftwire-lan.jsonl --label gige
#   --set big (1 x --big-mb, default 8192) | small (500 x 2 MiB) | tiny (5,000 x 200 KiB) | <a folder>; --fixture D keeps the set
#   --iperf3-streams N (default 4), or --line-mb-s X for a reference measured some other way
build/bin/mv_import_tests "[.driftwire-stream-bench]"   # #529: the DHT stream over emulated internet paths, a JSON line per profile (MV_STREAM_BENCH_S, _PROFILES, _SEED, _TRACE=1 for the sender's state every 100 ms)
build/bin/mv_import_tests "[.driftwire-idle]"    # CPU while Keep receiving is on, window closed (MV_IDLE_SECONDS)
python3 -m unittest tools/package/test_addon_pack.py   # DriftwirePackTest among them
build/bin/mv_import_tests "[.driftwire-send]"    # #452: a sender on this machine (MV_SEND_FILE, MV_SEND_CODE_FILE or MV_SEND_HOLD)
build/bin/mv_import_tests "[cli]"             # the driftwire command line: keys, the line editor, paths, render goldens, the live region, args, sessions over two engines, the program refusing a pipe
build/bin/mv_import_tests "[cli][e2e]"        # POSIX: two mv_driftwire_cli_test programs in ptys, a code typed across
# Linux: the Secret Service case runs against a keyring of its own (as CI's portable-core job),
# in a throwaway home: --unlock makes the login keyring of $HOME with that password, or unlocks
# the one there, so never run it in your own. A session bus, gnome-keyring unlocked with a
# NON-EMPTY password (an empty one makes no login keyring), then MV_SECRET_SERVICE=required makes
# a skip a failure. The subshell keeps your environment; the two daemons outlive it (kill them).
(
  export HOME=$(mktemp -d) XDG_RUNTIME_DIR=$(mktemp -d)
  export XDG_DATA_HOME=$HOME/.local/share
  export DBUS_SESSION_BUS_ADDRESS=$(dbus-daemon --session --fork --print-address=1)
  printf 'any-password' | gnome-keyring-daemon --unlock --components=secrets   # then wait until it owns org.freedesktop.secrets (ci.yml)
  MV_SECRET_SERVICE=required build-portable/bin/mv_import_tests "[driftwire]"
)
# #472 relays: a relay server of your own (built beside the add-on, never shipped with the app).
# On a machine both ends reach (a VPS, or a home server with the port forwarded); then type
# "<address or name>:47614#<key>" (it prints the key) in Driftwire's "Your own relay server (optional)" on
# the receiving computer. Only a holder of the key can wait there.
build/bin/mv-driftwire-relay --port 47614      # --key-file F (or MV_RELAY_KEY; else it makes a key and prints it once) --max-pairs 8 --wait-s 600 --max-gb 64 --rate-mbs 0 --idle-s 120
# The NAS service (docs/design/28 "The NAS service"), any host; a Mac or Linux box stands in for the NAS here.
# DIR/serve.json names the port and the folders; DIR/secrets is made 0700. The device key is the phone's public key.
build/bin/driftwire serve --data DIR init                # prints the identity and, once, the library access key
build/bin/driftwire serve --data DIR add "My phone" <64-hex-public-key>
build/bin/driftwire serve --data DIR                     # needs no terminal; revoke with: serve --data DIR revoke "My phone"
build/bin/mv_import_tests "[driftwire-serve]"            # the service over loopback
# The Tunnel: key holders' forwards go to ports 80 and 443 unless told otherwise; none turns it off.
build/bin/mv-driftwire-relay --port 47614 --key-file key.txt --forward-ports 80,443 --max-forwards 64
# Slice 4: also on 443 behind TLS, a web site to anyone without the key (BoringSSL; brew install nasm go first)
build/bin/mv-driftwire-relay --port 47614 --key-file key.txt --tls-port 443 --tls-cert fullchain.pem --tls-key privkey.pem --decoy-dir site/
# Slice 5 (the VPN): DNS to this resolver; UDP ports beyond 53 only if listed
build/bin/mv-driftwire-relay --port 47614 --key-file key.txt --dns 9.9.9.9 --forward-udp-ports 53
xcrun swiftc -typecheck -target arm64-apple-macos14.0 -module-name DriftwireTunnel src/addons/vpn/mac_tunnel/*.swift
# The VPN's provider built and embedded. It runs only from a Developer ID release with both
# provisioning profiles (RELEASING.md, "the Driftwire VPN's provisioning profiles"):
cmake -S . -B build -DMV_DRIFTWIRE_PACKET_TUNNEL=ON && cmake --build build --target mediaviewer_app
python3 tools/mac/macpack.py release --app build/MediaViewer.app --identity "Developer ID Application: …" \
  --team-id <TEAM> --app-profile <app>.provisionprofile --tunnel-profile <tunnel>.provisionprofile \
  --skip-notarize --allow-no-updater      # then copy to /Applications and approve the extension once
# With the VPN add-on's Tunnel on (Settings → Add-ons → VPN, its menu-bar row, or /vpn on), through its proxy:
curl -p -x http://127.0.0.1:<port> https://example.com/      # or HTTPS_PROXY=http://127.0.0.1:<port>, ALL_PROXY=socks5h://127.0.0.1:<port>
python3 tools/driftwire-relay/tor_spike.py --tor /opt/homebrew/bin/tor --mb 64 --runs 3 --live-tor   # owner only: joins the live Tor network
python3 tools/package/addon-pack.py pack --addon driftwire --platform macos --src build/addons/driftwire \
  --version 1.0.0 --key <key file> --out dist/
# #472: the parsers of what a peer sends, fuzzed on the Mac with Homebrew's LLVM (CI builds
# tools/fuzz/fuzz_driftwire.cpp with clang-cl in the fuzz job); S=src/addons/driftwire
/opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -O1 -g -fsanitize=fuzzer,address -isysroot $(xcrun --show-sdk-path) \
  -Isrc -Ibuild/vcpkg_installed/arm64-osx/include tools/fuzz/fuzz_driftwire.cpp $S/wire.cpp $S/noise.cpp \
  $S/cpace.cpp $S/channel.cpp $S/serve_wire.cpp $S/serve.cpp $S/transfer.cpp $S/bt_pipe.cpp $S/invite.cpp $S/mapping.cpp $S/net_address.cpp \
  $S/net_posix.cpp $S/machine_mac.cpp $S/bencode.cpp $S/sha1.cpp $S/dht_item.cpp $S/dht.cpp $S/udp_hub.cpp \
  $S/known.cpp $S/secrets_file.cpp $S/splice.cpp $S/relay.cpp $S/internet.cpp $S/meet.cpp \
  src/io/file_port.cpp src/io/file_port_mac.cpp src/io/content_hash.cpp \
  build/vcpkg_installed/arm64-osx/lib/libsodium.a build/vcpkg_installed/arm64-osx/lib/libblake3.a \
  -framework CoreFoundation -framework SystemConfiguration -o build/fuzz_driftwire
build/fuzz_driftwire <corpus dir> -max_total_time=180 -max_len=4096
```

To see it in the app on a developer build (`-DMV_ADDON_DEV_PUBLIC_KEY`): pack with the dev key as
above, unzip it with `ditto -x -k` into `<scratch>/addons/Places/1.0.0/` beside the signed
`manifest.json(.sig)`, and start a copy of the app with its own bundle id, `HOME`,
`MV_DEV_ADDONS_DIR` and `MV_DEV_THUMBS_DIR` (as the slots test add-on, under "Where this actually
is"). PR 56 is in the same build, so the manifest's `settings` page installs as it is; a change
on Places' page in Settings redraws the Place section at once (`mv_import_tests "[settings]"`
covers the re-ask, `"[places]"` Places' own settings).

### Performance suite: regenerating the README charts

Every chart in the README's "Speed you can measure" is drawn from a report one of the
app's own harnesses wrote; nothing is typed in. One command re-measures all of them and
redraws the SVGs:

```powershell
python tools/perf/regenerate.py --list           # stages, and what each is missing here
python tools/perf/regenerate.py                  # full run: docs/perf + docs/img (~15 min)
python tools/perf/regenerate.py --out perf-run   # staged: reports + charts under perf-run/
python tools/perf/regenerate.py --quick --out x  # seconds per soak: checks the suite only
python tools/perf/regenerate.py --only first-pixel,browse
python tools/perf/make-charts.py                 # redraw from the committed reports
```

| Stage | Harness | Report | Chart |
|---|---|---|---|
| `pacing` | `frametime --seconds 60` (animated + idle soak) | `frametime-animated.json`, `frametime-idle.json` | `perf-pacing.svg` |
| `pan` | `mediaviewer_lab --soak 60 --pan-soak` on the 42 MP ARW | `pan-soak-42mp-arw.json` | `perf-pacing.svg` |
| `first-pixel` | `mediaviewer_lab --soak 6 --static` per camera file, after one warm-up open | `first-pixel/<file>.json` | `perf-first-pixel.svg` |
| `browse` | `mediaviewer_lab --browse-soak` on `tools/testmedia/raw` | `browse.json` | `perf-browse.svg` |
| `video` | `mediaviewer_lab --soak 60` on the 1080p HEVC + AAC soak clip | `investigation/video-native-60s.json` (+ `.video.json`) | `perf-video.svg` |
| `av-sync` | `mediaviewer_lab --av-soak 120 --csv` on the same clip | `investigation/av-after-120s.csv` | `perf-av-sync.svg` |
| `compare` | `tools/perf/compare-screen.ps1` (MediaViewer vs Windows Photos / Media Player) | `compare/screen.json` | `compare-*.svg` |
| `bench` | `mv_tests "[.perf-bench]"`: headless decode, colour, folder list and sort | `bench.json` | — |
| `search` | `mv_ai_tests "[.calibration]"` at 1,000–25,000 photos, both picture towers; needs `MV_AI_PACK_DIR` and `MV_AI_EVAL_DIR` (the labelled COCO folder, "Local search" above) and takes `MV_AI_CALIBRATION_CACHE` and `MV_AI_EVAL_NAME`. Accuracy, not timing: it runs on either platform and leaves `machine.json` alone | `search.json` | `search-accuracy.svg` |

The published charts are Windows numbers. The macOS lab has `pacing`, `first-pixel`,
`browse` and `video`; `pan`, `av-sync` and `compare` are Windows-only harnesses, so a Mac
run draws the charts it has and names the rest as skipped. The README's Mac numbers are a run with
`--out docs/perf/mac --only pacing,first-pixel,browse,video,bench`. Inputs are the RAW/HEIC samples
(`tools/testmedia/fetch-raw.ps1`, `fetch-heif.ps1`) and the video corpus
(`tools/testmedia/generate.sh`); a stage whose inputs are missing is skipped, never faked.
Soaks open windows and need a visible display and a quiet machine. On macOS a measuring
run orders its window front without taking the keyboard, so typing elsewhere cannot reach
it, and `--browse-soak` voids its report (`"error"`) if the window was covered. Each run
also writes `machine.json` (OS, CPU, commit). `tools/perf/test_perf_tools.py` checks the
suite itself (every chart reproduces byte-for-byte from the committed reports) and runs
under `ctest -R perf_tools`.

Timing asserts are not in the unit suite (it runs under `ctest -j` and on shared runners):
they are hidden `[.perf-bench]` tests beside the bench, run on a quiet machine in an optimised
build. `mv_tests "[.perf-bench]"` holds the colour-stage starvation bound, pairing a 4000-file
dump and, on Windows, the WARP tile frame and create budgets; `mv_ai_tests "[.perf-bench]"`
holds the 100 k-row search scan.

**Launch (issue #486).** Every Windows launch logs its boot timeline once it is ready
(`boot (ms from process start): host_entry=… interactive=…`, in the debugger output). To measure,
alternate builds with the harness, warm, one file or none:

```powershell
powershell -File tools\perf\boot.ps1 -Exe C:\b\base\bin\mediaviewer_lab.exe,C:\b\new\bin\mediaviewer_lab.exe `
  -Runs 10 -Open C:\media\raw\sony_ilce7rm3.arw -Out boot.json
```

On the Mac the line goes to stderr, and the runner is `tools/perf/boot.py` (it
runs on Windows too); hold the display awake, since the lab shows its window each launch:

```bash
caffeinate -d -i python3 tools/perf/boot.py --exe ../base/build/bin/mediaviewer_lab --exe build/bin/mediaviewer_lab --runs 10 --open ~/media/arw/sony_ilce7rm3.arw --out boot.json
```

It discards one warm-up launch per build, prints min / median / max per stage and writes every run.
Put the file in a folder of its own so the folder scan is the same each run. The stage list and
the report shape are `src/core/boot_timeline.h`; the published runs are `docs/perf/boot/`. Not a
README chart yet.

`mv_tests "[.perf-bench]"` is the quick loop for decode work: `MV_BENCH_JSON=path` writes
its medians, `MV_BENCH_DIR=folder` adds your own files to the decode rows. Its `preview` row is
the canvas's first pixel (`decode_first_pixel`). libheif's example is one small image, so for
camera-sized HEIC run `python tools/testmedia/make-grid-heic.py` (after `fetch-heif.ps1`; needs
an ffmpeg with libx265 on `PATH`, a dev tool only): it writes `tools/testmedia/heif/grid-12mp.heic`,
an iPhone's layout (4032x3024 as 8x6 tiles of 512, a 320x240 thumbnail item, Display P3 ICC).
`MV_OS_CODEC=0` times the bundled libheif path, the one a PC without the HEVC extension takes.
`mv_tests "thumb store lookup throughput"` (also tagged `[.perf-bench]`, printed, not in the
JSON) times 2,000 thumbnail-cache hits from four threads, the filmstrip sweep's lookups.
`mv_tests "job system per-job overhead"` (the same tag, printed) times the worker pool alone:
100,000 empty jobs on four workers, foreground only and alternating lanes (on macOS, a QoS switch
per job).

`mv_tests "foreground start latency under a background sweep"` (also `[.perf-bench]`, printed)
times how long a view-tied job waits for a worker while 60 ms background jobs fill a two-worker
pool: the job system's reserved foreground worker (issue #160).

The pacing soak cannot see a paused clip's cost (it presents nothing). `mv_tests "[idle]"`
(macOS, headless, a synthetic clip) does: it prints the process's context switches and CPU
per second with a clip paused on its poster and with one played out, and fails above 200
wakes a second (issue #230: the sleep-polling player measured ~1,750 paused, now single digits).

### Copy throughput: `copybench` (PR 49)

`copybench` runs the copy engine (`io/verified_copy`, what F8 and Import use) over a folder
with no app, and prints one JSON line: files, bytes, seconds, MB/s, files/s. It copies into a
fresh `copybench-<n>` folder under the destination and removes it afterwards (`--keep` leaves it).

```bash
build/bin/copybench --make-fixture ~/scratch/raw   --count 100  --size-kib 16384
build/bin/copybench --make-fixture ~/scratch/small --count 2000 --size-kib 200
# alternate the runs: seq is the pre-PR-49 path, auto is what the app picks
build/bin/copybench ~/scratch/raw /Volumes/nas/scratch --mode seq
build/bin/copybench ~/scratch/raw /Volumes/nas/scratch --mode auto
build/bin/copybench ~/scratch/raw /Volumes/nas/scratch --mode auto --no-verify   # vs ditto / robocopy
```

`--mode deep --depth N --files N --chunk-kib N` sets the shape by hand; `--rtt-us N` makes
every file request wait N µs first, a stand-in for a share's round trip on a local disk (not
a substitute for one: PR 49's verify line wants a real 10 GbE share). The fixtures are
incompressible, so a share that compresses on the wire shows no flattering number. On
Windows the reference is `robocopy <src> <dst> /E /MT:16 /J`; on the Mac,
`ditto <src> <dst>`. Delete the fixtures from the share afterwards.

### Open add-ons self-test, PR 56 leg (macOS)

The same rig walks settings pages and key layouts across a restart: install
`example.settings-demo`, photograph its page (`p2-page.png`, `p4-page.png`), change four values;
then a second launch over the same folder installs `example.vim-keys`, reads the values back,
applies the Vim layout, Reset, removes settings-demo keeping its settings, installs it again and
removes it without. Keys are not written to your preferences while it runs:

```bash
python3 tools/addon-sdk/mvaddon.py pack examples/addons/settings-demo --key /tmp/mv-addon.key --out /tmp/mv-addon
python3 tools/addon-sdk/mvaddon.py pack examples/addons/vim-keys --key /tmp/mv-addon.key --out /tmp/mv-addon
```

```bash
MV_ADDON_SELFTEST=/tmp/mv-addon/rig56 MV_ADDON_SELFTEST_PR56=a caffeinate -d -i build/bin/MediaViewer /tmp/mv-addon/example.settings-demo-1.0.0.mvaddon
MV_ADDON_SELFTEST=/tmp/mv-addon/rig56 MV_ADDON_SELFTEST_PR56=b MV_ADDON_SELFTEST_REINSTALL=/tmp/mv-addon/example.settings-demo-1.0.0.mvaddon caffeinate -d -i build/bin/MediaViewer /tmp/mv-addon/example.vim-keys-1.0.0.mvaddon
```

`rig56/state.txt` has a line per step: the page's values, the keys of Previous / Next / Toggle
mark / Flip horizontal / Forward 10 s before and after the layout and after Reset, and whether
the settings file was kept.

The settings / keymap / theme parsers have a libFuzzer harness, `tools/fuzz/fuzz_addon_contributions.cpp`,
in the Windows clang-cl fuzz build (`mv_fuzzers`, seeds in `tests/data/seeds/addon`); its header
has the one-line Homebrew LLVM build for a quick run on a Mac.

### "What's new" in the update notice (#620, both hosts)

The release writes a short "What's new" once (`tools/package/whats_new.py`, from the run's
`whats_new` input or the commit subjects since the last release) and puts it where each app
already looks: the Windows manifest's signed `notes` and the Mac appcast's embedded plain-text
description ([docs/design/13](design/13-updates-and-telemetry.md#staging-and-restart)). Both
bars add it to the "Update ready — restart" tooltip. Tests:

```sh
python3 tools/package/test_whats_new.py -v        # the rule, fitting, the git history
python3 tools/package/test_github_release.py -v   # prepare's output, the release page
python3 tools/mac/test_macpack.py -v              # embedded, never a releaseNotesLink
dotnet run --project src.managed/MediaViewer.Updater.Tests -c Release   # add -p:EnableWindowsTargeting=true off Windows
```

Where it is (2026-11-02): the updater suite (112 cases, the notes rule, altered notes failing the
signature, schema 1 still updating) and the three Python suites pass in a Linux container with
the .NET 8 SDK; the manifest tool wrote and signed a manifest with notes there. Not built or run:
the WinUI chrome and the Mac host (`main_mac.mm`, the Swift bar), `generate_appcast` on a real
feed, a release run. Owed: an update with notes shown in the restart notice on both hosts, a
proxy log showing no request beyond today's fixed asset URLs, both PR 1 present-loop gates.

### Add-on restart card (#510, both hosts)

The card that asks for a restart after an add-on update ([docs/design/18](design/18-import.md)
"Updates") can be shown without publishing an update. `MV_ADDON_RESTART_SELFTEST` lists the
updates as `Name=version`; `;busy=<what>` makes something run that Restart now waits for, and
`;wait` presses Restart now (the card then says "Waiting for … to finish"; nothing restarts):

```powershell
$env:MV_ADDON_RESTART_SELFTEST = "Import=1.3.0,Local search=0.4.0;busy=a Driftwire transfer;wait"
build\bin\mediaviewer_lab.exe --new-instance tools\testmedia\raw
```

Leave it set for `--soak 60 --gate` to measure the present loop with the card up. The words
are `src.managed/Shared/RestartWords.cs`; `dotnet test src.managed/MediaViewer.Chrome.Tests`
checks them and that the Mac's copy (`RestartNoticeView.swift`) says the same. The Mac reads
the same variable:

```sh
MV_ADDON_RESTART_SELFTEST="Import=1.3.0,Local search=0.4.0" build-darwin/MediaViewer.app/Contents/MacOS/MediaViewer tools/testmedia/raw
```

**Mac, built and run (2026-10-20, M-series, Release):** the Mac half compiles with no
fixes needed. `mv_import_tests "[slots]"` 21 and `"[driftwire]"` 125 cases pass; `mv_tests` 899
passed, 11 skipped. In a copy of the app with its own bundle id and `HOME`: the two-update card
in the corner, **Later** hiding it while the bar keeps **Add-on updated — restart**, the
Settings marker and **Restart now** above the list, the rows and their states, a row's page and
back; with `;busy=a Driftwire transfer;wait`, the waiting card, and **Cancel** back to the
update card. Two fixes, to match Windows: Esc on an add-on's page now goes back to the list
(the host took Esc and closed Settings; `MVChromeHost.settingsBack`), and the list comes back
scrolled to the row it left. Mac PR 1 gate (`--soak 60 --gate`, `IMG_2.JPG`), card off / on
alternated in the same build: off 0 / 17.19 ms max, on 0 / 17.10, off 0 / 17.03, on 0 / 17.00
(dropped / max), p99 16.85–16.9 ms, 0 idle presents, all pass. Not run on the Mac: **Restart
now**'s relaunch (an installed MediaViewer was running and would take the hand-off), live
reload of a newer signed add-on, VoiceOver. While the card is up it sits above Settings, as
the resume notice does, and covers Settings' **Done** (Esc still closes it).

### Add-on install failures and the updates notice (#525, Windows)

The failure rows (plain words and one next step) and the command bar's **Add-on update
available** ([docs/design/18](design/18-import.md) "Installing", "Updates") can be shown without
a broken network or a published update. `MV_ADDON_INSTALL_SELFTEST` takes `id=update:<version>`
(the channel offers that version until a probe asks it for real) and `id=fail:<kind>` (an
`InstallFailure`: `offline`, `damaged`, `notverified`, `nospace`, `notstarted`, `failed`), for
`import`, `ai` and its pieces, `places`, `driftwire` or another slot add-on's id; `;now` runs the
background check after a second instead of 30 s (it still obeys "Check for updates
automatically"):

```powershell
$env:MV_ADDON_INSTALL_SELFTEST = "places=update:9.9.9,import=fail:offline"
build\bin\mediaviewer_lab.exe --new-instance tools\testmedia\raw
```

The words are `src.managed/Shared/InstallWords.cs`; Chrome.Tests checks them and that the Mac's
`InstallWords.swift` has the same strings and failure kinds.

### Script add-ons self-test (PR 57, macOS)

`MV_SCRIPT_SELFTEST=<folder>` presses an add-on's keys as a person would (key events through the
app's own queue: router, command table, runtime) and photographs the result. Put two add-ons in
`<folder>/store` first: the example Folder Count (Count files, `⌘⇧K`, with `folder.read`) and a
spinner whose Spin command (`⌘⇧J`) never returns:

```bash
python3 tools/addon-sdk/mvaddon.py pack examples/addons/folder-count --key /tmp/mv-addon.key --out /tmp/mv-script
```

```bash
build/bin/mv_addon_verify --open /tmp/mv-script/example.folder-count-1.0.0.mvaddon /tmp/mv-script/rig/store
```

Pack and install the spinner the same way (`"scripts": ["main.lua"]`, `function spin() while
true do end end`, a command `spin` on `Ctrl+Shift+J`), then:

```bash
MV_SCRIPT_SELFTEST=/tmp/mv-script/rig caffeinate -d -i build/bin/MediaViewer tools/testmedia/raw
```

`rig/state.txt` gets the notice Count files showed ("Folder Count: 4 files in this folder"), the
scripts' state after Spin (stopped, "ran for more than 100 million steps"), and `s0` … `s3` PNGs,
the last of Settings → From others. Two more variables serve the measurements and are inert
unless set (both hosts): `MV_SCRIPT_STORE=<folder>` reads open add-ons from that folder instead
of the profile's, and `MV_SCRIPT_SPIN=<addon>/<command>` keeps that command running back to back
(for the present-loop gate with a script busy; a command that stays inside the limits, such as a
half-second loop, keeps the add-on's worker at one core without stopping it):

```bash
MV_SCRIPT_STORE=/tmp/mv-script/rig/store MV_SCRIPT_SPIN=rig.spinner/burn caffeinate -d -i build/bin/frametime --seconds 60 --lab build/bin/mediaviewer_lab
```

### Open add-ons self-test (PR 55, macOS)

The Mac app can walk the whole add-on flow by itself and photograph each step: the sheet for a
package, Install, each of its themes, Default again, a file of the installed add-on changed on
disk (the chrome must fall back and say so), Remove. It installs under the folder you name and
keeps the theme choice in memory, so nothing in your profile is touched:

```bash
python3 tools/addon-sdk/mvaddon.py keygen --out /tmp/mv-addon.key
```

```bash
python3 tools/addon-sdk/mvaddon.py pack examples/addons/film-tones --key /tmp/mv-addon.key --out /tmp/mv-addon
```

```bash
MV_ADDON_SELFTEST=/tmp/mv-addon/rig caffeinate -d -i build/bin/MediaViewer /tmp/mv-addon/example.film-tones-1.0.0.mvaddon
```

`rig/state.txt` has one line per step (what is offered, what is installed, which theme is on,
the viewer's background colour, what Settings says); `a1-sheet.png` … `a9-removed.png` are the
window. To look at what the app makes of any package without the app:

```bash
build/bin/mv_addon_verify --open some.mvaddon
```

Add a folder as a second argument and it installs there, as the app would. A `.mvaddon` is
registered with both OSes (the installer's `MediaViewer.Addon` ProgId, the Mac bundle's
`Add-on` document type over the exported UTI `io.github.longtimeno-c.mediaviewer.addon`), so a
double-click opens the install sheet once the installed build carries it; it is not a photo type
and the default-viewer prompt leaves it alone. Windows has no rig
yet; its half is checked by compiling (`dotnet msbuild src.managed/MediaViewer.Chrome/MediaViewer.Chrome.csproj -t:Compile -p:WindowsAppSDKSelfContained=false`
works on any OS with the .NET 8 SDK) and by CI.

### Edit workspace self-test (PR 29, macOS and Windows)

Both apps can drive their own Edit workspace and photograph each step, with no pointer (and, on
the Mac, no screen-recording permission). Point it at a scratch copy (Save copy writes a new file beside it,
and the run edits then reverts its metadata):

```bash
MV_EDIT_SELFTEST=/tmp/mv-edit build-darwin/MediaViewer.app/Contents/MacOS/MediaViewer ~/scratch/IMG_0001.jpg
```

```powershell
$env:MV_EDIT_SELFTEST = "$env:TEMP\mv-edit"
.\build\bin\Release\mediaviewer_lab.exe --open C:\scratch\IMG_0001.jpg
```

It quits when done, leaving `s0-viewer` … `s8-closed-reverted` captures (PNG on the Mac, BMP on
Windows; a clip gives `c0` … `c4`) and `state.txt` in the folder. Since the inspector refresh
(docs/plans/edit-refresh.md S2) a still also passes the Enhance tab (`s4d-enhance`) and presses
✦ Auto twice: `state.txt`'s `auto_enhance:` line should read `after` one more than
`edits_before`, `on=1`, and `after_second_press` back at `edits_before`. The typed request's
`prompt_temperature:` line (both hosts; "a bit warmer" on Windows, a longer request on the Mac)
should read `rose=1`. On the Mac,
`MV_EDIT_SELFTEST_DARK=1` or `0` forces Dark or Light Mode; Windows follows the system theme.

On a clip it opens the Video Editor and, on the Mac, also walks a marked range, a dragged edge,
the `L` / `K` shuttle and the close guard, writing `polish:` lines to `state.txt`. Windows'
`MV_EDIT_SELFTEST_KEYS=1` walk does the same with synthesised keys (End, `J`, `O`, `J`, `I`,
Delete, `L`, `K`, the exports, Esc).

**Editor soak (Mac).** `MV_EDIT_SELFTEST_SOAK=<seconds>` on a clip cuts it to three pieces, plays
the program three times, sits paused, closes the editor and sits again, then writes one `soak:`
line: whole-process and main-thread CPU for each phase, the playback ticks that saw a cut
(`glimpse_ticks`) and the jumps over cuts. Add-ons are not loaded for the run. Compare builds
the usual way (each in its own worktree, runs alternated, the display awake: wrap in
`caffeinate -d -i -u`); a run whose `jumps` is short of 6 stalled and does not count.

```bash
MV_EDIT_SELFTEST=/tmp/mv-soak MV_EDIT_SELFTEST_SOAK=8 caffeinate -d -i -u build/bin/MediaViewer ~/scratch/clip.mp4
```

### Crash reports (PR 7)

Native crashes are captured out-of-process by Crashpad into
`%LocalAppData%\MediaViewer\Crashes`. Nothing is uploaded. On the next launch the app
scrubs each dump: memory outside thread stacks is zeroed, and paths, media filenames and
your username are masked. Managed exceptions go to `Crashes\managed\`.
[docs/design/13](design/13-updates-and-telemetry.md) has the details.

The PR 7 verify is "a deliberately-corrupted RAW produces a minidump containing no path,
filename, or pixel data". The crash hook only fires when **both** the environment variable
and the marker in the file are present.

```powershell
# 1. a marked COPY of a real RAW, in PRIVATE_FOLDER_canary\SECRET_FILENAME_canary_7Q3.dng
.\tools\make-crash-raw.ps1 -Source tools\testmedia\raw\pentax_k50.dng

# 2. crash on it
$env:MV_CRASH_TEST = 'decode'
.\build\bin\Release\mediaviewer_lab.exe --open "$env:LOCALAPPDATA\Temp\mv-crash-canary\PRIVATE_FOLDER_canary\SECRET_FILENAME_canary_7Q3.dng"
Remove-Item Env:MV_CRASH_TEST

# 3. scrub: relaunch the app (it scrubs on start), or run the standalone tool
.\build\bin\Release\mv_minidump_scrub.exe <in.dmp> <out.dmp>

# 4. scan; exit 0 = PASS
.\tools\minidump-scan.ps1 -Dump <out.dmp> -Forbidden '<canary path>','SECRET_FILENAME_canary_7Q3','PRIVATE_FOLDER_canary',$env:USERNAME

# pixel data: run make-crash-raw.ps1 with no -Source. It writes a pattern BMP pair and
# prints the byte patterns. Open SECRET_COMPANION_canary.bmp instead; neighbour prefetch
# crashes on the canary. Pass the printed patterns to the scan as -PixelHex.

# video threads (#233): MV_CRASH_TEST=video crashes the video decode thread on the
# first frame of any clip. Copy a corpus clip into the canary folder and open that.
$env:MV_CRASH_TEST = 'video'
.\build\bin\Release\mediaviewer_lab.exe --open "<canary folder>\SECRET_FILENAME_canary_7Q3.mp4"
Remove-Item Env:MV_CRASH_TEST
# scrub and scan as above; the dump's mv_decode_N slots read
# "role=video fmt=mov,mp4,... dec=<codec>/Lavc... <w>x<h> <bits>bit" and name no path.
```

`--av-soak` exits 0 only on a run of 1800 s or more whose drift slope stays
within 1 ms/min, with no position discontinuities and no host-clock gaps; 1 is a
real failure, 3 means the clip ended early, 4 means the run was too short to
count. Audio must be the master for a clip that has an audio track, or it fails.

**The drift figure only means something with the audio master.** On a clip with no
audio track the clock falls back to the host (`clock host fallback` in the F3
overlay) and the slope is measuring the host clock against itself, so a large
number there — tens of ms/min — is an artefact of the fallback, not a defect. Read
the `audio_master` column in the CSV before reading the slope. That is the whole
reason the corpus now carries an audio-bearing 31-minute clip.

#### macOS (PR 11)

Crashpad's `crashpad_handler` runs out of process (`MediaViewer.app/Contents/Helpers`, or
beside `mediaviewer_lab`). Dumps go to `~/Library/Application Support/MediaViewer/Crashes`;
nothing is uploaded, and forwarding to Apple's crash reporter is off. The next launch
scrubs each dump with the Windows scrub, which also masks POSIX paths under `/Users`,
`/Volumes`, `/private`, … and the short and full user name and computer name. An uncaught
`NSException` from the chrome is written, scrubbed, to `Crashes/chrome/` with the
correlation id of the native call in flight; the dump carries the same id
(`mv_last_call_cid`). The verify, with `tools/mac/crash_canary.py` in place of the two
PowerShell scripts:

```sh
# 1. a marked COPY of a real RAW (or no --source: the pixel-pattern BMP pair)
python3 tools/mac/crash_canary.py make --source tools/testmedia/raw/pentax_k50.dng

# 2. crash on it, then relaunch normally: the relaunch scrubs the dump
MV_CRASH_TEST=decode build/MediaViewer.app/Contents/MacOS/MediaViewer \
    /tmp/mv-crash-canary/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.dng
open build/MediaViewer.app

# 3. scan; exit 0 = PASS (it also fails a dump the app has not scrubbed yet)
python3 tools/mac/crash_canary.py scan ~/Library/Application\ Support/MediaViewer/Crashes/*/*.dmp \
    --forbid SECRET_FILENAME_canary_7Q3 --forbid PRIVATE_FOLDER_canary --forbid "$USER"

# the chrome path: an NSException raised in AppKit event handling, or a Swift trap
MV_CRASH_TEST=nsexception build/MediaViewer.app/Contents/MacOS/MediaViewer
MV_CRASH_TEST=swift_trap  build/MediaViewer.app/Contents/MacOS/MediaViewer

# the video threads (#233): the video decode thread crashes on the first frame of any clip;
# the dump's mv_decode_N slots read "role=video fmt=mov,mp4,... dec=<codec>/Lavc... WxH Nbit"
mkdir -p /tmp/mv-crash-canary/PRIVATE_FOLDER_canary
cp tools/testmedia/av_transport.mp4 /tmp/mv-crash-canary/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.mp4
MV_CRASH_TEST=video build/MediaViewer.app/Contents/MacOS/MediaViewer \
    /tmp/mv-crash-canary/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.mp4
```

`--av-soak` exits 0 only on a run of 1800 s or more whose drift slope stays
within 1 ms/min, with no position discontinuities and no host-clock gaps; 1 is a
real failure, 3 means the clip ended early, 4 means the run was too short to
count. Audio must be the master for a clip that has an audio track, or it fails.

**The drift figure only means something with the audio master.** On a clip with no
audio track the clock falls back to the host (`clock host fallback` in the F3
overlay) and the slope is measuring the host clock against itself, so a large
number there — tens of ms/min — is an artefact of the fallback, not a defect. Read
the `audio_master` column in the CSV before reading the slope. That is the whole
reason the corpus now carries an audio-bearing 31-minute clip.

## Package and install (PR 8)

> Cutting a release, and how updates reach people who already installed:
> **[RELEASING.md](../RELEASING.md)**. The short version — the wizard is a one-time
> download, and every update after it arrives in-app from GitHub Releases.

The v1 release is a **per-user** install under `%LocalAppData%\MediaViewer`, with **no
UAC** at any point. `Program Files` is not offered: a per-machine install needs elevation
for every update, which is how update mechanisms stop working
([docs/design/13](design/13-updates-and-telemetry.md)). There is no Microsoft Store channel — the
app is GPL-3.0-or-later ([docs/design/11](design/11-licensing.md)).

First install is an Inno Setup wizard; every later update is Velopack, in the background,
never re-opening the wizard.

### Building the release artefacts

Needs a built Release tree, plus two tools that are not in the repo:

```powershell
dotnet tool install -g vpk --version 1.2.0
winget install JRSoftware.InnoSetup

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\tools\package\build-release.ps1 -BuildDir build
```

That writes `dist\releases\` (the Velopack release set and the update manifest) and
`dist\MediaViewer-<version>-Setup.exe` (the wizard). It refuses to proceed if the app
breaks docs/design/09's 250 MB cap, fails docs/design/11's licence gate, or contains the Windows App SDK
AI / ONNX / DirectML / WebView2 files docs/design/13 forbids shipping.

The first run downloads the pinned .NET runtime (31.7 MB) once and caches it in the build
directory; its SHA-256 is verified every time. For an offline build, pass the same archive
with `-DotnetRuntimeZip`.

**No prerequisites on the target machine.** The payload carries both runtimes — the
Windows App SDK and .NET — so a clean Windows 10 21H2 install runs it with nothing
installed first. That is not a nicety: the wizard is per-user and takes no UAC, and a
machine-wide runtime prerequisite needs admin. docs/design/09 made the same call for .NET — "a
viewer whose whole pitch is 'point it at a folder and it works' cannot open with a runtime
prerequisite dialog". The app is **208.6 MB**, inside docs/design/09's stated 200–250 MB band; a
first install occupies **292.7 MB** on disk, because Velopack also keeps one full package
so a bad update can be rolled back.

**An artefact from that command is unsigned and not publishable.** It says so on its last
line. Signing needs credentials the repo does not and must not hold:

```powershell
.\tools\package\build-release.ps1 -BuildDir build `
  -SigningMetadata trusted-signing.json `     # Azure Trusted Signing
  -ManifestKey C:\offline\release.key         # Ed25519, signs the update manifest
```

`-NotesFile whats-new.txt` signs a "What's new" into the manifest (#620; write it with
`python3 tools/package/whats_new.py`, which holds it to the manifest's rule); the release
workflow passes the one it made. On the Mac, `macpack.py release --release-notes-file` embeds
the same text in the signed appcast.

Without Authenticode, Windows SmartScreen may warn. Without a correctly signed update
manifest, clients reject updates. The production public key is already pinned in
`src.managed/MediaViewer.Updater/UpdateKeys.cs`; preserve its matching private key.

### Linux packages (driftwire)

The `driftwire` command line's packages come from `cmake/driftwire-cli`
([docs/design/28](design/28-driftwire.md#from-a-terminal-driftwire)); CI and the release build them
on Ubuntu 22.04 with GCC 12 (`.github/actions/driftwire-cli-linux`), so they run from glibc 2.35 up.
vcpkg's BoringSSL (the Tunnel's TLS, linked in statically like libsodium; its Apache-2.0 notice is
in the packages' copyright file) needs `nasm` and Go to build: `sudo apt-get install nasm golang-go`.

```sh
cmake -S cmake/driftwire-cli -B build-cli -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DVCPKG_MANIFEST_DIR=tools/portable
cmake --build build-cli                 # fails if driftwire NEEDs more than libc, libm and the loader
(cd build-cli && cpack -G "DEB;RPM;TGZ" -B "$PWD/../dist")
# dist/driftwire_<ver>_amd64.deb, driftwire-<ver>-1.x86_64.rpm, driftwire-<ver>-linux-x86_64.tar.gz
# (arm64 / aarch64 on an arm64 machine; cpack needs rpmbuild, Ubuntu's `rpm` package)
```

Install and remove:

```sh
sudo apt install ./driftwire_<ver>_amd64.deb      # Debian, Ubuntu; sudo apt remove driftwire
sudo dnf install ./driftwire-<ver>-1.x86_64.rpm   # Fedora; sudo dnf remove driftwire
tar -xzf driftwire-<ver>-linux-x86_64.tar.gz --strip-components=1 -C ~/.local   # no root: ~/.local/bin/driftwire
```

They hold `/usr/bin/driftwire`, `driftwire(1)`, bash and zsh completions, a firewalld service
(`sudo firewall-cmd --permanent --add-service=driftwire`, the beacon's 47613/udp; the manual page
has the ufw lines and the TCP a receiver needs) and `/usr/share/doc/driftwire/copyright` with every
notice. They recommend libsecret (remembered devices need a Secret Service). The packages
themselves are not signed; people get them from the signed package repository below, which is
also how they are updated. The tarball is relative (`bin/`, `share/`), so it runs where it is
unpacked.

### Publishing the command line

How a person installs `driftwire` with no MediaViewer
([docs/design/28 "Installing"](design/28-driftwire.md#installing), [12](design/12-decision-log.md#driftwire),
2026-10-24):

```
Linux    curl -fsSL https://longtimeno-c.github.io/mediaviewer/driftwire/install.sh | sh
macOS    brew install longtimeno-c/tap/driftwire
Windows  irm https://longtimeno-c.github.io/mediaviewer/driftwire/install.ps1 | iex
```

On a Mac without Homebrew, the Linux line unpacks the signed macOS archive into `~/.local`
(it does not update itself; `sh -s -- --uninstall` removes it).

One repository variable publishes all three from every **stable** release: with
`MV_DRIFTWIRE_REPO` = `1`, `publish` requires and attaches the macOS archive and the Windows zip
that the `macos` and `windows` jobs pack on every run, `driftwire-repo` deploys the site (the apt
and dnf repositories, `install.sh`, the macOS archive, the Windows zip, `install.ps1` and
`latest.json`, which installed copies read to say a newer one is out) to GitHub Pages, and
`driftwire-homebrew` writes the tap's formula. Previews, and any release with it off, leave all
of that alone; the installers publish as before.

**The owner's one-time setup**, all of it (steps 1 and 2 in detail under [Linux package repository](#linux-package-repository) below). Steps 1–5 are one script, run on your own machine signed in to `gh` as an admin of the repository; it skips what is done already and stops at step 4 for the one token GitHub offers no API for:

```sh
tools/package/setup-driftwire-publishing.sh            # --dry-run shows what it would do
```

1. **The repository's signing key** (OpenPGP, RSA-4096): made in a throwaway GnuPG home, its
   armored private half in the repository secret `DRIFTWIRE_REPO_GPG_KEY` (and a passphrase, if
   any, in `DRIFTWIRE_REPO_GPG_PASSPHRASE`), with an offline backup ([the commands](#linux-package-repository)).
2. **GitHub Pages**: Settings → Pages → Build and deployment → Source: **GitHub Actions**; then
   on the `github-pages` environment a deployment rule for tags `v*`, since a release may run from
   its tag ([details](#linux-package-repository)).
3. **The tap repository**: a public repository named `homebrew-tap` under the same owner, with a
   README so it is not empty (the first release replaces a README that does not mention driftwire
   with `packaging/macos/homebrew-tap-README.md`'s):
   `gh repo create longtimeno-c/homebrew-tap --public --add-readme --description "Homebrew formulae for MediaViewer's command line"`.
4. **The tap's token**: a fine-grained personal access token (GitHub → Settings → Developer
   settings → Personal access tokens → Fine-grained tokens → Generate new token): resource owner
   `longtimeno-c`, repository access **Only select repositories** → `longtimeno-c/homebrew-tap`,
   permissions **Contents: Read and write** (Metadata: Read comes with it), nothing else. Put it in
   the repository secret `HOMEBREW_TAP_TOKEN`:
   `gh secret set HOMEBREW_TAP_TOKEN --repo longtimeno-c/mediaviewer` (it prompts). An expiry is
   fine; note the date, since an expired token fails `driftwire-homebrew` after the release is out
   (the formula then stays at the last version until a new token is set and the job re-run).
5. **The switch**: Settings → Secrets and variables → Actions → Variables → `MV_DRIFTWIRE_REPO` =
   `1` (`gh variable set MV_DRIFTWIRE_REPO --body 1 --repo longtimeno-c/mediaviewer`).
6. **Windows signing, for the signed `driftwire.exe` chosen on 2026-10-24.** The Mac side needs
   nothing new: the Apple secrets (`MV_MAC_CERT_P12_BASE64`, `MV_MAC_CERT_PASSWORD`, `APPLE_ID`,
   `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD`) that sign and notarize the app also sign and notarize
   the Mac program. Windows is signed only by the optional Trusted Signing secrets, all six:
   `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET`, `TRUSTED_SIGNING_ENDPOINT`,
   `TRUSTED_SIGNING_ACCOUNT`, `TRUSTED_SIGNING_PROFILE` ([RELEASING](../RELEASING.md#optional-windows-publisher-signing)),
   which sign the Windows installer too. The README says that installer is not Authenticode-signed,
   so they are not set up yet: without them the first release ships `driftwire.exe` and its DLLs
   unsigned, and `install.ps1` checks only the zip's SHA-256. With them, the release refuses an
   unsigned program and `install.ps1` checks every signature.

**What the first stable release then does.** The preflight refuses to start when the variable is
on and either secret is empty (it is told whether each is set, never its value). `windows` packs
the zip (signed when Trusted Signing is configured, step 6), `macos` the universal archive (signed
and notarized), and `publish` attaches both, with the three install lines in the release notes.
After that, `driftwire-repo` builds the site from the Linux packages and the zip as published,
checks it on localhost and deploys it, and
`driftwire-homebrew` downloads the macOS archive as published, writes `Formula/driftwire.rb` with
its SHA-256 (and the README) into the tap and pushes it as `github-actions[bot]`. From then on
the three lines above work, and every stable release updates them; `brew upgrade` and running the
Windows line again pick up the new version, apt and dnf with the system's updates. Neither job
holds up the installers' release; a failed one can be re-run from the run's page.

#### Linux package repository

Signed apt and dnf repositories, the tarballs and the one-line installer, as a static site on this
repository's GitHub Pages:

```sh
curl -fsSL https://longtimeno-c.github.io/mediaviewer/driftwire/install.sh | sh
curl -fsSL https://longtimeno-c.github.io/mediaviewer/driftwire/install.sh | sh -s -- --uninstall
```

`tools/package/driftwire_repo.py build` makes the site from one version's packages (and the
Windows zip, when it is there); release.yml's `driftwire-repo` job builds it from the release's
`linux-cli` packages and the published zip, checks it (`driftwire_repo.py smoke`) and deploys it,
after `publish`, on **stable** releases only and only when the repository variable
`MV_DRIFTWIRE_REPO` is `1`. Each deploy replaces the whole Pages site. The installer's source is
`packaging/linux/repo/install.sh`, the page's `packaging/linux/repo/index.html`. On macOS
`install.sh` installs nothing and prints the `brew install` line.

The same deploy carries the site's own pages from `packaging/site`, copied as they are: the front
page and the iPhone app's privacy and support pages (`/iphone/privacy/`, `/iphone/support/`), whose
URLs are in its App Store listing (`src.ios/AppStore/listing.md`). A change to them reaches the
site only with the next stable release's deploy.

**The key** (step 1 above), on a trusted machine with GnuPG 2.2 or newer:

```sh
export GNUPGHOME=$(mktemp -d)                    # a throwaway home: the key never sits in ~/.gnupg
gpg --quick-gen-key 'Driftwire packages <longtimeno-c@users.noreply.github.com>' rsa4096 sign never
gpg --list-secret-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }'   # the fingerprint
gpg --armor --export-secret-keys > driftwire-repo-key.asc      # asks for the passphrase, if one was set
gh secret set DRIFTWIRE_REPO_GPG_KEY --repo longtimeno-c/mediaviewer < driftwire-repo-key.asc
gh secret set DRIFTWIRE_REPO_GPG_PASSPHRASE --repo longtimeno-c/mediaviewer   # only with a passphrase; prompts
# Back up driftwire-repo-key.asc and $GNUPGHOME/openpgp-revocs.d/*.rev offline (not in git,
# not on this machine's disk), then:
rm -f driftwire-repo-key.asc; gpgconf --kill gpg-agent; rm -rf "$GNUPGHOME"; unset GNUPGHOME
```

**Pages** (step 2 above): **Settings → Pages → Build and deployment → Source: GitHub
Actions**. That makes the `github-pages` environment, whose rule deploys from `main` only; a
stable release may also run from its `v<version>` tag ([RELEASING.md](../RELEASING.md)), and the
repository job of such a run would be refused after the release is out ("Tag … is not allowed to
deploy to github-pages due to environment protection rules"), so allow tags too: **Settings →
Environments → github-pages → Deployment branches and tags → Add deployment branch or tag rule →
Ref type: Tag**, pattern `v*` (or `gh api --method POST
repos/longtimeno-c/mediaviewer/environments/github-pages/deployment-branch-policies -f name='v*' -f
type=tag`). With the variable set (step 5), the next stable release publishes the site; until
then the address serves nothing. RSA-4096 is what every rpm
verifies; `ed25519` instead of `rsa4096` also works for apt and current dnf. `never` because an
apt machine keeps the key it was given: an expiry would break every installed copy until the key
is fetched again. Losing the private key strands installed copies (they cannot get a signed index
from a new key without running the installer again); the revocation certificate is for a key that
leaks.

**What a release then does.** Its preflight refuses a stable release when the variable is on and
the secret is empty. It is told only whether the secret is set, never the key, which only the
repository job's signing step gets; it cannot see a secret of the `github-pages` environment, so
both secrets are repository secrets (`gh secret set --repo`, as above). `configure-pages` fails,
saying so, when Pages is not set to GitHub Actions. Every secret key in `DRIFTWIRE_REPO_GPG_KEY` signs (one passphrase for all, or none). The apt `Release` carries
`Valid-Until` a year ahead (`--valid-days`), so **a stable release must come within each year**,
or `apt update` fails on every installed machine ("Release file … is expired", exit 100) and
takes nothing new from the repository until the next one.

**Changing the key.** Make the new key as above and put both private keys in the secret, the old
one first (`{ gpg --armor --export-secret-keys OLD; gpg --armor --export-secret-keys NEW; } >
both.asc`, with the old key imported from its backup): every index is then signed by both,
`key.asc` and `key.gpg` hold both, and the `.rpm` files carry the old key's signature. dnf and
zypper ask to import the new key when they first meet it. An apt machine only learns the new key
when the installer runs again (or by hand: `curl -fsSL <site>/key.gpg | sudo tee
/etc/apt/keyrings/driftwire.gpg > /dev/null`), so keep both for at least one stable release and say so in its
notes; then put only the new key in the secret.

**Testing it here** (Debian or Ubuntu with `apt-utils createrepo-c rpm gnupg`; `smoke` installs
into this machine's apt and removes it again, so run it in a container or VM):

```sh
python3 tools/package/test_driftwire_repo.py -v              # tiny packages, throwaway keys
export GNUPGHOME=$(mktemp -d /tmp/dwg.XXXXXX)                 # short: gpg-agent's socket is in it
gpg --batch --passphrase '' --quick-gen-key 'Driftwire test <test@invalid>' rsa3072 sign 1d
fpr=$(gpg --with-colons --list-secret-keys | awk -F: '$1 == "fpr" { print $10; exit }')
python3 tools/package/driftwire_repo.py build --packages dist --out site \
  --base-url http://127.0.0.1:8765/driftwire --key-id "$fpr"
python3 tools/package/driftwire_repo.py smoke --site site    # serves it on 127.0.0.1 itself
# or by hand: python3 -m http.server 8765 --bind 127.0.0.1 --directory site &
#             curl -fsSL http://127.0.0.1:8765/driftwire/install.sh | sh
```

`DRIFTWIRE_REPO_URL` makes an installer use another copy of the site (https, or http on 127.0.0.1
or localhost).

#### The Windows zip and its one-liner

`tools/package/driftwire-windows.ps1` packs the zip from a Windows build (Release): `Stage` reads
the import tables of `driftwire.exe` and of each DLL it finds, and copies ours from
`build/bin/Release` and the Visual C++ runtime from the newest Visual Studio's
`VC\Redist\MSVC\<version>\x64\Microsoft.VC14*.CRT` (or `-RedistDir`); `Pack` checks that the
program runs from its folder and zips it. Between the two, a release signs `dist/driftwire-cli/sign`.

```powershell
./tools/package/driftwire-windows.ps1 -Action Stage -BuildDir build      # dist/driftwire-cli/driftwire-<ver>-windows-x64/
./tools/package/driftwire-windows.ps1 -Action Pack                       # dist/driftwire-cli/driftwire-<ver>-windows-x64.zip
./tools/package/driftwire-windows.ps1 -Action Imports -Path build/bin/Release/driftwire.exe
./tools/package/check-driftwire-install.ps1 -Zip (Get-Item dist/driftwire-cli/driftwire-*-windows-x64.zip)
```

`check-driftwire-install.ps1` serves `install.ps1` and the zip on 127.0.0.1 and runs the one-liner
as a person would, under the Restricted execution policy (a Windows client's default): it
installs and uninstalls in Windows PowerShell 5.1 and PowerShell 7, and runs the re-run, the update
and a changed zip in PowerShell 7 (CI's Build, msvc Release, also installs and uninstalls a build
signed by a throwaway certificate in each, `-SignedZip`). On Windows it installs into and
removes from **your own** profile and `Path`, and refuses when driftwire is already installed
there: run it on a CI runner or a throwaway account. Off Windows it runs the same in
`install.ps1`'s test mode (a scratch `LOCALAPPDATA`, the user's `Path` in a file, no
Authenticode), which is what `tools/package/test_driftwire_repo.py` does with stand-in zips; the
packer's tests are `python3 tools/package/test_driftwire_windows.py` (pwsh needed for both).
`driftwire_repo.py windows-site --zip Z --out SITE --base-url URL` writes the Windows half of the
site alone. By hand on a Windows machine, against a local copy:

```powershell
$env:DRIFTWIRE_REPO_URL = 'http://127.0.0.1:8765/driftwire'      # python -m http.server 8765 --bind 127.0.0.1 --directory SITE
irm $env:DRIFTWIRE_REPO_URL/install.ps1 | iex
& ([scriptblock]::Create((irm $env:DRIFTWIRE_REPO_URL/install.ps1))) -Uninstall
```

#### The macOS archive and the Homebrew tap

`tools/mac/macpack.py cli` signs the program, checks that it says the version, notarizes it in a
zip unless `--skip-notarize`, and packs `driftwire-<ver>-macos-<universal|arm64>.tar.gz`; the
release joins the two architectures with `tools/mac/lipo_merge.py` first.
`tools/package/homebrew_formula.py render` writes the formula for one archive.

```sh
python3 tools/mac/macpack.py cli --binary build/bin/driftwire --version 0.1.37 --identity - --skip-notarize
python3 tools/mac/macpack.py cli --binary build/cli/driftwire --version 0.1.37 \
  --identity "Developer ID Application: …" --notary-profile mediaviewer-notary
python3 tools/package/homebrew_formula.py render --version 0.1.37 \
  --archive build/release/cli/driftwire-0.1.37-macos-universal.tar.gz \
  --url https://github.com/longtimeno-c/mediaviewer/releases/download/v0.1.37/driftwire-0.1.37-macos-universal.tar.gz \
  --out /tmp/driftwire.rb
# On a Mac, once that asset is published (Homebrew installs formulae from a tap only):
brew tap-new --no-git local/test && cp /tmp/driftwire.rb "$(brew --repository local/test)/Formula/"
brew install local/test/driftwire && brew test local/test/driftwire && brew audit local/test/driftwire
brew untap local/test
python3 tools/package/test_homebrew_formula.py -v       # ruby -c, and the formula evaluated against a stand-in
python3 tools/mac/test_macpack.py -v
```

### Continuous integration

Pushes to `main` and pull requests build four Windows variants (MSVC Release, MSVC Debug, clang-cl, ASan), run the C# ABI smoke test, and fuzz each decoder for a minute. On macOS they build the native core on macos-15 (arm64) and run the full `ctest` suite (`mv_tests`, `mv_import_tests`, `mv_ai_tests` and the Mac-only tests), build the three SwiftPM chrome packages, and run `swift test` on `src.swift/AIChrome`. The Mac job shares its vcpkg cache key with the release workflow's arm64 leg, so a cold cache costs the whole Mac manifest once. On Linux, `portable-core` runs the shared suites under ASan / UBSan (Driftwire's among them, with a Secret Service of the job's own), and `driftwire-cli-linux` builds the command line's packages on x64 and arm64 (Ubuntu 22.04, GCC 12), runs `"[cli]"`, installs the `.deb`, builds the package repository from the packages with a throwaway key and installs from it on localhost (`driftwire_repo.py smoke`, with `test_driftwire_repo.py` and shellcheck), and uploads the three packages. The Windows MSVC Release leg packs the `driftwire` zip as a release does and runs its one-line installer against it on localhost under the Restricted execution policy: it installs in both PowerShells, unsigned and signed by a throwaway certificate, and uninstalls in both; the re-run, the update and a changed zip run in PowerShell 7 (`check-driftwire-install.ps1`); the Mac job packs the arm64 archive with `macpack.py cli` (ad hoc). Both upload them as artefacts. A newer push to the same branch cancels the run it replaces. Dependencies are installed by one job and reused. The first run after `vcpkg.json` or the pinned vcpkg baseline changes spends about 45 minutes there, mostly building FFmpeg. Later runs restore that cache. The nightly schedule is the long fuzz run, separate from pull requests.

### Releasing from CI

Use **Actions → Release → Run workflow**. Pushes and tags do not start release packaging.
Both platforms use the exact `x.y.z` from `CMakeLists.txt`.

| Mode | Result |
|---|---|
| `artifacts` | Unsigned installers in Actions artifacts; no GitHub Release |
| `preview` | Both unsigned test installers on GitHub Releases as a prerelease |
| `stable` | Windows installer, notarized Mac image, both signed update feeds and checksums on Latest |

Windows and macOS build independently; a final job publishes only after both succeed. The
driftwire Linux packages (`linux-cli`, x64 and arm64) build and are tested beside them on every
run; after a stable release, `driftwire-repo` publishes them in the package repository on GitHub
Pages once `MV_DRIFTWIRE_REPO` is `1`, and the same switch attaches the command line's macOS
archive and Windows zip to a stable release, puts the zip and `install.ps1` on the site and
updates the Homebrew tap ([Publishing the command line](#publishing-the-command-line)). The Linux
packages are not attached to the release (`MV_RELEASE_LINUX_CLI`, [RELEASING.md](../RELEASING.md)).
Stable needs the Windows manifest key and six Mac signing/notarization secrets.
Windows Authenticode remains optional. Preview needs no signing secrets; its Mac app
must be replaced manually with a stable build later.

See **[RELEASING.md](../RELEASING.md)** for commands, credentials, assets, version rules and
recovery steps. There is no automatic publishing on push or run-number version stamping.

### Installing and uninstalling

The wizard is six pages and no more: Welcome, Licence (GPL, scroll and accept), Location,
Options (Start Menu **on**, Desktop **off**), Progress, Finish (Launch, GitHub, Licence, and
a checked *Choose MediaViewer as the default for all supported photos and videos*).
It registers MediaViewer for the D5 photo and video types (per-user, removed on uninstall),
so it appears in **Open with** and in Settings > Default apps. Windows does not let an
installer set the default itself: leave the Finish-page box checked to open Default apps on
MediaViewer and confirm there, or untick it to skip. It does **not** ask about telemetry, which is a first-run
screen inside the app.

Uninstall is from Apps & features, and removes the shortcuts, the registry entry, the
whole install directory, the thumbnail cache and the add-ons (`addons\<id>\<version>`, the
Local search models included). It then asks, defaulting to No, whether to also delete each
add-on's `data\` (the Local search index, Import's history); a silent uninstall keeps it.

Running Setup over an existing install keeps `addons\`, `thumbs\`, `metadata-snapshots\`,
`Crashes\`, `clipboard\`, `telemetry\`, `settings.ini` and `import-hint.dismissed`. Velopack's
`--installto` clears the directory, so the wizard moves those into a sibling
`MediaViewer.keepN` first and puts them back afterwards. If one cannot be moved, MediaViewer
is still running and Setup stops before anything is deleted.

The chrome registers its UI font session-wide, so DirectWrite can see it. On an installed
build it registers the copy in `<root>\fonts\`, never the file in `current\`. Windows keeps a
registered font open until sign-out if the process dies before `Detach` (a crash, a kill, or
the `TerminateProcess` exit under a running add-on). Registered from `current\`, that open
file made every in-app update fail on Velopack's rename of `current\`, and made Setup exit
with "failed with code 1". Setup and uninstall both unregister any copy left in `current\` or
`fonts\` before they delete anything. Apps & features shows the payload plus the retained
package (`UninstallDisplaySize`, passed in by `build-release.ps1`); add-ons are not counted.

```powershell
# unattended, e.g. on a test VM
.\dist\MediaViewer-0.1.0-Setup.exe /VERYSILENT /DIR="C:\path\to\install" /TASKS=startmenu
```

### Updates

```powershell
# the local end-to-end check: staging, signature rejection, and rollback
.\tools\package\e2e-update.ps1 -Payload .\build\bin\Release
```

It packs four versions into a temp feed, installs one, proves a **tampered manifest
stages nothing**, proves a signed one downloads in the background and applies on exit
without showing the wizard, proves nothing is applied while **a second copy** runs (#532:
Update.exe would kill it) and that the copy, asked to quit for the update, quits and applies
it, and proves a build that fails to start twice **rolls back** to its predecessor on the
third try.

### Telemetry

**Off by default, and it stays off unless you turn it on.** One first-run screen, two
buttons, neither preselected; dismissing it leaves telemetry off. Settings has the same
switch, and turning it off deletes the random install id and anything not yet sent.

Nothing about your files ever leaves the machine — no paths, filenames, folder names,
thumbnails, pixels or EXIF. That is enforced by the shape of the payload rather than by
care: an event is an id from a fixed table, a tag from a fixed vocabulary, and named
integers. There is no free-text field to put a filename in.
`tools/telemetry-schema-check.ps1` fails the build if that ever changes.

There is no upload endpoint yet, exactly as there is none for crash reports.

## Layout

```
src/core        job system, result<T>, lock-free rings, ETW
src/io          whole-file reads, directory listing + watcher, copy/move that never
                overwrites, Recycle Bin (Windows impl), the replace port for edited
                pixels (replace_win / replace_mac: never-overwrite export, atomic swap)
src/codec       JPEG / PNG / BMP / GIF / WebP / TIFF / ICO / HEIC / AVIF / RAW,
                APNG walker, frame-at-a-time animation sources, magic-byte probe,
                HEIC-only OS-codec probe (WIC, os_decode_win.cpp)
src/image       LCMS colour (8-bit display LUT; sRGB copy-through), CPU mips,
                immutable GPU upload, JPEG-512 thumbs
src/meta        EXIF / IPTC / XMP (Exiv2) + container / stream (libavformat) read
                model, summary rows, AF geometry (PR 9)
src/geo         offline reverse geocoder over the place database (the Places
                add-on links it; docs/design/27)
src/edit        EditStack + geometry (the blit's output -> source map), lossless
                JPEG rotate / flip / MCU crop, JPEG / PNG export with a metadata
                policy (PR 10)
src/canvas      pan/zoom springs, fit / fill / 100 %, sticky zoom
src/gfx         D3D11 device, flip-model swapchain, frame pacer, blit
src/addon       the add-on host: MediaViewer's own add-ons (signed manifests, store,
                loader, host function table) and open add-ons from other makers
                (package reader, manifest schema 2, publisher keys, themes; the Lua
                script sandbox, its limits and commands, script_*; docs/design/25)
third_party/lua Lua 5.4.8, vendored, static (MIT); without io/os/package/debug
                (third_party/lua/VENDOR.txt)
src/addons/driftwire/cli
                the driftwire command line: terminal port (term_posix / term_win),
                keys, line editor, paths, the look (FTXUI), the live region, the
                session over the engine, main() (docs/design/28)
src/abi         the flat C ABI — the top of the native graph; animation session
src/shell       Win32 window, render thread, present lab, hostfxr island host,
                key router and live command table, marks, slideshow, file jobs,
                Settings persistence
src.managed/    C# interop and WinUI chrome (hosted as an island, not the app):
                command bar, filmstrip, gallery, `?` sheet, Settings,
                file drag/drop
tests/          Catch2 suites for core, gfx, codec, colour, camera, ABI, folder,
                key router, file ops, slideshow, animation
tools/          frametime harness, module-graph, hostable-core, and licence gates;
                addon-sdk (MIT): the tool people make add-ons with; geo/build_places.py
                (the place database from pinned Natural Earth + GeoNames data)
examples/       an add-on that packs and installs as it is (MIT)
cmake/driftwire-cli  the driftwire Linux packages (CPack, the NEEDED gate)
packaging/linux the driftwire manual page, completions, firewalld service, copyright;
                repo/: the one-line installer and the package repository's page
tools/package/driftwire_repo.py  the signed apt and dnf repository site, and its smoke check
docs/design/    how each subsystem works (design reference)
docs/plans/     forward plans not built yet
```

Dependencies point downward only —
`shell → abi → {canvas, edit, player, image, meta} → {codec, gfx, io} → core`. No back-edges,
and nothing may depend on `shell`. That is what keeps the core testable with no window, and
`tools/check-module-graph.ps1` enforces it on every push.

## Design reference

[docs/design/](design/README.md) describes how each subsystem works; code comments cite it as
`docs/design/NN §section`. Forward plans are in [docs/plans/](plans/).

## The iPhone app's link (src/mobile, src.ios)

The core: `mv_mobile_link_tests "[mobile-link]"` (the Mac build's `build/bin`), filter by tag. The app:
build `build-ios-sim/` (`cmake/ios.cmake`), then the `MediaViewer` scheme. End to end against
`driftwire serve` on loopback in a Simulator of your own: `tools/ios/link-e2e.sh <udid> <folder>`
after copying the built `MediaViewer.app` into `<folder>`. Codes for a service come from
`driftwire serve pair-code [SECONDS]` (`--for 24h` for a guest code) until the desktop claim lands.
The owner's passphrase and timed phones on the service: `driftwire serve init --passphrase-stdin` or
`--no-passphrase` when no terminal can ask, `serve status`, `serve passphrase set|reset`,
`serve grant NAME --for D|--forever` (`docs/design/28-driftwire.md` "The owner's passphrase"). Tests:
`mv_import_tests "[driftwire-serve],[phone-pairing]"`; the iOS copy in `MobileUITests` (the package's
test scheme links the iOS core from `build-ios-iphonesimulator/`).
