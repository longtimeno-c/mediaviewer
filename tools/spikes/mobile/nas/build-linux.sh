#!/usr/bin/env bash
# M1 spikes S2 and S5 (throwaway). Builds `driftwire` and `mv-driftwire-relay` for a Synology, in a
# container, the way .github/actions/driftwire-cli-linux does (cmake/driftwire-cli), and says what
# each program needs from the NAS.
#
#   tools/spikes/mobile/nas/build-linux.sh x64          # Intel and AMD models
#   tools/spikes/mobile/nas/build-linux.sh arm64        # aarch64 models
#   BASE=ubuntu:20.04 tools/spikes/mobile/nas/build-linux.sh x64
#
# Needs Docker (or Colima). On an Apple silicon Mac x64 runs emulated: expect a long first build.
# Writes tools/spikes/mobile/nas/dist/<arch>/. Downloads the base image, apt packages and vcpkg
# ports; contacts nothing else, and runs neither program.
set -euo pipefail

arch="${1:-}"
case "$arch" in
  x64) platform=linux/amd64 ;;
  arm64) platform=linux/arm64 ;;
  *) echo "usage: $0 x64|arm64" >&2; exit 2 ;;
esac

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
out="$here/dist/$arch"
base="${BASE:-ubuntu:22.04}"
baseline="$(sed -n 's/.*"builtin-baseline": *"\([0-9a-f]*\)".*/\1/p' "$root/tools/portable/vcpkg.json")"
[ -n "$baseline" ] || { echo "no builtin-baseline in tools/portable/vcpkg.json" >&2; exit 1; }
command -v docker >/dev/null || { echo "docker is not installed (README, S5, has the CI route)" >&2; exit 1; }

mkdir -p "$out"
docker run --rm --platform "$platform" \
  -v "$root":/src:ro -v "$out":/out \
  -e VCPKG_COMMIT="$baseline" -e ARCH="$arch" \
  "$base" bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends software-properties-common ca-certificates git curl zip unzip tar \
      build-essential autoconf autoconf-archive automake libtool pkg-config ninja-build python3-pip binutils
    if ! apt-cache show g++-12 >/dev/null 2>&1; then add-apt-repository -y ppa:ubuntu-toolchain-r/test; apt-get update -qq; fi
    apt-get install -y -qq --no-install-recommends g++-12
    python3 -m pip install --quiet "cmake>=3.28,<4"
    export PATH="$HOME/.local/bin:$PATH" CC=gcc-12 CXX=g++-12
    [ "$ARCH" = arm64 ] && export VCPKG_FORCE_SYSTEM_BINARIES=1

    # /src is read-only: work on a copy, without anything of the host'\''s builds.
    mkdir -p /work/src
    tar -C /src --exclude=./build --exclude=./build-cli --exclude=./.git --exclude=./tools/spikes/mobile/nas/dist -cf - . | tar -C /work/src -xf -

    git clone -q https://github.com/microsoft/vcpkg /work/vcpkg
    git -C /work/vcpkg checkout -q "$VCPKG_COMMIT"
    /work/vcpkg/bootstrap-vcpkg.sh -disableMetrics

    # The relay is not one of the packages'\'' programs, so the link flags the package sets on the
    # command line only are given here for both.
    cd /work/src
    cmake -S cmake/driftwire-cli -B /work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/work/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_MANIFEST_DIR=/work/src/tools/portable \
      -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
    cmake --build /work/build --target mv_driftwire_cli mv_driftwire_relay
    cmake --build /work/build --target driftwire_needed

    for program in driftwire mv-driftwire-relay; do
      file="$(find /work/build -type f -name "$program" -perm -u+x | head -n 1)"
      strip "$file"
      cp "$file" /out/
      echo "== $program: $(wc -c < "$file") bytes"
      readelf -d "$file" | sed -n "s/.*NEEDED.*\[\(.*\)\]/   needs \1/p"
      echo "   newest glibc symbol: $(objdump -T "$file" | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -n 1)"
      echo "   newest libstdc++ symbol: $(objdump -T "$file" | grep -o "GLIBCXX_[0-9.]*" | sort -Vu | tail -n 1)"
    done
    getconf GNU_LIBC_VERSION
  '
cp "$here/probe-synology.sh" "$here/range_server.py" "$out/"
echo "Copy $out to the NAS (README, S5)."
