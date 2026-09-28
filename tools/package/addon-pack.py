#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack and sign an add-on (plan/18 "Add-ons: how Import is installed").

    addon-pack.py pack [--addon import|ai|ai-faces|ai-cuda] --platform win-x64|macos \
        --src build/addons/<addon> --version 1.0.0 --key <ed25519-private-key-hex-file> --out dist/
    addon-pack.py ceiling dist/mediaviewer-addon-ai*-<platform>.json

writes, for the release to publish beside the app's own assets:

    mediaviewer-addon-<addon>-<platform>.zip        the files, and nothing else
    mediaviewer-addon-<addon>-<platform>.json       the manifest (schema 1)
    mediaviewer-addon-<addon>-<platform>.json.sig   64-byte raw Ed25519, detached

Milestone H (plan/17 "The AI pack") adds the AI pack's pieces: `ai` (the
Core: mv_ai, ONNX Runtime, the CLIP towers, the chrome), and the model-only
or runtime-only pieces `ai-faces` and `ai-cuda` (`part_of: ai`, no native
entry, no chrome). Model files carry the licence tools/package/ai-models.py
recorded in the staged `licences.json`; a licence outside the allowed set
fails the pack (the weights-licence gate). `ceiling` refuses a family whose
supported installed combination exceeds 3 GB (plan/17).

The manifest lists every file with its SHA-256, size and licence, the host API
range the add-on supports, and the archive's own name, size and SHA-256. It
is signed with the update-manifest key (tools/package/update-signing.md), and
the app refuses anything else: a changed byte, a missing file, an extra file,
an unsigned or wrongly signed manifest, the wrong platform, or a host API
outside the range. The private key never touches the repository or a log.

On macOS the add-on is universal, like the app (D9 amended 2026-09-24): build
it on both architectures and join the two `build*/addons/import` trees with
tools/mac/lipo_merge.py before packing. The bundle and dylib must then be Developer ID-signed (same Team
ID as the app, for library validation) before packing; the archive is made
with `ditto -c -k` so the code signature survives, and notarization of the
archive follows the app's own procedure (RELEASING.md).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ALLOWED_LICENCES = {"GPL-3.0-or-later", "MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause",
                    "Zlib", "ISC", "CC0-1.0"}
CEILINGS = {"ai": 3_000_000_000}  # plan/17; src/addon/manifest.cpp family_ceiling agrees

# What ships, per add-on and platform. Anything else in the build folder stays
# out. `licences`: (prefix, SPDX) for files the staged licences.json does not
# name; the add-on's own code is GPL-3.0-or-later.
ADDONS = {
    "import": {
        "name": "Import",
        "host_api": {"min": 1, "max": 1},  # MV_ADDON_HOST_API range this build supports
        "files": {
            "win-x64": {
                "native": "mv_import.dll",
                "chrome": "MediaViewer.Import.Chrome.dll",
                "include": ["mv_import.dll", "MediaViewer.Import.Chrome.dll",
                            "MediaViewer.Import.Chrome.deps.json"],
            },
            "macos": {
                "native": "libmv_import.dylib",
                "chrome": "Import.bundle",
                "include": ["libmv_import.dylib", "Import.bundle"],
            },
        },
        "licences": [],
        "notice": ("Import add-on for MediaViewer. GPL-3.0-or-later. Uses SQLite (public domain).\n"
                   "Content hashes use BLAKE3 (CC0-1.0) and signatures libsodium (ISC), both in the app.\n"),
    },
    "ai": {
        "name": "AI",
        "host_api": {"min": 2, "max": 2},  # needs host table 2's pixels
        "files": {
            "win-x64": {
                "native": "mv_ai.dll",
                "chrome": "MediaViewer.Ai.Chrome.dll",
                "include": ["mv_ai.dll", "onnxruntime.dll", "onnxruntime_providers_shared.dll",
                            "MediaViewer.Ai.Chrome.dll", "MediaViewer.Ai.Chrome.deps.json", "models"],
            },
            "macos": {
                "native": "libmv_ai.dylib",
                "chrome": "AI.bundle",
                "arch": "arm64",  # ONNX Runtime 1.30 ships no x86_64 macOS build
                "include": ["libmv_ai.dylib", "libonnxruntime.dylib", "AI.bundle", "models"],
            },
        },
        "licences": [("onnxruntime", "MIT"), ("libonnxruntime", "MIT")],
        "notice": ("Local search (the AI pack) for MediaViewer. GPL-3.0-or-later.\n"
                   "ONNX Runtime (MIT, Microsoft). OpenAI CLIP ViT-B/32 and ViT-L/14 weights (MIT),\n"
                   "ONNX exports by Xenova (MIT). Uses SQLite (public domain) in the app.\n"
                   "Runs entirely on this computer; nothing is sent anywhere.\n"),
        "ship_notices": ["ThirdPartyNotices.txt"],
    },
    "ai-audio": {
        "name": "AI Audio",
        "part_of": "ai",
        "host_api": {"min": 2, "max": 2},
        "files": {
            "win-x64": {"include": ["models"]},
            "macos": {"arch": "arm64", "include": ["models"]},
        },
        "licences": [],
        "notice": ("Audio search for MediaViewer's Local search: what clips sound like and what is said\n"
                   "in them. Models: LAION CLAP (Apache-2.0), OpenAI Whisper base and small\n"
                   "(Apache-2.0; ONNX exports by Xenova / onnx-community). Runs on this computer only.\n"),
    },
    "ai-faces": {
        "name": "AI Faces",
        "part_of": "ai",
        "host_api": {"min": 2, "max": 2},
        "files": {
            "win-x64": {"include": ["models"]},
            "macos": {"arch": "arm64", "include": ["models"]},
        },
        "licences": [],
        "notice": ("People (face search) for MediaViewer's Local search. Models: YuNet (MIT,\n"
                   "Shiqi Yu), SFace (Apache-2.0, OpenCV Zoo). Face data never leaves this computer.\n"),
    },
    "ai-cuda": {
        "name": "AI CUDA",
        "part_of": "ai",
        "host_api": {"min": 2, "max": 2},
        "files": {
            "win-x64": {"include": ["onnxruntime.dll", "onnxruntime_providers_shared.dll",
                                    "onnxruntime_providers_cuda.dll"]},
        },
        "licences": [("onnxruntime", "MIT")],
        "notice": ("NVIDIA acceleration for MediaViewer's Local search: ONNX Runtime's CUDA build\n"
                   "(MIT, Microsoft). The CUDA runtime and cuDNN are NOT included: they are\n"
                   "NVIDIA's, installed by the user; without them Local search runs on the CPU.\n"),
    },
}

LICENCE = "GPL-3.0-or-later"
# Import's, kept for callers of the Milestone G interface.
ADDON_ID = "import"
FILES = ADDONS["import"]["files"]
NOTICE = ADDONS["import"]["notice"]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


ROOT = Path(__file__).resolve().parents[2]


def pinned_public_key_hex() -> str:
    """The key every app build trusts (UpdateKeys.ProductionPublicKeyHex; the
    native copy in src/addon/manifest.cpp is tested to agree)."""
    cs = (ROOT / "src.managed/MediaViewer.Updater/UpdateKeys.cs").read_text()
    m = re.search(r'ProductionPublicKeyHex\s*=\s*"([0-9a-f]{64})"', cs)
    if not m:
        raise ValueError("ProductionPublicKeyHex not found in UpdateKeys.cs")
    return m[1]


def public_key_hex(key) -> str:
    from cryptography.hazmat.primitives import serialization
    return key.public_key().public_bytes(serialization.Encoding.Raw,
                                         serialization.PublicFormat.Raw).hex()


def load_private_key(path: Path):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    raw = bytes.fromhex(path.read_text().strip())
    if len(raw) == 64:  # libsodium secret key: seed || public key
        raw = raw[:32]
    if len(raw) != 32:
        raise ValueError("expected a 32-byte Ed25519 seed (or 64-byte libsodium key), hex")
    return Ed25519PrivateKey.from_private_bytes(raw)


def licence_of(rel: str, addon: dict, staged: dict) -> str:
    if rel in staged:
        return staged[rel]
    for prefix, spdx in addon.get("licences", []):
        if rel.startswith(prefix):
            return spdx
    return LICENCE


def collect(src: Path, platform: str, stage: Path, addon_id: str = "import") -> list:
    addon = ADDONS[addon_id]
    spec = addon["files"][platform]
    stage.mkdir(parents=True, exist_ok=True)
    for name in spec["include"]:
        item = src / name
        if not item.exists():
            raise FileNotFoundError(f"{name} is missing from {src}")
        if item.is_dir():
            shutil.copytree(item, stage / name, symlinks=True)
        else:
            shutil.copy2(item, stage / name)
    (stage / "LICENSES").mkdir(exist_ok=True)
    (stage / "LICENSES" / "NOTICE.txt").write_text(addon["notice"])
    for notice in addon.get("ship_notices", []):
        if (src / notice).exists():
            shutil.copy2(src / notice, stage / "LICENSES" / notice)
    # Model files name their licence in the staged licences.json (ai-models.py).
    staged = {}
    if (src / "licences.json").exists():
        staged = json.loads((src / "licences.json").read_text(encoding="utf-8"))
    files = []
    for p in sorted(stage.rglob("*")):
        if p.is_symlink():
            raise ValueError(f"{p.relative_to(stage)} is a symlink; the app never follows links")
        if p.is_file():
            rel = p.relative_to(stage).as_posix()
            spdx = licence_of(rel, addon, staged)
            if spdx not in ALLOWED_LICENCES:
                raise ValueError(f"{rel} is licensed {spdx}: not redistributable in the pack")
            files.append({"path": rel, "sha256": sha256_file(p), "size": p.stat().st_size,
                          "licence": spdx})
    return files


def make_archive(stage: Path, archive: Path, platform: str):
    if archive.exists():
        archive.unlink()
    if platform == "macos" and shutil.which("ditto"):
        subprocess.run(["ditto", "-c", "-k", str(stage), str(archive)], check=True)
        return
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(stage).as_posix())


def build_manifest(platform: str, version: str, files: list, archive: Path,
                   addon_id: str = "import") -> bytes:
    addon = ADDONS[addon_id]
    spec = addon["files"][platform]
    manifest = {
        "schema": 1,
        "id": addon_id,
        "name": addon["name"],
        "version": version,
        "platform": platform,
        "host_api": addon["host_api"],
        "installed_size": sum(f["size"] for f in files),
        "native": spec.get("native", ""),
        "chrome": spec.get("chrome", ""),
        "archive": {"path": archive.name, "sha256": sha256_file(archive),
                    "size": archive.stat().st_size},
        "files": files,
    }
    if addon.get("part_of"):
        manifest["part_of"] = addon["part_of"]
    if spec.get("arch"):
        manifest["arch"] = spec["arch"]
    return (json.dumps(manifest, indent=1, sort_keys=False) + "\n").encode()


def pack(args) -> Path:
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", args.version):
        raise ValueError("version must be x.y.z")
    key = load_private_key(Path(args.key))
    # A release signed with any other key is refused by every app; fail here.
    if args.require_pinned_key and public_key_hex(key) != pinned_public_key_hex():
        raise ValueError("the signing key is not the pinned update key (UpdateKeys.cs)")
    addon_id = getattr(args, "addon", None) or "import"
    if args.platform not in ADDONS[addon_id]["files"]:
        raise ValueError(f"{addon_id} is not built for {args.platform}")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    base = f"mediaviewer-addon-{addon_id}-{args.platform}"
    stage = out / (base + ".stage")
    if stage.exists():
        shutil.rmtree(stage)
    files = collect(Path(args.src), args.platform, stage, addon_id)
    archive = out / (base + ".zip")
    make_archive(stage, archive, args.platform)
    manifest = build_manifest(args.platform, args.version, files, archive, addon_id)
    (out / (base + ".json")).write_bytes(manifest)
    (out / (base + ".json.sig")).write_bytes(key.sign(manifest))
    shutil.rmtree(stage)
    print(f"packed {base}: {len(files)} files, {archive.stat().st_size} bytes")
    return out / (base + ".json")


def verify(args) -> int:
    """Signature and archive checks, the same rules the app applies first."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
    manifest_path = Path(args.manifest)
    data = manifest_path.read_bytes()
    sig = Path(str(manifest_path) + ".sig").read_bytes()
    pub = Ed25519PublicKey.from_public_bytes(bytes.fromhex(args.public_key))
    try:
        pub.verify(sig, data)
    except InvalidSignature:
        print("bad_signature")
        return 1
    m = json.loads(data)
    archive = manifest_path.parent / m["archive"]["path"]
    if sha256_file(archive) != m["archive"]["sha256"] or archive.stat().st_size != m["archive"]["size"]:
        print("archive_mismatch")
        return 1
    print("ok")
    return 0


def ceiling(args) -> int:
    """Every supported installed combination of a family within its ceiling:
    the parent, plus at most one vendor runtime piece, plus every model piece."""
    by_family = {}
    for path in args.manifests:
        m = json.loads(Path(path).read_bytes())
        family = m.get("part_of") or m["id"]
        by_family.setdefault(family, []).append(m)
    bad = 0
    for family, members in by_family.items():
        limit = CEILINGS.get(family)
        if not limit:
            continue
        runtimes = [m for m in members if m["id"].startswith(family + "-") and not m["native"]
                    and any(f["path"].endswith((".dll", ".dylib", ".so")) for f in m["files"])]
        others = [m for m in members if m not in runtimes]
        base = sum(m["installed_size"] for m in others)
        worst = base + max((m["installed_size"] for m in runtimes), default=0)
        print(f"{family}: largest supported combination {worst / 1e9:.3f} GB of {limit / 1e9:.1f} GB")
        if worst > limit:
            print(f"{family}: over the ceiling", file=sys.stderr)
            bad = 1
    return bad


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--addon", choices=sorted(ADDONS), default="import")
    p.add_argument("--platform", choices=["macos", "win-x64"], required=True)
    p.add_argument("--src", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--key", required=True, help="file holding the Ed25519 private key, hex")
    p.add_argument("--out", required=True)
    p.add_argument("--require-pinned-key", action="store_true",
                   help="fail unless the key is the one the app pins (release builds)")
    v = sub.add_parser("verify")
    v.add_argument("manifest")
    v.add_argument("--public-key", required=True, help="32-byte Ed25519 public key, hex")
    c = sub.add_parser("ceiling")
    c.add_argument("manifests", nargs="+")
    args = ap.parse_args(argv)
    if args.cmd == "pack":
        pack(args)
        return 0
    if args.cmd == "ceiling":
        return ceiling(args)
    return verify(args)


if __name__ == "__main__":
    sys.exit(main())
