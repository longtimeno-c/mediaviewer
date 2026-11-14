#!/bin/sh
# M1 spike S5 (throwaway). Run on the NAS, over ssh, from the folder holding the copied programs:
#   sh probe-synology.sh
# Reads only, changes nothing, sends nothing. Paste the output into the report.

say() { printf '%s: %s\n' "$1" "$2"; }

conf() { sed -n "s/^$2=\"\{0,1\}\([^\"]*\)\"\{0,1\}$/\1/p" "$1" 2>/dev/null | head -n 1; }

say model "$(conf /etc/synoinfo.conf upnpmodelname)"
say unique "$(conf /etc/synoinfo.conf unique)"
say dsm "$(conf /etc.defaults/VERSION productversion) build $(conf /etc.defaults/VERSION buildnumber) update $(conf /etc.defaults/VERSION smallfixnumber)"
say kernel "$(uname -r)"
say machine "$(uname -m)"
say cpu "$(sed -n 's/^model name[[:space:]]*: //p; s/^Hardware[[:space:]]*: //p' /proc/cpuinfo | head -n 1)"
say cores "$(grep -c '^processor' /proc/cpuinfo)"
say memory_kb "$(sed -n 's/^MemTotal:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo)"
say page_size "$(getconf PAGESIZE 2>/dev/null)"
say glibc "$(getconf GNU_LIBC_VERSION 2>/dev/null || echo unknown)"

libstdcxx=$(ls /usr/lib/libstdc++.so.6* /lib/libstdc++.so.6* 2>/dev/null | head -n 1)
say libstdcxx "${libstdcxx:-none found}"
# Over ssh DSM's PATH can leave out /usr/local/bin, where packages put these.
find_program() {
  command -v "$1" 2>/dev/null && return
  [ -x "/usr/local/bin/$1" ] && echo "/usr/local/bin/$1" && return
  echo none
}
say python3 "$(find_program python3)"
say docker "$(find_program docker)"
say container_manager "$(ls -d /var/packages/ContainerManager /var/packages/Docker 2>/dev/null | head -n 1)"
say systemd "$(command -v systemctl || echo none)"

for program in ./driftwire ./mv-driftwire-relay; do
  if [ -x "$program" ]; then
    if [ "$program" = ./driftwire ]; then
      output=$("$program" --version 2>&1); code=$?
    else
      # The relay has no version. --help is a usage error, so exit 2 with its usage line means it
      # loaded and ran; anything else (127, 126, a loader message) means it did not.
      output=$("$program" --help 2>&1); code=$?
      output=$(printf '%s\n' "$output" | head -n 1)
    fi
    say "$program" "exit $code: $output"
  else
    say "$program" "not here"
  fi
done
