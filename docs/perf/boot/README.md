# Boot timeline reports (issue #486)

Written by `tools/perf/boot.ps1` from `mediaviewer_lab --boot-report` launches on the Windows desktop
(AMD Ryzen 7 5700X3D, RTX 4070, Windows 11 26200), Release Ninja builds, warm cache, one warm-up
launch per build discarded, builds alternated. Times are ms from process creation; -1 is a stage
the launch did not reach. Stages: `src/core/boot_timeline.h`.

| File | What |
|---|---|
| `windows-empty.json` | no file, 10 runs: build 0 = base (the timeline only, 86ba2f3f), build 1 = chrome preload |
| `windows-arw.json` | the 42 MP Sony ARW alone in a folder, 10 runs, same two builds |
| `windows-cr3.json` | the Canon R6 CR3 alone in a folder, 12 runs: base, preload at normal priority (not shipped), preload below normal (shipped) |
| `windows-empty-baseline-6.json` | the first six base runs, no file |
| `windows-first-launch-cold.json` | the first launch after a build: the only cold sample |
| `gate-<n>-<build>.json` | PR 1 gate, `--soak 60 --gate`, base / new alternated, on a loaded machine (fails on both) |

The Mac files come from `tools/perf/boot.py` on an Apple M5 MacBook (Mac17,4), the bare
`mediaviewer_lab` from Release Ninja builds, warm, one warm-up launch per build discarded, builds
alternated. Build 0 = base (the Mac marks only, 8b5061ab), build 1 = lazy hidden panes.

| File | What |
|---|---|
| `mac-empty.json` | no file, 10 runs |
| `mac-arw.json` | the 42 MP Sony ARW alone in a folder, 10 runs |
| `mac-cr3.json` | the Canon R6 CR3 alone in a folder, 10 runs |
| `mac-gate-<n>-<build>.json` | Mac PR 1 gate, `--soak 60 --gate`, base / new alternated |
