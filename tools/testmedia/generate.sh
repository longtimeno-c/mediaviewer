#!/usr/bin/env bash
# PR 5 test corpus generator.
#
# THE CLIPS are gitignored (plan/09: the corpus does not go in git). THIS
# SCRIPT is tracked — see .gitignore. Ignoring the whole directory used to take
# the generator with it, so a clean clone had no clips and no way to make them,
# and every real-clip test skipped while ctest still reported success.
#
# LICENCE: this drives whatever ffmpeg you already have as an external *tool*,
# never a link target. x264/x265 here are a command you run, not something the
# app is built against, so tools/licence-check.ps1 (scoped to the vcpkg install
# tree) is correct not to look at it. Do NOT confuse this with the LGPL FFmpeg
# the app links (plan/11, D3).
#
# THE CLIPS ARE SYNTHETIC. None of them is an iPhone HLG clip, and one real one
# is worth more than this whole set for the "looks correct rather than washed
# out" clause of 5a's verify.
#
# Usage:
#   ./generate.sh              # ffmpeg from $FFMPEG, else from PATH
#   FFMPEG=/path/to/ffmpeg.exe ./generate.sh
#   ./generate.sh --list       # print the clips and exit, generate nothing
set -u

OUT="$(cd "$(dirname "$0")" && pwd)"
V="-hide_banner -loglevel error -y"

# Every clip this corpus is supposed to contain, and the test that needs it.
# Kept next to the generator so `--list` and the CMake corpus check agree with
# what is actually produced below.
CLIPS="
hevc_4k_10bit_bt709.mp4        tests/test_video_ring.cpp, test_video_colour.cpp — P010 + 4K decode rate
hevc_4k_8bit_bt709.mp4         tests/test_video_ring.cpp — NV12 path, texture-leak cycles
av1_4k_10bit_bt709.mp4         tests/test_video_ring.cpp — second half of 5a's codec pair
hlg_4k_10bit.mp4               tests/test_video_colour.cpp — HLG tone-map branch
pq_4k_10bit.mp4                tests/test_video_colour.cpp — PQ tone-map branch
hevc_4k_10bit_fullrange.mp4    tests/test_video_colour.cpp — 'do not assume limited range'
untagged_1080p_8bit.mp4        tests/test_video_colour.cpp — resolve_unspecified path
fullrange_1080p_8bit.mp4       tests/test_video_colour.cpp — 8-bit full range
av_transport.mp4               tests/test_video_transport.cpp — seek/step/resume + the video ABI
soak_10min_1080p_hevc.mp4      tests/test_video_ring.cpp — 5a's 10-minute no-stall soak
soak_31min_1080p_hevc_aac.mp4  5b's 30-minute A/V drift soak (--av-soak 1860)
"

if [ "${1:-}" = "--list" ]; then
  printf '%s\n' "$CLIPS"
  exit 0
fi

# ---------------------------------------------------------------------------
# Find ffmpeg. The old script hardcoded an absolute D:\ path on one developer's
# machine, which is the other half of why nobody else could build this corpus.
# ---------------------------------------------------------------------------
FF="${FFMPEG:-}"
if [ -z "$FF" ]; then
  FF="$(command -v ffmpeg || true)"
fi
if [ -z "$FF" ] || ! "$FF" -hide_banner -version >/dev/null 2>&1; then
  echo "generate.sh: no usable ffmpeg." >&2
  echo "  Put one on PATH, or set FFMPEG=/path/to/ffmpeg.exe" >&2
  echo "  Any recent build works; this is a tool, not a link target." >&2
  exit 1
fi
echo "ffmpeg: $FF"

encoders="$("$FF" -hide_banner -loglevel error -encoders 2>/dev/null)"
has() { printf '%s' "$encoders" | grep -q "[[:space:]]$1[[:space:]]"; }

# NVENC when the box has it (seconds, not minutes, for 4K), software otherwise.
# The clips only have to be DECODABLE and correctly TAGGED — nothing in the
# test suite cares which encoder produced them.
if has hevc_nvenc; then
  HEVC="-c:v hevc_nvenc"; HEVC8="-profile:v main"; HEVC10="-profile:v main10"
elif has libx265; then
  HEVC="-c:v libx265 -preset ultrafast"; HEVC8=""; HEVC10=""
else
  echo "generate.sh: no HEVC encoder (need hevc_nvenc or libx265)." >&2; exit 1
fi

if has h264_nvenc; then
  H264="-c:v h264_nvenc -profile:v high"
elif has libx264; then
  H264="-c:v libx264 -preset ultrafast -profile:v high"
else
  echo "generate.sh: no H.264 encoder (need h264_nvenc or libx264)." >&2; exit 1
fi

# libaom at 4K is measured in hours, so it is the last resort, not the default.
if has av1_nvenc; then
  AV1="-c:v av1_nvenc"
elif has libsvtav1; then
  AV1="-c:v libsvtav1 -preset 12"
elif has libaom-av1; then
  AV1="-c:v libaom-av1 -cpu-used 8 -row-mt 1"
  echo "  note: falling back to libaom-av1; the 4K AV1 clip will take a while."
else
  echo "generate.sh: no AV1 encoder (need av1_nvenc, libsvtav1 or libaom-av1)." >&2; exit 1
fi

if ! has aac; then
  echo "generate.sh: no AAC encoder; av_transport.mp4 and the 5b soak need one." >&2
  exit 1
fi

# Moving content, not a still: a static frame hides both a decode-rate problem
# and a chroma-plane bug.
SRC4K="-f lavfi -i testsrc2=size=3840x2160:rate=30:duration=20"
SRCHD="-f lavfi -i testsrc2=size=1920x1080:rate=30:duration=20"
# Ramps + bars are what make a wrong matrix / wrong range visibly wrong rather
# than subtly wrong.
BARS4K="-f lavfi -i smptehdbars=size=3840x2160:rate=30:duration=20"

fail=0
gen() {
  name="$1"; shift
  echo "  $name"
  if ! "$FF" $V "$@" "$OUT/$name"; then
    echo "  FAILED: $name" >&2
    fail=1
  fi
}

echo "4K 10-bit HEVC (p010le, bt709) — the P010 path and the 4K decode rate"
gen hevc_4k_10bit_bt709.mp4 $SRC4K $HEVC $HEVC10 -pix_fmt p010le \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -b:v 40M

echo "4K 8-bit HEVC (nv12) — the NV12 path at 4K, same clip otherwise"
gen hevc_4k_8bit_bt709.mp4 $SRC4K $HEVC $HEVC8 -pix_fmt yuv420p \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -b:v 30M

echo "4K 10-bit AV1 — second half of the verify's codec pair"
gen av1_4k_10bit_bt709.mp4 $SRC4K $AV1 -pix_fmt p010le \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -b:v 30M

echo "4K 10-bit HEVC HLG (bt2020nc / arib-std-b67) — must NOT look washed out"
gen hlg_4k_10bit.mp4 $BARS4K $HEVC $HEVC10 -pix_fmt p010le \
  -colorspace bt2020nc -color_primaries bt2020 -color_trc arib-std-b67 -color_range tv -b:v 40M

echo "4K 10-bit HEVC PQ (bt2020nc / smpte2084) — the other tone-map branch"
gen pq_4k_10bit.mp4 $BARS4K $HEVC $HEVC10 -pix_fmt p010le \
  -colorspace bt2020nc -color_primaries bt2020 -color_trc smpte2084 -color_range tv -b:v 40M

# Was never in this script, yet tests/test_video_colour.cpp:167 requires it, so
# it only ever existed as a hand-made file on one machine.
echo "4K 10-bit HEVC FULL RANGE — 10-bit half of 'do not assume limited range'"
gen hevc_4k_10bit_fullrange.mp4 $BARS4K -vf scale=in_range=full:out_range=full \
  $HEVC $HEVC10 -pix_fmt yuv420p10le -color_range pc \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -b:v 40M

echo "1080p 8-bit H.264, DELIBERATELY UNTAGGED — the resolve_unspecified path"
gen untagged_1080p_8bit.mp4 $SRCHD $H264 -pix_fmt yuv420p \
  -bsf:v 'filter_units=remove_types=6' -b:v 10M

echo "1080p 8-bit H.264 FULL RANGE — catches 'assume limited'"
gen fullrange_1080p_8bit.mp4 $SRCHD -vf scale=in_range=full:out_range=full \
  $H264 -pix_fmt yuv420p -color_range pc \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -b:v 10M

# Also never in this script, and it is the only clip with an audio track.
# The transport tests pin its shape: 640 wide (the ABI test asserts it), 30 fps
# (frame step asserts a step is <= 33.334 ms), exactly one audio and one video
# stream, and long enough that a seek to 20 s has somewhere to land.
echo "45 s 640x360 H.264 + AAC — seek, frame step, resume, and the video ABI"
gen av_transport.mp4 \
  -f lavfi -i testsrc2=size=640x360:rate=30:duration=45 \
  -f lavfi -i sine=frequency=440:sample_rate=48000:duration=45 \
  $H264 -pix_fmt yuv420p -b:v 2M \
  -c:a aac -ac 1 -ar 48000 -b:a 96k \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv

echo "10-minute 1080p HEVC — 5a's 'decoder never stalls over a 10-minute play'"
gen soak_10min_1080p_hevc.mp4 \
  -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=600 \
  $HEVC $HEVC8 -pix_fmt yuv420p \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv -b:v 6M

# 5b's verify is "A/V drift flat over 30 minutes", and until now there was no
# clip in the corpus that could carry it: av_transport.mp4 is 45 s and every
# other clip is silent, so the soak ran on the QPC fallback with audio_master=0
# and proved nothing about the audio master clock.
#
# 1860 s, not 1800: src/shell/av_soak.cpp returns 4 ("diagnostic, not a
# verification") for anything under 1800 s, so the clip has to outlast the run
# rather than end level with it. 720p at 2 Mbps keeps it near 500 MB.
#
# CAVEAT, and it belongs in the PR text: a synthetic sine against synthetic
# video exercises the drift_tracker slope gate — which av_clock.h calls THE
# gate — but it does not stress endpoint-crystal vs container-timebase
# mismatch the way real camera footage does. This satisfies 5b's verify as
# written, not its intent. A real 30-minute phone clip is worth more.
echo "31-minute 720p HEVC + AAC — 5b's 30-minute A/V drift soak"
gen soak_31min_1080p_hevc_aac.mp4 \
  -f lavfi -i testsrc2=size=1280x720:rate=30:duration=1860 \
  -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1860 \
  $HEVC $HEVC8 -pix_fmt yuv420p -b:v 2M \
  -c:a aac -ac 2 -ar 48000 -b:a 128k \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv

if [ "$fail" -ne 0 ]; then
  echo "generate.sh: one or more clips failed to encode" >&2
  exit 1
fi
echo done
