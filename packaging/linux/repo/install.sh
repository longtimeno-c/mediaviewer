#!/bin/sh
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The driftwire installer (docs/design/28 "Installing"):
#
#   curl -fsSL <site>/install.sh | sh                      install driftwire, or upgrade it
#   curl -fsSL <site>/install.sh | sh -s -- --uninstall    remove what this added
#
# On Debian, Ubuntu and their derivatives it adds the signed apt repository; on
# Fedora and other dnf or yum systems, and on openSUSE (zypper), the signed rpm
# repository. The system's package manager then installs driftwire and updates it
# with everything else. Anywhere else it unpacks the tarball into ~/.local
# (/usr/local as root) after checking its SHA-256; that copy does not update
# itself. It needs no MediaViewer. On macOS the channel is Homebrew: with brew
# there it prints the line for that and installs nothing; without it, it
# unpacks the macOS archive the same way, and says how to get Homebrew.
#
# tools/package/driftwire_repo.py writes the site's address, its key, the
# Homebrew tap and the release page into the lines below when it builds the
# site; DRIFTWIRE_REPO_URL overrides the address (tests, a fork's site).
# Everything is in functions and main runs on the last line, so a download cut
# off part way runs nothing.

set -eu

DEFAULT_URL='@DRIFTWIRE_REPO_URL@'
KEY_FINGERPRINTS='@DRIFTWIRE_KEY_FINGERPRINTS@'
BREW_TAP='@DRIFTWIRE_BREW_TAP@'
RELEASES_URL='@DRIFTWIRE_RELEASES_URL@'

APT_KEYRING=/etc/apt/keyrings/driftwire.gpg
APT_SOURCES=/etc/apt/sources.list.d/driftwire.sources
RPM_REPO_FILE=/etc/yum.repos.d/driftwire.repo
ZYPPER_REPO_FILE=/etc/zypp/repos.d/driftwire.repo
# What the tarball install put where, relative to its prefix, for --uninstall.
MANIFEST=share/driftwire/install-manifest

say() { printf 'driftwire: %s\n' "$*"; }
step() { printf '  %s\n' "$*"; }
die() {
  printf 'driftwire: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Installs driftwire, or upgrades it, from its signed package repository
(on macOS without Homebrew, from its signed checksums).

  curl -fsSL <site>/install.sh | sh
  curl -fsSL <site>/install.sh | sh -s -- --uninstall

  --uninstall   remove driftwire, and the repository source and key (or the files) this added
  --help        this text

DRIFTWIRE_REPO_URL=<https address> installs from another copy of the site.
EOF
}

# Prints a command, then runs it.
run() {
  printf '+ %s\n' "$*"
  "$@"
}

# Prints a command, then runs it as root (through sudo when this is not root).
run_root() {
  if [ -n "$SUDO" ]; then
    printf '+ sudo %s\n' "$*"
    sudo "$@"
  else
    printf '+ %s\n' "$*"
    "$@"
  fi
}

has() { command -v "$1" >/dev/null 2>&1; }

# ---- checks --------------------------------------------------------------------

check_system() {
  kernel=$(uname -s)
  case $kernel in
    Linux) OS=linux ;;
    Darwin) OS=macos ;;
    *) die "this installer is for Linux and macOS; this system is $kernel." ;;
  esac
}

# The Mac's channel is a Homebrew tap (docs/design/28 "Installing"): with brew
# here, Homebrew installs and updates it, so this says how and installs nothing.
# Without brew, main goes on to the archive (install_tarball).
macos_homebrew() {
  case $BREW_TAP in
    @*) die "this is the installer's template; on macOS, driftwire comes from Homebrew." ;;
  esac
  has brew || return 0
  say "on macOS, driftwire comes from Homebrew, which keeps it up to date (brew upgrade). Install it with:"
  printf '\n  brew install %s/driftwire\n\n' "$BREW_TAP"
  tarball_prefix
  if [ -f "$PREFIX/$MANIFEST" ]; then
    say "the copy this installer unpacked into $PREFIX is still there; remove it with this installer's --uninstall."
  fi
  exit 1
}

homebrew_hint() {
  say "Homebrew is not installed, so this installs the signed archive instead; it does not update itself."
  say "To have Homebrew keep driftwire up to date, install Homebrew (https://brew.sh):"
  # shellcheck disable=SC2016 # printed for the reader to run, not expanded here
  printf '\n  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"\n\n'
  say "then remove this copy (--uninstall) and run: brew install $BREW_TAP/driftwire"
}

check_macos() {
  machine=$(uname -m)
  # A shell under Rosetta says x86_64 on an Apple silicon Mac.
  if [ "$machine" = x86_64 ] && [ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]; then
    machine=arm64
  fi
  case $machine in
    arm64 | x86_64) ;;
    *) die "driftwire is built for Apple silicon and Intel Macs; this one is $machine." ;;
  esac
  macos=$(sw_vers -productVersion 2>/dev/null || true)
  macos_major=${macos%%.*}
  case $macos_major in
    '' | *[!0-9]*) die "could not tell the macOS version (sw_vers); driftwire needs macOS 14 or later." ;;
  esac
  if [ "$macos_major" -lt 14 ]; then
    die "driftwire needs macOS 14 or later; this Mac has $macos."
  fi
}

check_platform() {
  machine=$(uname -m)
  case $machine in
    x86_64 | amd64) DEB_ARCH=amd64 RPM_ARCH=x86_64 ;;
    aarch64 | arm64) DEB_ARCH=arm64 RPM_ARCH=aarch64 ;;
    *) die "driftwire is built for x86_64 and arm64 (aarch64); this machine is $machine." ;;
  esac
  # glibc answers getconf; musl does not, and its ldd says so.
  glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null || true)
  if [ -z "$glibc" ]; then
    if ldd --version 2>&1 | grep -qi musl; then
      die "this system's C library is musl (Alpine and others); driftwire needs glibc 2.35 or newer."
    fi
    die "could not tell this system's C library (getconf GNU_LIBC_VERSION); driftwire needs glibc 2.35 or newer."
  fi
  glibc=${glibc#glibc }
  glibc_major=${glibc%%.*}
  glibc_minor=${glibc#*.}
  glibc_minor=${glibc_minor%%.*}
  case $glibc_major.$glibc_minor in
    *[!0-9.]* | .* | *.) die "could not read the glibc version '$glibc'; driftwire needs glibc 2.35 or newer." ;;
  esac
  if [ "$glibc_major" -lt 2 ] || { [ "$glibc_major" -eq 2 ] && [ "$glibc_minor" -lt 35 ]; }; then
    die "driftwire needs glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 and later); this system has $glibc."
  fi
}

pick_url() {
  URL=${DRIFTWIRE_REPO_URL:-$DEFAULT_URL}
  URL=${URL%/}
  case $URL in
    @*) die "this is the installer's template, with no site address in it: set DRIFTWIRE_REPO_URL." ;;
    https://?*) ;;
    # A site served on this machine, for tests.
    http://127.0.0.1:* | http://127.0.0.1/* | http://localhost:* | http://localhost/*) ;;
    *) die "the repository's address must start with https:// ($URL)." ;;
  esac
  case $URL in
    *[!A-Za-z0-9._~:/%+-]*) die "the repository's address has a character it may not have ($URL)." ;;
  esac
}

pick_fetcher() {
  if has curl; then
    FETCH=curl
  elif has wget; then
    FETCH=wget
  else
    die "this needs curl or wget to download."
  fi
}

# fetch URL FILE
fetch() {
  printf '+ download %s\n' "$1"
  case $FETCH:$1 in
    curl:https://*) curl -fsSL --proto '=https' --tlsv1.2 --retry 3 -o "$2" "$1" ;;
    curl:*) curl -fsSL --retry 3 -o "$2" "$1" ;;
    wget:https://*) wget -q --https-only --tries=3 -O "$2" "$1" ;;
    wget:*) wget -q --tries=3 -O "$2" "$1" ;;
  esac || die "could not download $1"
}

# Root for the system steps: this user, or sudo.
need_root() {
  if [ "$(id -u)" -eq 0 ]; then
    SUDO=
  elif has sudo; then
    SUDO=sudo
    say "not running as root: the steps shown with sudo run through it, and it may ask for your password."
  else
    die "these steps need root, and sudo is not installed: run this as root."
  fi
}

describe_system() {
  if [ "$OS" = macos ]; then
    say "macOS $macos, $machine; from $URL"
    return 0
  fi
  pretty=
  if [ -r /etc/os-release ]; then
    pretty=$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | tr -d '"')
  fi
  say "${pretty:-Linux}, $(uname -m), glibc $glibc; from $URL"
}

suse_like() {
  [ -r /etc/os-release ] && grep -Eqi '^(ID|ID_LIKE)=.*suse' /etc/os-release
}

pick_manager() {
  if [ "$OS" = macos ]; then
    MANAGER=tarball
  elif has apt-get && has dpkg; then
    MANAGER=apt
  elif has zypper && suse_like; then
    MANAGER=zypper
  elif has dnf; then
    MANAGER=dnf
  elif has yum; then
    MANAGER=yum
  elif has zypper; then
    MANAGER=zypper
  else
    MANAGER=tarball
  fi
}

# A binary OpenPGP public key starts with a public-key packet (tag 6).
check_binary_key() {
  first=$(od -An -tx1 -N1 "$1" | tr -d ' \n')
  case $first in
    98 | 99 | 9a | c6) ;;
    *) die "$2 is not an OpenPGP public key." ;;
  esac
}

check_armored_key() {
  grep -q '^-----BEGIN PGP PUBLIC KEY BLOCK-----' "$1" || die "$2 is not an OpenPGP public key."
}

rpm_repo_text() {
  cat <<EOF
[driftwire]
name=Driftwire
baseurl=$URL/rpm/
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$URL/key.asc
type=rpm-md
EOF
}

# ---- installing ----------------------------------------------------------------

install_apt() {
  dpkg_arch=$(dpkg --print-architecture)
  [ "$dpkg_arch" = "$DEB_ARCH" ] ||
    die "this system's packages are $dpkg_arch; driftwire's .deb is built for amd64 and arm64."
  need_root
  say "Debian or Ubuntu (apt). About to:"
  step "1. save the repository's signing key as $APT_KEYRING"
  step "2. add the source $APT_SOURCES ($URL/apt stable main)"
  step "3. apt-get update, for that source only"
  step "4. apt-get install -y driftwire (installs it, or upgrades it)"
  fetch "$URL/key.gpg" "$TMP/driftwire.gpg"
  check_binary_key "$TMP/driftwire.gpg" "$URL/key.gpg"
  cat >"$TMP/driftwire.sources" <<EOF
Types: deb
URIs: $URL/apt
Suites: stable
Components: main
Architectures: $DEB_ARCH
Signed-By: $APT_KEYRING
EOF
  apt_new=0
  if [ ! -e "$APT_SOURCES" ]; then
    apt_new=1
  fi
  run_root install -d -m 0755 /etc/apt/keyrings
  run_root install -m 0644 "$TMP/driftwire.gpg" "$APT_KEYRING"
  run_root install -m 0644 "$TMP/driftwire.sources" "$APT_SOURCES"
  # --error-on=any: a source apt cannot read or verify fails here, not as a
  # warning that every later apt update repeats.
  if ! run_root apt-get update --error-on=any -o DPkg::Lock::Timeout=120 \
    -o "Dir::Etc::SourceList=$APT_SOURCES" -o Dir::Etc::SourceParts=/nonexistent \
    -o APT::Get::List-Cleanup=0; then
    if [ "$apt_new" = 1 ]; then
      say "apt could not use the repository: taking the source, the key and what apt fetched of it away again."
      run_root rm -f "$APT_SOURCES" "$APT_KEYRING"
      remove_apt_lists
    fi
    die "apt-get update failed for $URL/apt (above); driftwire is not installed."
  fi
  run_root apt-get install -y -o DPkg::Lock::Timeout=120 driftwire
  BIN=/usr/bin/driftwire
}

# install_dnf dnf|yum
install_dnf() {
  need_root
  say "Fedora or another rpm system ($1). About to:"
  step "1. import the repository's signing key into rpm (rpm --import)"
  step "2. add the repository $RPM_REPO_FILE ($URL/rpm/)"
  step "3. $1 install -y driftwire (or upgrade, when it is installed)"
  fetch "$URL/key.asc" "$TMP/key.asc"
  check_armored_key "$TMP/key.asc" "$URL/key.asc"
  rpm_repo_text >"$TMP/driftwire.repo"
  rpm_new=0
  if [ ! -e "$RPM_REPO_FILE" ] && [ -z "$(rpm_key_packages)" ]; then
    rpm_new=1
  fi
  run_root rpm --import "$TMP/key.asc"
  run_root install -d -m 0755 /etc/yum.repos.d
  run_root install -m 0644 "$TMP/driftwire.repo" "$RPM_REPO_FILE"
  if rpm -q driftwire >/dev/null 2>&1; then
    verb=upgrade
  else
    verb=install
  fi
  if ! run_root "$1" -y --refresh "$verb" driftwire; then
    if [ "$rpm_new" = 1 ] && ! rpm -q driftwire >/dev/null 2>&1; then
      say "$1 could not install from the repository: taking it and its key away again."
      run_root rm -f "$RPM_REPO_FILE"
      for key in $(rpm_key_packages); do
        run_root rpm -e "$key"
      done
    fi
    die "$1 failed (above)."
  fi
  BIN=/usr/bin/driftwire
}

install_zypper() {
  need_root
  say "openSUSE (zypper). About to:"
  step "1. import the repository's signing key into rpm (rpm --import)"
  step "2. add the repository 'driftwire' ($URL/rpm/), refreshed automatically"
  step "3. zypper install driftwire (or update, when it is installed)"
  fetch "$URL/key.asc" "$TMP/key.asc"
  check_armored_key "$TMP/key.asc" "$URL/key.asc"
  rpm_repo_text >"$TMP/driftwire.repo"
  zypper_repo=0
  if [ -e "$ZYPPER_REPO_FILE" ]; then
    zypper_repo=1
  fi
  rpm_new=0
  if [ "$zypper_repo" = 0 ] && [ -z "$(rpm_key_packages)" ]; then
    rpm_new=1
  fi
  run_root rpm --import "$TMP/key.asc"
  if [ "$zypper_repo" = 1 ]; then
    run_root zypper --non-interactive removerepo driftwire
  fi
  run_root zypper --non-interactive addrepo --refresh "$TMP/driftwire.repo"
  if rpm -q driftwire >/dev/null 2>&1; then
    verb=update
  else
    verb=install
  fi
  if ! run_root zypper --non-interactive refresh driftwire ||
    ! run_root zypper --non-interactive "$verb" driftwire; then
    if [ "$rpm_new" = 1 ] && ! rpm -q driftwire >/dev/null 2>&1; then
      say "zypper could not install from the repository: taking it and its key away again."
      run_root zypper --non-interactive removerepo driftwire
      for key in $(rpm_key_packages); do
        run_root rpm -e "$key"
      done
    fi
    die "zypper failed (above)."
  fi
  BIN=/usr/bin/driftwire
}

tarball_prefix() {
  if [ "$(id -u)" -eq 0 ]; then
    PREFIX=/usr/local
  else
    PREFIX=${HOME:?HOME is not set}/.local
  fi
}

# The SHA256SUMS line of this system's tarball: Linux's for this architecture;
# on a Mac the universal archive, or the arm64 one (a release whose Intel leg
# did not build) on Apple silicon.
pick_tarball() {
  sums='^[0-9a-f]{64}  driftwire-[0-9]+\.[0-9]+\.[0-9]+'
  if [ "$OS" = linux ]; then
    line=$(grep -E "$sums-linux-$RPM_ARCH\.tar\.gz\$" "$TMP/SHA256SUMS" || true)
    what="$RPM_ARCH tarball"
  else
    line=$(grep -E "$sums-macos-universal\.tar\.gz\$" "$TMP/SHA256SUMS" || true)
    if [ -z "$line" ] && [ "$machine" = arm64 ]; then
      line=$(grep -E "$sums-macos-arm64\.tar\.gz\$" "$TMP/SHA256SUMS" || true)
    fi
    what="macOS archive for this Mac ($machine)"
  fi
  case $line in
    '' | *"
"*)
      if [ "$OS" = macos ]; then
        die "SHA256SUMS does not name one $what. Install Homebrew (https://brew.sh) and run: brew install $BREW_TAP/driftwire; or see $RELEASES_URL"
      fi
      die "SHA256SUMS does not name one $what."
      ;;
  esac
}

# check_sha256 LINE: LINE of SHA256SUMS against its file in $TMP. macOS has
# shasum, not sha256sum.
check_sha256() {
  if has sha256sum; then
    (cd "$TMP" && printf '%s\n' "$1" | sha256sum -c --status -)
  else
    (cd "$TMP" && printf '%s\n' "$1" | shasum -a 256 -c -s)
  fi
}

# The macOS archive is one flat folder, as Homebrew's formula takes it
# (tools/mac/macpack.py stage_cli); laid out here as the Linux tarball is.
stage_macos() {
  [ -f "$1/driftwire" ] || die "$name has no driftwire."
  mkdir -p "$2/bin" "$2/share/man/man1" "$2/share/doc/driftwire"
  for file in "$1"/* "$1"/.[!.]*; do
    [ -e "$file" ] || continue
    [ -f "$file" ] || die "$name holds something other than files in $top/."
    case ${file##*/} in
      driftwire) cp -p "$file" "$2/bin/driftwire" ;;
      driftwire.1) cp -p "$file" "$2/share/man/man1/driftwire.1" ;;
      *) cp -p "$file" "$2/share/doc/driftwire/" ;;
    esac
  done
}

install_tarball() {
  tarball_prefix
  if [ "$OS" = macos ]; then
    homebrew_hint
    say "About to:"
    step "1. download the macOS archive and check its SHA-256 against the signed SHA256SUMS"
    step "2. unpack it into $PREFIX (bin/driftwire, its manual page and notices)"
  else
    say "no apt, dnf, yum or zypper here. About to:"
    step "1. download the $RPM_ARCH tarball and check its SHA-256 against the signed SHA256SUMS"
    step "2. unpack it into $PREFIX (bin/driftwire, its manual page and completions)"
  fi
  step "This copy does not update itself: run this installer again for a newer version."
  fetch "$URL/SHA256SUMS" "$TMP/SHA256SUMS"
  pick_tarball
  name=${line#*  }
  # Checked against the site's own key when gpgv is here; the SHA-256 is checked either way.
  if has gpgv; then
    fetch "$URL/SHA256SUMS.asc" "$TMP/SHA256SUMS.asc"
    fetch "$URL/key.gpg" "$TMP/key.gpg"
    check_binary_key "$TMP/key.gpg" "$URL/key.gpg"
    run gpgv --keyring "$TMP/key.gpg" "$TMP/SHA256SUMS.asc" "$TMP/SHA256SUMS" ||
      die "SHA256SUMS is not signed by the repository's key."
  fi
  fetch "$URL/$name" "$TMP/$name"
  check_sha256 "$line" || die "$name does not match its SHA-256 in SHA256SUMS."
  top=${name%.tar.gz}
  tar -tzf "$TMP/$name" >"$TMP/entries"
  while IFS= read -r entry; do
    case $entry in
      "$top" | "$top/" | "$top/"*) ;;
      *) die "$name holds a path outside $top/." ;;
    esac
    case /$entry/ in
      */../*) die "$name holds a path with '..'." ;;
    esac
  done <"$TMP/entries"
  mkdir "$TMP/unpacked"
  tar -xzf "$TMP/$name" -C "$TMP/unpacked" --no-same-owner
  src=$TMP/unpacked/$top
  if [ "$OS" = macos ]; then
    stage_macos "$src" "$TMP/staged"
    src=$TMP/staged
  fi
  [ -f "$src/bin/driftwire" ] || die "$name has no bin/driftwire."

  # An upgrade takes the previous copy's files away first.
  if [ -f "$PREFIX/$MANIFEST" ]; then
    remove_tarball_files quietly
  fi
  printf '+ unpack %s into %s\n' "$name" "$PREFIX"
  : >"$TMP/manifest"
  if [ ! -d "$PREFIX" ]; then
    mkdir -p "$PREFIX"
    printf 'd .\n' >>"$TMP/manifest"
  fi
  (cd "$src" && find . -type d | sed 's|^\./||' | grep -v '^\.$' | sort) >"$TMP/dirs"
  # The manifest's own folder, its parent first: a tarball without share/ would
  # otherwise leave the share/ that mkdir -p made.
  printf '%s\n' share share/driftwire >>"$TMP/dirs"
  while IFS= read -r dir; do
    if [ ! -d "$PREFIX/$dir" ]; then
      mkdir -p "$PREFIX/$dir"
      printf 'd %s\n' "$dir" >>"$TMP/manifest"
    fi
  done <"$TMP/dirs"
  (cd "$src" && find . -type f | sed 's|^\./||' | sort) >"$TMP/files"
  while IFS= read -r file; do
    cp -p "$src/$file" "$PREFIX/$file"
    printf 'f %s\n' "$file" >>"$TMP/manifest"
  done <"$TMP/files"
  cp "$TMP/manifest" "$PREFIX/$MANIFEST"
  BIN=$PREFIX/bin/driftwire
}

# ---- removing ------------------------------------------------------------------

# apt names a source's list files after its address, '/' turned to '_'. Other
# characters it escapes, and then the files are left for apt's next update.
apt_list_prefix() {
  rest=${URL#*://}
  case $rest in
    *[!A-Za-z0-9.:/-]*) return 1 ;;
  esac
  printf '%s' "$rest/apt/dists/stable" | tr / _
}

# apt's files of this source and no other: the lists it took, and in partial/
# what it fetched and refused (an InRelease whose signature is bad stays there)
# or did not finish.
remove_apt_lists() {
  lists=$(apt_list_prefix) || return 0
  for dir in /var/lib/apt/lists /var/lib/apt/lists/partial; do
    if [ -d "$dir" ]; then
      run_root find "$dir" -maxdepth 1 -type f -name "${lists}_*" -delete
    fi
  done
}

uninstall_apt() {
  # shellcheck disable=SC2016 # dpkg-query's own ${Status}, not the shell's
  status=$(dpkg-query -W -f='${Status}' driftwire 2>/dev/null || true)
  if [ "$status" != "install ok installed" ] && [ ! -e "$APT_SOURCES" ] && [ ! -e "$APT_KEYRING" ]; then
    return 0
  fi
  need_root
  say "Debian or Ubuntu (apt). About to:"
  if [ "$status" = "install ok installed" ]; then
    step "apt-get remove -y driftwire"
  fi
  step "delete $APT_SOURCES, $APT_KEYRING and apt's lists of that source (partial/ too)"
  if [ "$status" = "install ok installed" ]; then
    run_root apt-get remove -y -o DPkg::Lock::Timeout=120 driftwire
  fi
  run_root rm -f "$APT_SOURCES" "$APT_KEYRING"
  remove_apt_lists
  REMOVED=1
}

# The rpm names of this repository's keys: gpg-pubkey-<last 8 hex digits of the
# fingerprint>, or the whole fingerprint in newer rpm.
rpm_key_packages() {
  for fpr in $KEY_FINGERPRINTS; do
    case $fpr in
      @*) continue ;;
    esac
    lower=$(printf '%s' "$fpr" | tr 'A-F' 'a-f')
    short=$(printf '%s' "$lower" | tail -c 8)
    for name in "gpg-pubkey-$short" "gpg-pubkey-$lower"; do
      if rpm -q "$name" >/dev/null 2>&1; then
        printf '%s\n' "$name"
      fi
    done
  done
}

# Only where rpm is the system's package database: on Debian rpm is a tool, and
# querying it as a user would make a database in ~/.rpmdb.
uninstall_rpm() {
  case $MANAGER in
    dnf | yum | zypper) ;;
    *) return 0 ;;
  esac
  installed=0
  if rpm -q driftwire >/dev/null 2>&1; then
    installed=1
  fi
  zypper_repo=0
  if [ -e "$ZYPPER_REPO_FILE" ]; then
    zypper_repo=1
  fi
  keys=$(rpm_key_packages)
  if [ "$installed" = 0 ] && [ "$zypper_repo" = 0 ] && [ ! -e "$RPM_REPO_FILE" ] && [ -z "$keys" ]; then
    return 0
  fi
  need_root
  say "rpm ($MANAGER). About to:"
  if [ "$installed" = 1 ]; then
    step "remove driftwire"
  fi
  step "remove the repository and its key from rpm"
  if [ "$installed" = 1 ]; then
    case $MANAGER in
      zypper) run_root zypper --non-interactive remove driftwire ;;
      *) run_root "$MANAGER" -y remove driftwire ;;
    esac
  fi
  if [ "$zypper_repo" = 1 ]; then
    run_root zypper --non-interactive removerepo driftwire
  fi
  if [ -e "$RPM_REPO_FILE" ]; then
    run_root rm -f "$RPM_REPO_FILE"
  fi
  for key in $keys; do
    run_root rpm -e "$key"
  done
  REMOVED=1
}

# remove_tarball_files [quietly]: the files the manifest lists, then the folders
# this made (the prefix itself as "."), deepest first, when they are empty.
remove_tarball_files() {
  if [ "${1:-}" != quietly ]; then
    printf '+ remove the files listed in %s\n' "$PREFIX/$MANIFEST"
  fi
  sed -n 's/^f //p' "$PREFIX/$MANIFEST" | while IFS= read -r file; do
    case /$file/ in
      */../*) continue ;;
    esac
    rm -f "$PREFIX/$file"
  done
  sed -n 's/^d //p' "$PREFIX/$MANIFEST" >"$TMP/made"
  rm -f "$PREFIX/$MANIFEST"
  sort -r "$TMP/made" | while IFS= read -r dir; do
    case /$dir/ in
      */../*) continue ;;
      /./) rmdir "$PREFIX" 2>/dev/null || true ;;
      *) rmdir "$PREFIX/$dir" 2>/dev/null || true ;;
    esac
  done
}

uninstall_tarball() {
  tarball_prefix
  [ -f "$PREFIX/$MANIFEST" ] || return 0
  say "the tarball in $PREFIX. About to remove the files it put there."
  remove_tarball_files
  REMOVED=1
}

# Homebrew's copy is Homebrew's to remove; this says how.
uninstall_homebrew_note() {
  if [ "$OS" != macos ] || ! has brew || ! brew list --formula driftwire >/dev/null 2>&1; then
    return 0
  fi
  say "Homebrew's driftwire is installed, and this script did not install it. Remove it with:"
  printf '\n  brew uninstall driftwire\n\n'
  [ "$REMOVED" = 1 ] || exit 1
}

# ---- main ----------------------------------------------------------------------

finish() {
  version=$("$BIN" --version) || die "$BIN did not run."
  printf '%s\n' "$version"
  start=driftwire
  case :${PATH:-}: in
    *:"$(dirname "$BIN")":*) ;;
    *)
      start=$BIN
      profile=.profile
      if [ "$OS" = macos ]; then
        profile=.zprofile
      fi
      say "$(dirname "$BIN") is not on your PATH; add it (in ~/$profile: PATH=\"$(dirname "$BIN"):\$PATH\") to type just driftwire."
      ;;
  esac
  say "installed. Start it with: $start"
}

main() {
  action=install
  for arg in "$@"; do
    case $arg in
      --uninstall) action=uninstall ;;
      -h | --help)
        usage
        exit 0
        ;;
      *)
        usage >&2
        die "unknown option: $arg"
        ;;
    esac
  done

  check_system
  if [ "$OS" = macos ] && [ "$action" = install ]; then
    macos_homebrew
  fi
  pick_url
  TMP=$(mktemp -d)
  trap 'rm -rf "$TMP"' EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  SUDO=
  pick_manager

  if [ "$action" = uninstall ]; then
    REMOVED=0
    if [ "$MANAGER" = apt ]; then
      uninstall_apt
    fi
    uninstall_rpm
    uninstall_tarball
    uninstall_homebrew_note
    data=.local/state/driftwire
    if [ "$OS" = macos ]; then
      data='Library/Application Support/Driftwire'
    fi
    if [ "$REMOVED" = 0 ]; then
      say "nothing of driftwire's to remove here."
    else
      say "removed. Its data folder (~/$data of each user who ran it) is left as it was."
    fi
    return 0
  fi

  if [ "$OS" = macos ]; then
    check_macos
  else
    check_platform
  fi
  pick_fetcher
  describe_system
  case $MANAGER in
    apt) install_apt ;;
    dnf | yum) install_dnf "$MANAGER" ;;
    zypper) install_zypper ;;
    tarball) install_tarball ;;
  esac
  finish
}

main "$@"
