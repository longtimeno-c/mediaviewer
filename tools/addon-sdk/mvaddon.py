#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: MIT
"""Make a MediaViewer add-on (docs/design/25, docs/ADDONS.md).

    mvaddon.py keygen --out publisher.key
    mvaddon.py init my-addon --id you.my-addon --name "My Add-on" --publisher "You"
    mvaddon.py pack my-addon --key publisher.key --out dist/
    mvaddon.py check dist/you.my-addon-1.0.0.mvaddon

One file, no dependencies beyond Python 3.8. MIT, unlike the app: using it
puts no licence on your add-on.

`pack` reads `addon.json` from the folder, lists every other file in it with
its SHA-256, adds your public key, signs the result, and writes one
`.mvaddon`: a ZIP with nothing compressed, which is the only kind MediaViewer
opens. `check` says what MediaViewer will say about a package, and for each
theme the contrast its text has against its backgrounds.

Your key is who you are to the people who install your add-on: MediaViewer
lets only the key that first installed an add-on update it. Keep
`publisher.key` out of your repository, and keep a copy somewhere safe. If
you lose it, your users must remove the add-on and install it again under a
new key.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import zlib

SCHEMA = 2
API = 1  # the contribution API this SDK writes for: themes
PACKAGE_MAX_BYTES = 64 << 20
PACKAGE_MAX_ENTRIES = 2048
MANIFEST_MAX_BYTES = 256 << 10
THEME_MAX_BYTES = 64 << 10
EXTENSION = ".mvaddon"

THEME_TOKENS = ("canvas", "surface", "title", "body", "disabled", "hairline", "accent")
TITLE_CONTRAST = 4.5
BODY_CONTRAST = 3.0

TOP_LEVEL_KEYS = {"schema", "id", "name", "version", "description", "licence", "publisher",
                  "update_url", "api", "installed_size", "files", "contributes"}
CONTRIBUTES_KEYS = {"themes"}
CODE_KEYS = {"native", "chrome", "scripts", "main"}
DEVICE_NAMES = {"con", "prn", "aux", "nul"}


class Refused(Exception):
    """What MediaViewer would say: a reason name (as the app spells it) and a detail."""

    def __init__(self, why: str, detail: str = ""):
        super().__init__(f"{why}: {detail}" if detail else why)
        self.why = why
        self.detail = detail


# ---- Ed25519 (RFC 8032, section 6), so the SDK needs nothing installed --------
# Signing a manifest takes a few milliseconds. This is not constant-time; it
# runs on your own machine, over your own key.

_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _recover_x(y: int, sign: int):
    if y >= _P:
        return None
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P)
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _I % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_GY = 4 * pow(5, _P - 2, _P) % _P
_GX = _recover_x(_GY, 0)
_G = (_GX, _GY, 1, _GX * _GY % _P)


def _add(a, b):
    a_, b_ = (a[1] - a[0]) * (b[1] - b[0]) % _P, (a[1] + a[0]) * (b[1] + b[0]) % _P
    c, d = 2 * a[3] * b[3] * _D % _P, 2 * a[2] * b[2] % _P
    e, f, g, h = b_ - a_, d - c, d + c, b_ + a_
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(s: int, point):
    q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            q = _add(q, point)
        point = _add(point, point)
        s >>= 1
    return q


def _compress(point) -> bytes:
    zinv = pow(point[2], _P - 2, _P)
    x, y = point[0] * zinv % _P, point[1] * zinv % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(s: bytes):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    return None if x is None else (x, y, 1, x * y % _P)


def _equal(a, b) -> bool:
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _expand(seed: bytes):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def ed25519_public(seed: bytes) -> bytes:
    a, _ = _expand(seed)
    return _compress(_mul(a, _G))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    a, prefix = _expand(seed)
    public = _compress(_mul(a, _G))
    r = int.from_bytes(hashlib.sha512(prefix + message).digest(), "little") % _L
    big_r = _compress(_mul(r, _G))
    h = int.from_bytes(hashlib.sha512(big_r + public + message).digest(), "little") % _L
    return big_r + int.to_bytes((r + h * a) % _L, 32, "little")


def ed25519_verify(public: bytes, message: bytes, signature: bytes) -> bool:
    if len(public) != 32 or len(signature) != 64:
        return False
    a = _decompress(public)
    big_r = _decompress(signature[:32])
    s = int.from_bytes(signature[32:], "little")
    if a is None or big_r is None or s >= _L:
        return False
    h = int.from_bytes(hashlib.sha512(signature[:32] + public + message).digest(), "little") % _L
    return _equal(_mul(s, _G), _add(big_r, _mul(h, a)))


# ---- the rules, as the app applies them ---------------------------------------

def safe_relative_path(path: str) -> bool:
    if not path or len(path.encode()) > 512 or path[0] in "/\\" or path[-1] == "/":
        return False
    if "\\" in path or ":" in path or any(ord(c) < 0x20 for c in path):
        return False
    return all(part not in ("", ".", "..") for part in path.split("/"))


def _id_part(part: str) -> bool:
    if not re.fullmatch(r"[a-z0-9]([a-z0-9-]*[a-z0-9])?", part):
        return False
    if part in DEVICE_NAMES or re.fullmatch(r"(com|lpt)[0-9]", part):
        return False
    return True


def valid_id(addon_id: str) -> bool:
    parts = addon_id.split(".")
    return (3 <= len(addon_id) <= 64 and len(parts) >= 2 and all(_id_part(p) for p in parts)
            and parts[0] != "mediaviewer")


def valid_version(version: str) -> bool:
    return re.fullmatch(r"[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}", version) is not None


def valid_https_url(url: str) -> bool:
    if len(url) > 2048 or not url.startswith("https://"):
        return False
    if any(ord(c) <= 0x20 or ord(c) >= 0x7F or c in '"<>\\`' for c in url):
        return False
    host = re.split(r"[/?#]", url[len("https://"):], maxsplit=1)[0]
    return bool(host) and re.fullmatch(r"[A-Za-z0-9.\-:\[\]]+", host) is not None


_HIDDEN = [(0x200B, 0x200F), (0x2028, 0x202E), (0x2060, 0x206F), (0xFEFF, 0xFEFF)]


def display_text(text: str, max_bytes: int, may_be_empty: bool = False) -> bool:
    """Text a person reads in the install sheet: nothing that hides or reorders it."""
    if text == "":
        return may_be_empty
    if len(text.encode()) > max_bytes or text[0] == " " or text[-1] == " ":
        return False
    for c in text:
        n = ord(c)
        if n < 0x20 or n == 0x7F or any(lo <= n <= hi for lo, hi in _HIDDEN):
            return False
    return True


def fingerprint(key_hex: str) -> str:
    digest = hashlib.sha256(bytes.fromhex(key_hex)).hexdigest()[:16]
    return "-".join(digest[i:i + 4] for i in range(0, 16, 4))


# ---- themes ---------------------------------------------------------------------

def _colour(text):
    if not isinstance(text, str) or not re.fullmatch(r"#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})", text):
        raise Refused("invalid_theme", "bad_colour")
    raw = bytes.fromhex(text[1:])
    return (raw[0], raw[1], raw[2], raw[3] if len(raw) == 4 else 255)


def _linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def contrast_ratio(fg, bg) -> float:
    """WCAG 2 contrast of `fg` (r, g, b, a) drawn over the opaque `bg`."""
    a = fg[3] / 255.0
    mixed = [(fg[i] / 255.0) * a + (bg[i] / 255.0) * (1.0 - a) for i in range(3)]

    def luminance(rgb):
        return 0.2126 * _linear(rgb[0]) + 0.7152 * _linear(rgb[1]) + 0.0722 * _linear(rgb[2])

    l1, l2 = luminance(mixed), luminance([bg[i] / 255.0 for i in range(3)])
    return (max(l1, l2) + 0.05) / (min(l1, l2) + 0.05)


def _no_duplicates(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise ValueError("duplicate key " + key)
        seen[key] = value
    return seen


def parse_json(data: bytes):
    """Strict, as the app's reader is: UTF-8, no duplicate keys, no NaN."""
    def refuse(_):
        raise ValueError("not JSON")
    return json.loads(data.decode("utf-8"), object_pairs_hook=_no_duplicates,
                      parse_constant=refuse)


def check_theme(data: bytes):
    """Returns {"dark": {...ratios...}, "light": {...}} or raises Refused."""
    if not data or len(data) > THEME_MAX_BYTES:
        raise Refused("invalid_theme", "malformed")
    try:
        doc = parse_json(data)
    except (ValueError, UnicodeDecodeError):
        raise Refused("invalid_theme", "malformed") from None
    if not isinstance(doc, dict) or doc.get("schema") != 1 or isinstance(doc.get("schema"), bool):
        raise Refused("invalid_theme", "malformed")
    report = {}
    for key, value in doc.items():
        if key == "schema":
            continue
        if key == "font":
            if (not isinstance(value, str) or len(value.encode()) > 64
                    or any(ord(c) < 0x20 or ord(c) == 0x7F for c in value)):
                raise Refused("invalid_theme", "malformed")
            continue
        if key not in ("dark", "light"):
            raise Refused("invalid_theme", "unknown_token")
        if not isinstance(value, dict):
            raise Refused("invalid_theme", "malformed")
        for token in value:
            if token not in THEME_TOKENS:
                raise Refused("invalid_theme", "unknown_token")
        palette = {token: _colour(text) for token, text in value.items()}
        if set(palette) != set(THEME_TOKENS):
            raise Refused("invalid_theme", "missing_token")
        if palette["canvas"][3] != 255 or palette["surface"][3] != 255:
            raise Refused("invalid_theme", "translucent")
        ratios = {}
        for bg in ("canvas", "surface"):
            for fg, floor in (("title", TITLE_CONTRAST), ("body", BODY_CONTRAST),
                              ("accent", BODY_CONTRAST)):
                ratio = contrast_ratio(palette[fg], palette[bg])
                ratios[f"{fg} on {bg}"] = (ratio, floor)
        report[key] = ratios
        low = [name for name, (ratio, floor) in ratios.items() if ratio < floor]
        if low:
            error = Refused("invalid_theme", "low_contrast")
            error.report = report
            raise error
    if not report:
        raise Refused("invalid_theme", "malformed")
    return report


# ---- the manifest ---------------------------------------------------------------

def check_manifest(data: bytes, signature, api: int = API) -> dict:
    """The manifest, if MediaViewer would accept it; raises Refused otherwise."""
    if not data or len(data) > MANIFEST_MAX_BYTES:
        raise Refused("malformed")
    if not signature:
        raise Refused("missing_signature")
    try:
        doc = parse_json(data)
    except (ValueError, UnicodeDecodeError):
        raise Refused("malformed") from None
    if not isinstance(doc, dict):
        raise Refused("malformed")

    def integer(value):
        return isinstance(value, int) and not isinstance(value, bool)

    if not integer(doc.get("schema")):
        raise Refused("malformed")
    if doc["schema"] != SCHEMA:
        raise Refused("unsupported_schema")
    publisher = doc.get("publisher")
    if not isinstance(publisher, dict):
        raise Refused("malformed")
    key = publisher.get("key")
    if not isinstance(key, str) or not re.fullmatch(r"[0-9a-f]{64}", key):
        raise Refused("malformed")
    if not ed25519_verify(bytes.fromhex(key), data, bytes(signature)):
        raise Refused("bad_signature")

    api_range = doc.get("api")
    files = doc.get("files")
    size = doc.get("installed_size")
    for name in ("id", "name", "version", "licence"):
        if not isinstance(doc.get(name), str):
            raise Refused("malformed", name)
    if (not isinstance(publisher.get("name"), str) or not isinstance(api_range, dict)
            or not isinstance(files, list) or not files or not integer(size) or size < 0):
        raise Refused("malformed")
    low, high = api_range.get("min"), api_range.get("max")
    if not integer(low) or not integer(high) or low < 1 or high < low or high > 1_000_000:
        raise Refused("malformed", "api")
    if not valid_id(doc["id"]):
        raise Refused("malformed", "id")
    if not valid_version(doc["version"]):
        raise Refused("malformed", "version")
    for text, limit in ((doc["name"], 64), (publisher["name"], 64), (doc["licence"], 64)):
        if not display_text(text, limit):
            raise Refused("malformed", "text")
    if "description" in doc and (not isinstance(doc["description"], str)
                                 or not display_text(doc["description"], 280, True)):
        raise Refused("malformed", "description")
    for holder, name in ((publisher, "url"), (doc, "update_url")):
        if name in holder:
            url = holder[name]
            if not isinstance(url, str) or (url and not valid_https_url(url)):
                raise Refused("malformed", name)

    contributes = doc.get("contributes")
    if any(k in CODE_KEYS for k in doc):
        raise Refused("code_not_allowed")
    if contributes is not None:
        if not isinstance(contributes, dict):
            raise Refused("malformed", "contributes")
        if any(k in CODE_KEYS for k in contributes):
            raise Refused("code_not_allowed")

    seen = set()
    total = 0
    for f in files:
        if not isinstance(f, dict):
            raise Refused("malformed", "files")
        path, sha, fsize, licence = f.get("path"), f.get("sha256"), f.get("size"), f.get("licence")
        if (not isinstance(path, str) or not isinstance(sha, str) or not integer(fsize)
                or fsize < 0 or not re.fullmatch(r"[0-9a-f]{64}", sha)
                or not isinstance(licence, str) or not display_text(licence, 64)):
            raise Refused("malformed", "files")
        if not safe_relative_path(path):
            raise Refused("unsafe_path", path)
        folded = path.lower() if path.isascii() else "".join(
            c.lower() if "A" <= c <= "Z" else c for c in path)
        if folded in ("manifest.json", "manifest.json.sig") or folded in seen:
            raise Refused("malformed", path)
        seen.add(folded)
        total += fsize
    if total != size:
        raise Refused("malformed", "installed_size")

    if low > api:
        raise Refused("needs_update")
    strict = high <= api
    if strict and any(k not in TOP_LEVEL_KEYS for k in doc):
        raise Refused("malformed", "unknown key")
    for name, value in (contributes or {}).items():
        if name == "themes":
            if not isinstance(value, list) or not value or len(value) > 32:
                raise Refused("malformed", "themes")
            ids = set()
            for t in value:
                if not isinstance(t, dict):
                    raise Refused("malformed", "themes")
                tid, tname, tpath = t.get("id"), t.get("name"), t.get("path")
                if (not isinstance(tid, str) or not isinstance(tname, str)
                        or not isinstance(tpath, str) or len(tid) > 32 or not _id_part(tid)
                        or not display_text(tname, 64) or tid in ids):
                    raise Refused("malformed", "themes")
                ids.add(tid)
                if tpath not in [f["path"] for f in files]:
                    raise Refused("unsafe_path", tpath)
        elif strict and name not in CONTRIBUTES_KEYS:
            raise Refused("malformed", "unknown key")
    return doc


# ---- the package ----------------------------------------------------------------

_LOCAL, _CENTRAL, _END = 0x04034b50, 0x02014b50, 0x06054b50
_UTF8 = 1 << 11


def write_package(entries) -> bytes:
    """`entries`: [(name, bytes)]. Stored, end to end, no extras: the only ZIP
    MediaViewer opens."""
    out = bytearray()
    directory = bytearray()
    for name, data in entries:
        raw = name.encode("utf-8")
        flags = 0 if name.isascii() else _UTF8
        crc = zlib.crc32(data) & 0xFFFFFFFF
        at = len(out)
        # A fixed date: two packs of the same files are the same bytes.
        out += struct.pack("<IHHHHHIIIHH", _LOCAL, 20, flags, 0, 0, 0x21, crc, len(data),
                           len(data), len(raw), 0)
        out += raw + data
        directory += struct.pack("<IHHHHHHIIIHHHHHII", _CENTRAL, 20, 20, flags, 0, 0, 0x21, crc,
                                 len(data), len(data), len(raw), 0, 0, 0, 0, 0, at)
        directory += raw
    at = len(out)
    out += directory
    out += struct.pack("<IHHHHIIH", _END, 0, 0, len(entries), len(entries), len(directory), at, 0)
    return bytes(out)


def read_package(package: bytes):
    """[(name, bytes)] of a strict package; raises Refused otherwise."""
    if len(package) > PACKAGE_MAX_BYTES:
        raise Refused("too_large")
    if len(package) < 22:
        raise Refused("bad_package")
    end = len(package) - 22
    sig, disk, cd_disk, here, total, cd_size, cd_at, comment = struct.unpack_from(
        "<IHHHHIIH", package, end)
    if sig != _END or disk or cd_disk or here != total or comment:
        raise Refused("bad_package", "not a package, or it carries a comment")
    if total in (0, 0xFFFF) or cd_size == 0xFFFFFFFF or cd_at == 0xFFFFFFFF:
        raise Refused("bad_package", "ZIP64")
    if total > PACKAGE_MAX_ENTRIES:
        raise Refused("too_large")
    if cd_at + cd_size != end:
        raise Refused("bad_package")
    entries, seen, at, next_local = [], set(), cd_at, 0
    for _ in range(total):
        if end - at < 46:
            raise Refused("bad_package")
        (sig, _made, _need, flags, method, _time, _date, crc, packed, size, name_len, extra_len,
         note_len, start_disk, _internal, _external, local_at) = struct.unpack_from(
             "<IHHHHHHIIIHHHHHII", package, at)
        if sig != _CENTRAL:
            raise Refused("bad_package")
        if method != 0 or packed != size:
            raise Refused("bad_package", "compressed; pack with mvaddon.py, which stores")
        if flags & ~_UTF8 or extra_len or note_len or start_disk or not name_len:
            raise Refused("bad_package", "encrypted, or with extra fields")
        if end - at - 46 < name_len:
            raise Refused("bad_package")
        raw = package[at + 46:at + 46 + name_len]
        at += 46 + name_len
        try:
            name = raw.decode("utf-8")
        except UnicodeDecodeError:
            raise Refused("unsafe_path") from None
        if not safe_relative_path(name):
            raise Refused("unsafe_path", name)
        folded = "".join(c.lower() if "A" <= c <= "Z" else c for c in name)
        if folded in seen:
            raise Refused("bad_package", "two entries with one name")
        seen.add(folded)
        if local_at != next_local or cd_at - local_at < 30:
            raise Refused("bad_package")
        (lsig, _lneed, lflags, lmethod, _lt, _ld, lcrc, lpacked, lsize, lname_len,
         lextra_len) = struct.unpack_from("<IHHHHHIIIHH", package, local_at)
        if (lsig != _LOCAL or lflags != flags or lmethod != method or lcrc != crc
                or lpacked != packed or lsize != size or lname_len != name_len or lextra_len):
            raise Refused("bad_package")
        data_at = local_at + 30 + name_len
        if data_at > cd_at or cd_at - data_at < size:
            raise Refused("bad_package")
        if package[local_at + 30:data_at] != raw:
            raise Refused("bad_package")
        next_local = data_at + size
        entries.append((name, package[data_at:data_at + size]))
    if next_local != cd_at or at != end:
        raise Refused("bad_package")
    return entries


def check_package(package: bytes) -> dict:
    """What MediaViewer will make of a package: the manifest and a report per
    theme. Raises Refused with the app's reason."""
    entries = dict(read_package(package))
    if "manifest.json" not in entries:
        raise Refused("malformed", "no manifest.json")
    manifest = check_manifest(entries["manifest.json"], entries.get("manifest.json.sig"))
    for f in manifest["files"]:
        data = entries.get(f["path"])
        if data is None:
            raise Refused("file_missing", f["path"])
        if len(data) != f["size"] or hashlib.sha256(data).hexdigest() != f["sha256"]:
            raise Refused("file_mismatch", f["path"])
    listed = {f["path"] for f in manifest["files"]} | {"manifest.json", "manifest.json.sig"}
    for name in entries:
        if name not in listed:
            raise Refused("unexpected_file", name)
    themes = {}
    for t in (manifest.get("contributes") or {}).get("themes", []):
        try:
            themes[t["id"]] = check_theme(entries[t["path"]])
        except Refused as e:
            error = Refused("invalid_theme", f"{t['id']}: {e.detail}")
            error.report = getattr(e, "report", None)
            raise error from None
    return {"manifest": manifest, "themes": themes,
            "sha256": hashlib.sha256(package).hexdigest()}


# ---- commands -------------------------------------------------------------------

def load_key(path: Path) -> bytes:
    raw = bytes.fromhex(path.read_text().strip())
    if len(raw) == 64:  # a libsodium secret key: seed || public key
        raw = raw[:32]
    if len(raw) != 32:
        raise ValueError("expected a 32-byte Ed25519 seed, hex")
    return raw


def cmd_keygen(args) -> int:
    out = Path(args.out)
    if out.exists():
        print(f"{out} exists. A key is not something to overwrite.", file=sys.stderr)
        return 1
    seed = os.urandom(32)
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(seed.hex() + "\n")
    public = ed25519_public(seed).hex()
    print(f"Wrote {out}. Keep it private, and keep a copy.")
    print(f"Public key   {public}")
    print(f"Fingerprint  {fingerprint(public)}   (what people see when they install)")
    return 0


STARTER_THEME = {
    "schema": 1,
    "dark": {"canvas": "#1c1b1a", "surface": "#262523", "title": "#f2efe9", "body": "#b9b4aa",
             "disabled": "#6f6b64", "hairline": "#3a3835", "accent": "#e0793a"},
    "light": {"canvas": "#f4f1ea", "surface": "#ffffff", "title": "#1c1b1a", "body": "#57534c",
              "disabled": "#9a958c", "hairline": "#d6d1c6", "accent": "#b5542d"},
}


def cmd_init(args) -> int:
    root = Path(args.dir)
    if root.exists() and any(root.iterdir()):
        print(f"{root} is not empty.", file=sys.stderr)
        return 1
    if not valid_id(args.id):
        print("The id is publisher.name: lowercase letters, digits and hyphens, with a dot "
              "(you.my-addon).", file=sys.stderr)
        return 1
    (root / "themes").mkdir(parents=True)
    source = {
        "id": args.id, "name": args.name, "version": "1.0.0",
        "description": "", "licence": "MIT",
        "publisher": {"name": args.publisher, "url": ""},
        "update_url": "",
        "api": {"min": API, "max": API},
        "contributes": {"themes": [{"id": "example", "name": args.name,
                                    "path": "themes/example.json"}]},
    }
    (root / "addon.json").write_text(json.dumps(source, indent=2, ensure_ascii=False) + "\n")
    (root / "themes" / "example.json").write_text(json.dumps(STARTER_THEME, indent=2) + "\n")
    print(f"Started {root}. Edit addon.json and themes/example.json, then pack.")
    return 0


def collect_files(root: Path):
    found = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if path.is_symlink():
            raise Refused("unsafe_path", f"{rel} is a link")
        if path.is_dir():
            continue
        if rel == "addon.json" or any(part.startswith(".") for part in rel.split("/")):
            continue
        if path.name.lower() in ("thumbs.db", "desktop.ini"):
            continue
        found.append((rel, path.read_bytes()))
    return found


def build_package(root: Path, seed: bytes):
    """(file name, package bytes) for the add-on folder `root`."""
    source = parse_json((root / "addon.json").read_bytes())
    if not isinstance(source, dict):
        raise Refused("malformed", "addon.json")
    for written in ("schema", "installed_size", "files"):
        if written in source:
            raise Refused("malformed", f"addon.json: `{written}` is written by pack")
    licences = source.pop("licences", {})
    publisher = dict(source.get("publisher") or {})
    if "key" in publisher:
        raise Refused("malformed", "addon.json: the key comes from --key")
    publisher["key"] = ed25519_public(seed).hex()
    files = collect_files(root)
    if not files:
        raise Refused("malformed", "the folder holds no files")
    default_licence = source.get("licence", "")
    manifest = {"schema": SCHEMA}
    manifest.update(source)
    manifest["publisher"] = publisher
    manifest["installed_size"] = sum(len(data) for _, data in files)
    manifest["files"] = [{"path": rel, "sha256": hashlib.sha256(data).hexdigest(),
                          "size": len(data), "licence": licences.get(rel, default_licence)}
                         for rel, data in files]
    text = json.dumps(manifest, indent=2, ensure_ascii=False).encode("utf-8")
    package = write_package([("manifest.json", text),
                             ("manifest.json.sig", ed25519_sign(seed, text))] + files)
    # Never hand out a package the app would refuse.
    check_package(package)
    return f"{manifest['id']}-{manifest['version']}{EXTENSION}", package


def describe(result: dict) -> None:
    m = result["manifest"]
    p = m["publisher"]
    print(f"{m['name']} {m['version']}  ({m['id']})")
    print(f"  From         {p['name']}" + (f" · {p['url']}" if p.get("url") else ""))
    print(f"  Fingerprint  {fingerprint(p['key'])}")
    print(f"  Size         {m['installed_size']} bytes in {len(m['files'])} file(s) · {m['licence']}")
    for theme_id, palettes in result["themes"].items():
        for palette, ratios in palettes.items():
            print(f"  Theme {theme_id}, {palette}:")
            for name, (ratio, floor) in ratios.items():
                print(f"    {name:<18} {ratio:5.2f} : 1   (at least {floor} : 1)")
    print(f"  SHA-256      {result['sha256']}")


def cmd_pack(args) -> int:
    try:
        name, package = build_package(Path(args.dir), load_key(Path(args.key)))
    except Refused as e:
        print(f"MediaViewer would refuse this: {e}", file=sys.stderr)
        _print_report(e)
        return 1
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / name).write_bytes(package)
    describe(check_package(package))
    print(f"Wrote {out / name}")
    return 0


def _print_report(error) -> None:
    report = getattr(error, "report", None)
    for palette, ratios in (report or {}).items():
        for name, (ratio, floor) in ratios.items():
            mark = "" if ratio >= floor else "   <- too low"
            print(f"    {palette}: {name:<18} {ratio:5.2f} : 1   (at least {floor} : 1){mark}",
                  file=sys.stderr)


def cmd_check(args) -> int:
    try:
        result = check_package(Path(args.package).read_bytes())
    except Refused as e:
        print(f"refused: {e}")
        _print_report(e)
        return 1
    describe(result)
    print("ok")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen", help="make a publisher key")
    k.add_argument("--out", required=True)
    k.set_defaults(run=cmd_keygen)
    i = sub.add_parser("init", help="start an add-on folder")
    i.add_argument("dir")
    i.add_argument("--id", required=True, help="publisher.name, e.g. you.my-addon")
    i.add_argument("--name", required=True)
    i.add_argument("--publisher", required=True)
    i.set_defaults(run=cmd_init)
    p = sub.add_parser("pack", help="sign and pack a folder into a .mvaddon")
    p.add_argument("dir")
    p.add_argument("--key", required=True, help="the file keygen wrote")
    p.add_argument("--out", required=True, help="folder to write the package into")
    p.set_defaults(run=cmd_pack)
    c = sub.add_parser("check", help="what MediaViewer will say about a package")
    c.add_argument("package")
    c.set_defaults(run=cmd_check)
    args = ap.parse_args(argv)
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
