#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Synology package of the NAS service (docs/plans/nas-setup.md §4).

    driftwire_spk.py build --binary PATH --arch x86_64|aarch64 --version X.Y.Z [--build N]
                           [--icon PNG] --out FILE.spk

An SPK is an uncompressed tar of INFO, package.tgz (bin/driftwire), scripts/, conf/,
WIZARD_UIFILES/ and two icons (DSM 7 Developer Guide, "Package structure"). The package
runs as its own user (conf/privilege); DSM opens and offers to forward its port
(conf/resource port-config); the install wizard names the share phones read, and DSM
grants the package user read on it (data-share). postinst makes the service's keys and its
serve.json; the service starts unclaimed and waits for a desktop on its own network.

Byte-for-byte reproducible: fixed member order, owners, modes and times. Nothing here
touches a NAS. What only a real unit can confirm (that DSM takes these exact fields) is
spike S5's real-unit check (docs/plans/nas-setup.md §1).
"""
from __future__ import annotations

import argparse
import gzip
import io
import json
from pathlib import Path
import re
import sys
import tarfile

PACKAGE = 'mediaviewer-nas'
PORT = 7878
OS_MIN = '7.2-64570'
# DSM's arch values per build: a generic x86-64 family, and the 64-bit ARM platforms
# (a DSM 7 SPK names platforms, not a generic "aarch64"). 32-bit ARM is not supported
# (docs/plans/nas-setup.md §2).
ARCHES = {
    'x86_64': 'x86_64',
    'aarch64': 'rtd1296 rtd1619b armada37xx',
}
MTIME = 1_700_000_000  # fixed: the same inputs give the same bytes
ICON = Path(__file__).resolve().parents[2] / 'assets' / 'icon' / 'mediaviewer-256.png'

START_STOP = r'''#!/bin/sh
# MediaViewer NAS service: started, stopped and asked about by DSM (scripts/start-stop-status).
# Runs as the package user. A verified update staged by the service itself (var/bin, see
# docs/plans/nas-setup.md §6) is preferred over the one the package installed (target/bin).
VAR="${SYNOPKG_PKGVAR:-/var/packages/mediaviewer-nas/var}"
TARGET="${SYNOPKG_PKGDEST:-/var/packages/mediaviewer-nas/target}"
PIDFILE="$VAR/driftwire.pid"
BIN="$TARGET/bin/driftwire"
if [ -x "$VAR/bin/driftwire" ]; then BIN="$VAR/bin/driftwire"; fi

running() {
  [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null
}

# A trial (cli/nas_update.h): the service clears it once five minutes up. A third
# start with it still there puts the previous binary back and skips that version.
trial() {
  [ -f "$VAR/state/trial" ] || return 0
  version=$(sed -n 1p "$VAR/state/trial")
  starts=$(sed -n 2p "$VAR/state/trial")
  starts=$(( ${starts:-0} + 1 ))
  if [ "$starts" -ge 3 ] && [ -x "$VAR/bin/driftwire.prev" ]; then
    mv -f "$VAR/bin/driftwire.prev" "$VAR/bin/driftwire"
    echo "$version" >"$VAR/state/skip"
    rm -f "$VAR/state/trial"
  elif [ "$starts" -ge 3 ]; then
    rm -f "$VAR/bin/driftwire"   # no previous: the package's own binary
    echo "$version" >"$VAR/state/skip"
    rm -f "$VAR/state/trial"
  else
    printf '%s\n%s\n' "$version" "$starts" >"$VAR/state/trial"
  fi
  BIN="$TARGET/bin/driftwire"
  if [ -x "$VAR/bin/driftwire" ]; then BIN="$VAR/bin/driftwire"; fi
}

# Keeps the service up: each start counts toward a trial; exit 75 (an update was
# installed, cli/nas_update.h kRestartForUpdate) starts the new one at once; any
# other exit is a crash, tried again after a pause. TERM stops it and the service.
supervise() {
  child=""
  trap '[ -n "$child" ] && kill "$child" 2>/dev/null; [ -n "$child" ] && wait "$child" 2>/dev/null; exit 0' TERM INT
  while :; do
    BIN="$TARGET/bin/driftwire"
    if [ -x "$VAR/bin/driftwire" ]; then BIN="$VAR/bin/driftwire"; fi
    trial
    MV_NAS_VAR="$VAR" "$BIN" serve --data "$VAR/serve" >>"$VAR/serve.log" 2>&1 &
    child=$!
    wait "$child"
    code=$?
    child=""
    [ "$code" -eq 75 ] && continue
    # A pause TERM cuts short (waited on, not run in the foreground). The variable is the tests'.
    sleep "${MV_NAS_RESTART_PAUSE:-30}" &
    child=$!
    wait "$child"
    child=""
  done
}

case "$1" in
  start)
    running && exit 0
    # Its own process, detached from whoever called (Package Center, a test): every
    # descriptor its own, so the caller's pipes close when this script returns
    # (dash, DSM's /bin/sh, keeps them on a backgrounded function).
    if command -v nohup >/dev/null 2>&1; then
      nohup "$0" supervise </dev/null >/dev/null 2>&1 &
    else
      "$0" supervise </dev/null >/dev/null 2>&1 &
    fi
    echo $! >"$PIDFILE"
    exit 0
    ;;
  supervise)
    supervise
    ;;
  stop)
    if running; then
      kill "$(cat "$PIDFILE")" 2>/dev/null
      i=0
      while running && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    fi
    rm -f "$PIDFILE"
    exit 0
    ;;
  status)
    running && exit 0
    exit 3
    ;;
  *)
    exit 1
    ;;
esac
'''

POSTINST = r'''#!/bin/sh
# Makes the service's identity and library access key (driftwire serve init, owner-only in
# var/serve), and its serve.json naming the share the wizard asked for. Runs as the package
# user. An upgrade keeps var/ and never comes here with keys already made.
VAR="${SYNOPKG_PKGVAR:-/var/packages/mediaviewer-nas/var}"
TARGET="${SYNOPKG_PKGDEST:-/var/packages/mediaviewer-nas/target}"
SHARE="${wizard_share:-photo}"
mkdir -p "$VAR/serve" && chmod 700 "$VAR/serve" || exit 1
if [ ! -f "$VAR/serve/serve.json" ]; then
  "$TARGET/bin/driftwire" serve --data "$VAR/serve" init >/dev/null || exit 1
  ID=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
  printf '{"port":@PORT@,"name":"MediaViewer NAS","roots":[{"id":"%s","label":"%s","path":"/var/packages/mediaviewer-nas/shares/%s"}]}\n' \
    "$ID" "$SHARE" "$SHARE" >"$VAR/serve/serve.json.part" &&
    chmod 600 "$VAR/serve/serve.json.part" &&
    mv "$VAR/serve/serve.json.part" "$VAR/serve/serve.json" || exit 1
fi
exit 0
'''.replace('@PORT@', str(PORT))

NOTHING = '#!/bin/sh\nexit 0\n'

WIZARD = [{
    'step_title': 'The share your phone reads',
    'items': [{
        'type': 'textfield',
        'desc': 'The shared folder that holds your photos and videos, as Control Panel → Shared Folder names it. '
                'MediaViewer reads it; it never changes or deletes anything there.',
        'subitems': [{
            'key': 'wizard_share',
            'desc': 'Shared folder',
            'defaultValue': 'photo',
            'validator': {'allowBlank': False, 'regex': {'expr': r'/^[^\/\\:*?"<>|]{1,64}$/',
                                                         'errorText': 'A shared folder\'s name, without slashes'}},
        }],
    }],
}]

PRIVILEGE = {'defaults': {'run-as': 'package'}}

RESOURCE = {
    'port-config': {'protocol-file': f'conf/{PACKAGE}.sc'},
    # Read only: the service moves bytes out and never writes the share (phone backup, 1.1,
    # will ask for write on its own folder).
    'data-share': {'shares': [{'name': '{{wizard_share}}', 'permission': {'ro': [PACKAGE]}}]},
}

PORT_FILE = f'''[{PACKAGE}]
title="MediaViewer NAS service"
desc="Phones reach your library here"
port_forward="yes"
dst.ports="{PORT}/tcp"
'''


def info_text(arch: str, version: str, build: int) -> str:
    fields = [
        ('package', PACKAGE),
        ('version', f'{version}-{build:04d}'),
        ('os_min_ver', OS_MIN),
        ('displayname', 'MediaViewer NAS service'),
        ('description', 'Lets the MediaViewer app on your phone find and open your photos and videos on this NAS, '
                        'from home or away. Set up and paired from MediaViewer on your Mac or PC.'),
        ('maintainer', 'MediaViewer'),
        ('arch', ARCHES[arch]),
        ('thirdparty', 'yes'),
        ('silent_upgrade', 'yes'),
        ('startable', 'yes'),
    ]
    return ''.join(f'{k}="{v}"\n' for k, v in fields)


def _add(tar: tarfile.TarFile, name: str, data: bytes, mode: int) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = MTIME
    info.uid = info.gid = 0
    info.uname = info.gname = 'root'
    tar.addfile(info, io.BytesIO(data))


def package_tgz(binary: bytes) -> bytes:
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as t:
        _add(t, 'bin/driftwire', binary, 0o755)
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode='wb', mtime=MTIME, compresslevel=9) as g:
        g.write(raw.getvalue())
    return out.getvalue()


def build_spk(binary: bytes, arch: str, version: str, build: int = 1, icon: bytes | None = None) -> bytes:
    if arch not in ARCHES:
        raise ValueError(f'arch must be one of {sorted(ARCHES)}')
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('version must be X.Y.Z')
    if not (1 <= build <= 9999):
        raise ValueError('build must be 1..9999')
    if len(binary) < 4:
        raise ValueError('the binary is empty')
    icon = icon if icon is not None else ICON.read_bytes()
    members = [
        ('INFO', info_text(arch, version, build).encode(), 0o644),
        ('package.tgz', package_tgz(binary), 0o644),
        ('scripts/start-stop-status', START_STOP.encode(), 0o755),
        ('scripts/postinst', POSTINST.encode(), 0o755),
        ('scripts/preinst', NOTHING.encode(), 0o755),
        ('scripts/preuninst', NOTHING.encode(), 0o755),
        ('scripts/postuninst', NOTHING.encode(), 0o755),
        ('scripts/preupgrade', NOTHING.encode(), 0o755),
        ('scripts/postupgrade', NOTHING.encode(), 0o755),
        ('conf/privilege', (json.dumps(PRIVILEGE, indent=2) + '\n').encode(), 0o644),
        ('conf/resource', (json.dumps(RESOURCE, indent=2) + '\n').encode(), 0o644),
        (f'conf/{PACKAGE}.sc', PORT_FILE.encode(), 0o644),
        ('WIZARD_UIFILES/install_uifile', (json.dumps(WIZARD, indent=2, ensure_ascii=False) + '\n').encode(), 0o644),
        ('PACKAGE_ICON.PNG', icon, 0o644),
        ('PACKAGE_ICON_256.PNG', icon, 0o644),
    ]
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w', format=tarfile.USTAR_FORMAT) as t:
        for name, data, mode in members:
            _add(t, name, data, mode)
    return out.getvalue()


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('--binary', required=True, type=Path)
    b.add_argument('--arch', required=True, choices=sorted(ARCHES))
    b.add_argument('--version', required=True)
    b.add_argument('--build', type=int, default=1)
    b.add_argument('--icon', type=Path)
    b.add_argument('--out', required=True, type=Path)
    a = p.parse_args(argv)
    spk = build_spk(a.binary.read_bytes(), a.arch, a.version, a.build,
                    a.icon.read_bytes() if a.icon else None)
    a.out.write_bytes(spk)
    print(f'{a.out.name}: {len(spk)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
