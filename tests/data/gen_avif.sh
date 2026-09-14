#!/bin/sh
# Generate tiny licence-clean AVIF fixtures for tests/data/avif/ with ffmpeg
# (libaom-av1 encoder, avif muxer). Dev tool only.
# usage: gen_avif.sh <outdir>   (irot/ICC/animated come from gen_avif_pillow.py)
set -u
OUT="$1"
mkdir -p "$OUT"
cd "$OUT" || exit 1

PAT='color=red:s=64x32:r=1,drawbox=x=32:y=0:w=32:h=32:color=blue:t=fill,drawbox=x=0:y=0:w=8:h=8:color=lime:t=fill'
Q='-hide_banner -loglevel error -y'

ffmpeg $Q -f lavfi -i "$PAT" -frames:v 1 -c:v libaom-av1 -still-picture 1 -crf 0 -pix_fmt yuv444p \
  -color_primaries bt709 -color_trc iec61966-2-1 -colorspace bt709 -color_range pc -f avif srgb_8bit.avif
echo "still=$?"

ffmpeg $Q -f lavfi -i "$PAT" -frames:v 1 -c:v libaom-av1 -still-picture 1 -crf 0 -pix_fmt yuv444p10le \
  -f avif gradient_10bit.avif
echo "ten=$?"

ffmpeg $Q -f lavfi -i 'color=red:s=16x16:r=1' -frames:v 1 -c:v libaom-av1 -still-picture 1 -crf 0 \
  -pix_fmt yuv444p -color_primaries smpte432 -color_trc iec61966-2-1 -colorspace bt470bg -color_range pc \
  -f avif p3_nclx.avif
echo "p3=$?"

for f in *.avif; do
  printf '%s size=%s irot=%s imir=%s colr=%s prof=%s avis=%s\n' "$f" "$(wc -c < "$f")" \
    "$(grep -c irot "$f")" "$(grep -c imir "$f")" "$(grep -c colr "$f")" "$(grep -c prof "$f")" "$(grep -c avis "$f")"
done
