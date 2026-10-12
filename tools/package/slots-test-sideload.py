#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sideload the screens-and-slots test add-on into a scratch add-ons folder
(docs/design/25 PRs 58-59), for a developer build's own run.

    slots-test-sideload.py --module <build>/addons/slots-test/libmv_slots_test_addon.dylib \
        --key <dev key hex file> --addons <scratch add-ons folder> \
        [--platform macos|win-x64] [--fixtures <folder>]

It writes <addons>/<name>/1.0.0/ (Mac: the folder is the add-on's name, "Slots Test";
Windows: its id, "slots-test") with the module, a small logo.png, manifest.json and
its detached Ed25519 signature: what the store verifies at load. --fixtures also writes
four JPEGs, three with EXIF GPS (two inside the test add-on's box around mainland
Portugal, one outside) and one without.

Only a build configured with -DMV_ADDON_DEV_PUBLIC_KEY=<the key's public half> trusts
it, and only when started with MV_DEV_ADDONS_DIR=<addons> (and MV_DEV_THUMBS_DIR, and a
HOME of its own on the Mac): never install it into a real profile. Every name and symbol
here is test-only (owner, 2026-10-12): nothing looks like Places.

Needs `cryptography` (the system /usr/bin/python3 on the Mac has it) and, for
--fixtures, Pillow.
"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path


def entry(name: str, data: bytes) -> dict:
    return {"path": name, "sha256": hashlib.sha256(data).hexdigest(), "size": len(data),
            "licence": "GPL-3.0-or-later"}


def manifest(module: str, files: list, platform: str) -> dict:
    return {
        "schema": 1,
        "id": "slots-test",
        "name": "Slots Test",
        "version": "1.0.0",
        "platform": platform,
        "host_api": {"min": 3, "max": 3},
        "installed_size": sum(f["size"] for f in files),
        "native": module,
        "chrome": "",
        "description": "Fills every screen and slot point, for testing only.",
        "archive": {"path": "slots-test.zip", "sha256": "a" * 64, "size": 1},
        "files": files,
        "contributes": {
            "api": {"min": 4, "max": 5},
            "screens": [{"id": "explorer", "title": "Slots test window", "place": "window",
                         "width": 900, "height": 600}],
            "info_sections": [{"id": "spot", "title": "Slots test", "when": "location"}],
            "search_providers": [{"id": "spot", "name": "Slots test", "prefixes": ["test:"],
                                  "scope": ["files", "local"]}],
            "bar_items": [{"id": "open", "label": "Slots test", "symbol": "star", "opens": "explorer"}],
            "path_items": [{"id": "find", "label": "Slots test path item", "symbol": "flag",
                            "target": "folder"}],
            "context_menu": [{"id": "mark", "label": "Slots test entry"}],
            "status_items": [{"id": "count", "label": "Slots test"}],
            "tile_badges": [{"id": "geotagged", "label": "Slots test badge", "symbol": "check"}],
        },
    }


def fixtures(out: Path) -> None:
    from PIL import Image
    from PIL.TiffImagePlugin import IFDRational

    def dms(v: float):
        v = abs(v)
        d = int(v)
        m = int((v - d) * 60)
        s = round(((v - d) * 60 - m) * 60, 2)
        return (IFDRational(d, 1), IFDRational(m, 1), IFDRational(int(s * 100), 100))

    out.mkdir(parents=True, exist_ok=True)
    points = {
        "inside_a.jpg": (38.7981, -9.3902, (90, 140, 200)),
        "inside_b.jpg": (41.1496, -8.6110, (120, 170, 90)),
        "outside.jpg": (40.4168, -3.7038, (200, 160, 90)),
        "none.jpg": (None, None, (128, 128, 128)),
    }
    for name, (lat, lon, colour) in points.items():
        im = Image.new("RGB", (1600, 1067), colour)
        exif = Image.Exif()
        exif[0x010F] = "Test"
        if lat is not None:
            gps = exif.get_ifd(0x8825)
            gps[1] = "N" if lat >= 0 else "S"
            gps[2] = dms(lat)
            gps[3] = "E" if lon >= 0 else "W"
            gps[4] = dms(lon)
        im.save(out / name, "JPEG", quality=85, exif=exif)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--module", type=Path, required=True)
    ap.add_argument("--key", type=Path, required=True)
    ap.add_argument("--addons", type=Path, required=True)
    ap.add_argument("--platform", default="macos", choices=["macos", "win-x64"])
    ap.add_argument("--fixtures", type=Path)
    a = ap.parse_args()

    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    raw = bytes.fromhex(a.key.read_text().strip())
    key = Ed25519PrivateKey.from_private_bytes(raw[:32])

    folder = a.addons / ("Slots Test" if a.platform == "macos" else "slots-test")
    if folder.exists():
        shutil.rmtree(folder)
    version = folder / "1.0.0"
    version.mkdir(parents=True)
    data = a.module.read_bytes()
    (version / a.module.name).write_bytes(data)
    files = [entry(a.module.name, data)]
    try:
        from PIL import Image
        Image.new("RGB", (32, 32), (200, 200, 200)).save(version / "logo.png")
        files.append(entry("logo.png", (version / "logo.png").read_bytes()))
    except ImportError:
        pass
    text = json.dumps(manifest(a.module.name, files, a.platform), indent=1).encode()
    (version / "manifest.json").write_bytes(text)
    (version / "manifest.json.sig").write_bytes(key.sign(text))
    print("sideloaded", version)
    if a.fixtures:
        fixtures(a.fixtures)
        print("fixtures", a.fixtures)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
