#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble (and sign) "MediaViewer for Final Cut Pro.app", the fcp add-on's
payload (plan/23):

    Contents/Info.plist
    Contents/MacOS/MediaViewer for Final Cut Pro        the container
    Contents/MacOS/MediaViewerSearchAgent               the Local search agent
    Contents/Library/LaunchAgents/<service>.plist       SMAppService.agent's job
    Contents/PlugIns/MediaViewerSearch.appex            the workflow extension

Signed inside out: the extension (sandboxed, its app group), the agent
(hardened runtime; a dev-key build adds Agent-dev.entitlements so a locally
built AI pack loads), then the container. With no --identity everything is
signed ad hoc, which is enough to build and for the in-process tests but not
for FCP to load the extension or the agent to accept a peer.
"""
import argparse
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


def sign(path: Path, identity: str, entitlements: Optional[Path] = None, runtime: bool = True) -> None:
    cmd = ["codesign", "--force", "--timestamp=none" if identity == "-" else "--timestamp", "--sign", identity]
    if runtime:
        cmd += ["--options", "runtime"]
    if entitlements:
        cmd += ["--entitlements", str(entitlements)]
    subprocess.run(cmd + [str(path)], check=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", required=True, type=Path)
    ap.add_argument("--container", required=True, type=Path)
    ap.add_argument("--agent", required=True, type=Path)
    ap.add_argument("--appex", required=True, type=Path)
    ap.add_argument("--container-plist", required=True, type=Path)
    ap.add_argument("--appex-plist", required=True, type=Path)
    ap.add_argument("--agent-plist", required=True, type=Path)
    ap.add_argument("--appex-entitlements", required=True, type=Path)
    ap.add_argument("--agent-entitlements", type=Path)
    ap.add_argument("--identity", default="-")
    a = ap.parse_args(argv)

    app = a.app
    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"
    macos = contents / "MacOS"
    macos.mkdir(parents=True)
    exe_name = plistlib.loads(a.container_plist.read_bytes())["CFBundleExecutable"]
    shutil.copy2(a.container_plist, contents / "Info.plist")
    shutil.copy2(a.container, macos / exe_name)
    shutil.copy2(a.agent, macos / "MediaViewerSearchAgent")
    agents = contents / "Library" / "LaunchAgents"
    agents.mkdir(parents=True)
    shutil.copy2(a.agent_plist, agents / a.agent_plist.name)
    appex = contents / "PlugIns" / "MediaViewerSearch.appex" / "Contents"
    (appex / "MacOS").mkdir(parents=True)
    shutil.copy2(a.appex_plist, appex / "Info.plist")
    shutil.copy2(a.appex, appex / "MacOS" / "MediaViewerSearch")

    sign(contents / "PlugIns" / "MediaViewerSearch.appex", a.identity, a.appex_entitlements)
    sign(macos / "MediaViewerSearchAgent", a.identity, a.agent_entitlements)
    sign(app, a.identity)
    return 0


if __name__ == "__main__":
    sys.exit(main())
