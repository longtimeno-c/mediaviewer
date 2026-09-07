# PR 5 — working contract

Temporary coordination file for the three sessions building PR 5. **Delete before review.**
It is not spec (`plan/` is) and not the landing page (`README.md` is).

Branch `pr5-video`, off `5eaa530`. One shared checkout: `C:\Users\tphwo\Documents\repo\mediaviewer`.

## Verify lines — quote yours before you start

- **5a** "4K 10-bit HEVC and AV1 play at full rate with GPU video decode > 0 in Task Manager, on a
  clean VM with no Store codec packs; an iPhone HLG clip looks correct rather than washed out; the
  decoder never stalls waiting for a surface over a 10-minute play; photo → video → photo leaks no
  textures."
- **5b** "A/V drift flat over 30 minutes, with the overlay to prove it; unplugging the audio device
  mid-playback recovers without stopping video; a clip with no audio track plays at correct speed."
- **5c** "scrubbing feels instant; frame step lands on exact frames in both directions; media keys
  and the OS overlay work; resume returns to the right position."
- **All three, standing:** PR 1's present-loop verify still holds.

## File ownership — do not edit outside your set

| Owner | Files |
|---|---|
| **A** — `mediaviewer-48` (5a) | `src/player/{demux,video_decode,frame_ring,video_source,media_source}.cpp` · `src/player/hwdecode_win.cpp` · `src/gfx/video_blit.cpp` + shaders · `tests/test_video_*.cpp` |
| **B** — `mediaviewer-08` (5b) | `src/player/{audio_decode,av_clock}.cpp` · `src/player/audio_win.cpp` · `src/shell/av_soak.cpp` · `tests/test_av_clock.cpp` `tests/test_audio_*.cpp` |
| **C** — `mediaviewer-56` (5c + integration) | `src/player/transport.cpp` `src/player/presenter.cpp` · all `src/abi/**` `src.managed/**` `src/shell/**` · `CMakeLists.txt` `vcpkg.json` `.gitignore` `README.md` `plan/**` `tools/**` |

**Headers are contract and belong to C.** `media_source.h`, `video_source.h`, `audio_sink.h`,
`audio_block.h`, `av_clock.h`, `presenter.h`, `gfx/colour_desc.h`. Need a change? Message C.
Do not edit another owner's file, do not edit `CMakeLists.txt`, do not commit to `main`,
do not `git checkout` another branch — the working tree is shared.

Every file you own already exists as a stub and is already listed in `CMakeLists.txt`.
Fill the stub in. If you need an **additional** file, message C.

## Conventions that are already decided

- **Time is `int64` nanoseconds everywhere in `player/`**, suffixed `_ns` (`player::time_ns`).
  Chosen over microseconds so audio accounts in whole samples: at 48 kHz one sample is 20833.33 ns.
- **PTS is stream-relative** — the container `start_time` is subtracted by 5a before anything else
  sees it, and kept in `video_stream_info::start_time_ns` for diagnostics. A non-zero `start_time`
  that is not subtracted reads as a constant A/V offset and gets blamed on the clock.
- **Generation counters**: every frame and audio block carries the view generation. Navigation and
  seek bump it; stale items are discarded at acquire, never presented.
- **`playback_rate` lives in the clock from day one**, even though speed is 5c's feature. Retrofitting
  a scale factor into a running clock is the ugly version of this.
- **Cadence holds and starvation holds are different numbers, permanently.** 24p on a 60 Hz display
  holds most vblanks and that is correct 3:2 pulldown; merging the counters makes healthy playback
  read as broken.
- **`clock_stats` is published wait-free** via `publish_slot<clock_stats>` from `core/spsc_ring.h`.
  POD only. `player/` never makes an ImGui call — all ImGui stays in `shell/present_lab.cpp`.

## Module rules — CI, not etiquette

`tools/check-module-graph.ps1` and `tools/check-hostable-core.ps1` both must exit 0.

- `player/` may include from `player, codec, gfx, io, core`. **Never** `shell/`, `image/`, `abi/`.
- `player/*.h` and `player/*.cpp` may **not** directly include `d3d11.h`, `dxgi.h`, `windows.h`,
  `audioclient.h`, `mmdeviceapi.h`, `audiopolicy.h`, `mmreg.h`, `avrt.h`, `endpointvolume.h`,
  `functiondiscoverykeys_*`, `combaseapi.h`, `objbase.h`, `wrl/*`, `d3d12.h`.
  **Only `player/*_win.cpp` is exempt** — the checker skips that filename suffix. Hence
  `hwdecode_win.cpp` and `audio_win.cpp`. That exemption is the whole D9 port boundary: a Metal or
  Core Audio host replaces those two files and nothing else.
  *(The audio headers were added to that ban on 2026-09-07 — before then it was convention, not CI.)*
- Transitive D3D11 through `gfx/device.h` is fine; `image/gpu_image.h` already does it.
- **No FFmpeg type in any header outside `player/`.** `AVFrame*` never reaches `gfx/` or `abi/`.
- `/W4 /WX /permissive- /GR- /utf-8`. No exceptions on the hot path — `mv::result<T>` / `mv::expected`
  from `core/result.h`. No RTTI. No `std::shared_ptr` in the render loop.

## Build

FFmpeg is already in `vcpkg.json` and already built into the shared vcpkg binary cache, so configure
is fast. Use **your own build directory** — three concurrent CMake runs into `build/` will fight.

```
cmake -S . -B build-pr5a -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=C:/Users/tphwo/vcpkg/scripts/buildsystems/vcpkg.cmake -DMV_BUILD_TESTS=ON
cmake --build build-pr5a --config RelWithDebInfo
```

`cmake` is not on PATH; it is `C:\Program Files\CMake\bin\cmake.exe`.

## Test media

`tools/testmedia/` — in the repo so every session can reach it, gitignored so it never enters
history (`plan/09`: the corpus does not go in git). Generate with the system FFmpeg at
`D:\Tools\ffmpeg\...\bin\ffmpeg.exe`. That build is GPL; it is a **tool**, never a link target, and
`tools/licence-check.ps1` is now scoped to `build*/vcpkg_installed` so it can never see it.

Nothing on this machine is 10-bit, HDR, AV1, HEVC or 4K, and there is no iPhone footage — every clip
found is untagged 8-bit H.264. Generate: 4K 10-bit HEVC (p010le), 4K 10-bit AV1, HLG
(`bt2020nc`/`arib-std-b67`), PQ (`smpte2084`), one deliberately **untagged** 8-bit, one **full-range**.
The untagged and full-range pair matter as much as the HDR ones — they are what catches an
"assume BT.709 limited" bug, and they are the only kind of clip this box actually has.

## Definition of done — per owner

1. Builds clean at `/W4 /WX` in your own build dir.
2. `ctest` green, including tests you wrote. A test that asserts nothing is not a test.
3. `tools/check-module-graph.ps1` and `tools/check-hostable-core.ps1` exit 0.
4. `tools/licence-check.ps1` exits 0.
5. PR 1's present-loop verify still holds (`tests/test_frametime.ps1`).
6. **Report what you actually ran, with output.** "Not run" is an acceptable and useful answer.
   "Should pass" is not. Say plainly which parts of your verify line you could not exercise on this
   machine and why — several of them genuinely cannot be, and that goes to the user as a caveat
   rather than being quietly marked done.

## Known-shaky ground under us

PR 4's verify was never run by anyone, and the plan edits in `5eaa530` are unreviewed — see the
row in `plan/12-decision-log.md`. If you hit something in the folder/filmstrip/thumbnail layer that
looks wrong, it may genuinely be wrong. Report it, don't silently work around it.
