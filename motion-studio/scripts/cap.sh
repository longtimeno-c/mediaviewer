#!/bin/sh
# cap.sh WINDOW_ID NAME [COUNT INTERVAL]: capture a MediaViewer window into public/caps/NAME.png
# (or NAME-000.png… for a burst). Real screens of the app on the CC0 sample folder.
dir="$(cd "$(dirname "$0")/.." && pwd)/public/caps"
mkdir -p "$dir"
if [ -n "$3" ]; then
  i=0
  while [ $i -lt "$3" ]; do
    screencapture -l "$1" -o -x "$dir/$2-$(printf %03d $i).png"
    i=$((i + 1)); sleep "${4:-0.1}"
  done
else
  screencapture -l "$1" -o -x "$dir/$2.png"
fi
