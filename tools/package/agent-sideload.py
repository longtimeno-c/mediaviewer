#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sideload the AI agents add-on (docs/design/29) into an add-ons folder, signed
with a development key, for a developer build's own run of `MediaViewer --mcp`.

    agent-sideload.py --module build/addons/agent/libmv_agent.dylib --key <dev key hex file> \\
        --addons <add-ons folder> [--platform macos|win-x64] [--version 1.0.0]

It writes <addons>/<folder>/<version>/ (Mac: the add-on's name, "AI agents";
Windows: its id, "agent") with the module, the skills beside it when the build
has them, manifest.json from src/addons/agent/manifest.json and its detached
Ed25519 signature: what the store verifies at load. Only a build configured with
-DMV_ADDON_DEV_PUBLIC_KEY=<the key's public half> trusts it, and only when
started with MV_DEV_ADDONS_DIR=<addons>: never put it in a real profile.

Needs `cryptography` (the system /usr/bin/python3 on the Mac has it).
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def entry(name: str, data: bytes) -> dict:
    return {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            "licence": "GPL-3.0-or-later"}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--module", type=Path, required=True)
    ap.add_argument("--key", type=Path, required=True)
    ap.add_argument("--addons", type=Path, required=True)
    ap.add_argument("--platform", default="macos", choices=["macos", "win-x64"])
    ap.add_argument("--version", default="1.0.0")
    a = ap.parse_args()

    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    raw = bytes.fromhex(a.key.read_text().strip())
    key = Ed25519PrivateKey.from_private_bytes(raw[:32])

    source = json.loads((ROOT / "src/addons/agent/manifest.json").read_text(encoding="utf-8"))
    folder = a.addons / ("AI agents" if a.platform == "macos" else "agent")
    if folder.exists():
        shutil.rmtree(folder)
    version = folder / a.version
    version.mkdir(parents=True)
    data = a.module.read_bytes()
    (version / a.module.name).write_bytes(data)
    files = [entry(a.module.name, data)]
    skills = a.module.parent / "skills"
    if skills.is_dir():
        for p in sorted(skills.rglob("*")):
            if p.is_file():
                rel = p.relative_to(a.module.parent).as_posix()
                (version / rel).parent.mkdir(parents=True, exist_ok=True)
                b = p.read_bytes()
                (version / rel).write_bytes(b)
                files.append(entry(rel, b))
    manifest = {
        "schema": 1,
        "id": "agent",
        "name": "AI agents",
        "version": a.version,
        "platform": a.platform,
        "host_api": {"min": 3, "max": 3},
        "installed_size": sum(f["size"] for f in files),
        "native": a.module.name,
        "chrome": "",
        "description": source["description"],
        "archive": {"path": "agent.zip", "sha256": "a" * 64, "size": 1},
        "files": files,
        "contributes": source["contributes"],
    }
    text = json.dumps(manifest, indent=1).encode()
    (version / "manifest.json").write_bytes(text)
    (version / "manifest.json.sig").write_bytes(key.sign(text))
    print("sideloaded", version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
