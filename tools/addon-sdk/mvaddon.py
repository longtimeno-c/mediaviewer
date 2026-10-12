#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: MIT
"""Make a MediaViewer add-on (docs/design/25, docs/ADDONS.md).

    mvaddon.py keygen --out publisher.key
    mvaddon.py init my-addon --id you.my-addon --name "My Add-on" --publisher "You"
    mvaddon.py init my-tool --script --id you.my-tool --name "My Tool" --publisher "You"
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
API = 3  # the contribution API this SDK knows: 1 themes; 2 settings, keymaps, shape; 3 scripts, permissions, commands
THEME_API = 1  # what a theme-only add-on claims, so it installs on every MediaViewer with themes
SCRIPT_API = 3  # the API that introduced code (PR 57)
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
# Every contribution point docs/design/25 §7 names, and the API that introduces it
# (src/addon/open_manifest.h kContributionPoints).
CONTRIBUTION_POINTS = {"themes": 1, "settings": 2, "keymaps": 2, "commands": 3, "screens": 4,
                       "info_sections": 5, "search_providers": 5, "bar_items": 5,
                       "path_items": 5, "context_menu": 5, "status_items": 5, "tile_badges": 5}
THEME_SCHEMA = 2  # 2 adds "shape"
SHAPE_RANGES = {"radius": (0.0, 2.0), "density": (0.75, 1.25), "type_scale": (0.85, 1.25)}
CODE_KEYS = {"native", "chrome", "scripts", "main"}
                  "update_url", "api", "installed_size", "files", "contributes",
                  "scripts", "permissions"}
# Native code, by any name: refused by every MediaViewer. Other makers' code
# runs only as a Lua script (docs/design/25 §9).
CODE_KEYS = {"native", "chrome", "main"}
PERMISSIONS = ("folder.read", "thumbnails", "selection")  # what a script may ask for
LATER_PERMISSIONS = {"files.write", "metadata.write"}       # a newer MediaViewer's (PR 60)
MAX_SCRIPTS = 16
MAX_SCRIPT_BYTES = 1 << 20
MAX_SCRIPTS_TOTAL = 4 << 20
MAX_COMMANDS = 32
COMMAND_FIELDS = {"id", "name", "key", "run", "modes", "payload"}
COMMAND_MODES = {"viewing", "video", "browse", "all"}
COMMAND_PAYLOADS = {"none", "marks", "marked_or_current", "screen"}
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
    if any(c in '\\:<>"|?*' or ord(c) < 0x20 for c in path):
        return False
    return all(part not in ("", ".", "..") and part[-1] not in ". " and not _device(part)
               for part in path.split("/"))


def _device(part: str) -> bool:
    """A name Windows opens as a device in every folder, with or without an extension."""
    base = part.split(".", 1)[0].rstrip(" ").lower()
    return base in DEVICE_NAMES or re.fullmatch(r"(com|lpt)[0-9\u00b9\u00b2\u00b3]", base) is not None


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


_HIDDEN = [(0x80, 0x9F), (0x061C, 0x061C), (0x200B, 0x200F), (0x2028, 0x202E), (0x2060, 0x206F),
           (0xFEFF, 0xFEFF)]


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
    schema = doc.get("schema") if isinstance(doc, dict) else None
    if (not isinstance(schema, int) or isinstance(schema, bool) or schema < 1
            or schema > THEME_SCHEMA):
        raise Refused("invalid_theme", "malformed")
    report = {}
    for key, value in doc.items():
        if key == "schema":
            continue
        if key == "shape" and schema >= 2:
            if not isinstance(value, dict):
                raise Refused("invalid_theme", "bad_shape")
            for name, number in value.items():
                bounds = SHAPE_RANGES.get(name)
                if (bounds is None or not isinstance(number, (int, float))
                        or isinstance(number, bool) or not bounds[0] <= number <= bounds[1]):
                    raise Refused("invalid_theme", "bad_shape")
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


# ---- settings pages and keymap packs (API 2) --------------------------------------
# The app's rules (src/addon/settings_schema.cpp, src/addon/keymap.cpp), so `pack`
# never hands out a page or a layout the app would refuse.

SETTINGS_MAX_SECTIONS, SETTINGS_MAX_ITEMS, SETTINGS_MAX_OPTIONS = 16, 64, 32
SETTINGS_MAX_TEXT, NUMBER_LIMIT = 1024, 1e9


def _number(value):
    # At most three decimals: what the app stores and reads back.
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and value == value and abs(value) <= NUMBER_LIMIT
            and abs(value * 1000 - round(value * 1000)) <= 1e-3)


def check_settings(page) -> int:
    """The number of items on a valid page; raises Refused("malformed", why)."""
    def bad(why):
        raise Refused("malformed", "settings: " + why)

    def only(obj, keys, what):
        if any(k not in keys for k in obj):
            bad(what + ": unknown key")

    def text(obj, key, limit, required, what):
        if key not in obj:
            if required:
                bad(f"{what}: {key}")
            return
        if not isinstance(obj[key], str) or not display_text(obj[key], limit, not required):
            bad(f"{what}: {key}")

    if not isinstance(page, dict):
        bad("not an object")
    only(page, ("title", "sections"), "page")
    text(page, "title", 64, False, "page")
    sections = page.get("sections")
    if not isinstance(sections, list) or not sections or len(sections) > SETTINGS_MAX_SECTIONS:
        bad("sections")
    keys, toggles, count = set(), set(), 0
    common = ("type", "key", "label", "help", "enabled_by", "default")
    for section in sections:
        if not isinstance(section, dict):
            bad("section")
        only(section, ("title", "help", "items"), "section")
        text(section, "title", 64, True, "section")
        text(section, "help", 280, False, "section")
        items = section.get("items")
        if not isinstance(items, list) or not items:
            bad("items")
        for item in items:
            count += 1
            if count > SETTINGS_MAX_ITEMS:
                bad("too many items")
            if not isinstance(item, dict):
                bad("item")
            key = item.get("key")
            if not isinstance(key, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,39}", key):
                bad("key")
            text(item, "label", 64, True, key)
            text(item, "help", 280, False, key)
            if "enabled_by" in item and item["enabled_by"] not in toggles:
                bad(f"{key}: enabled_by names no toggle above it")
            if "default" not in item:
                bad(f"{key}: default")
            default, kind = item["default"], item.get("type")
            if kind == "toggle":
                only(item, common, key)
                if not isinstance(default, bool):
                    bad(f"{key}: default")
                toggles.add(key)
            elif kind == "number":
                only(item, common + ("min", "max", "step", "unit"), key)
                low, high, step = item.get("min"), item.get("max"), item.get("step", 1)
                if not _number(low) or not _number(high) or not low < high:
                    bad(f"{key}: range")
                if not _number(step) or step < 0.001 or step > high - low:
                    bad(f"{key}: step")
                text(item, "unit", 16, False, key)
                if not _number(default) or not low <= default <= high:
                    bad(f"{key}: default")
            elif kind == "choice":
                only(item, common + ("options",), key)
                options = item.get("options")
                if not isinstance(options, list) or not 2 <= len(options) <= SETTINGS_MAX_OPTIONS:
                    bad(f"{key}: options")
                values = set()
                for o in options:
                    if (not isinstance(o, dict) or any(k not in ("value", "label") for k in o)
                            or not isinstance(o.get("value"), str)
                            or not re.fullmatch(r"[a-z0-9_-]{1,40}", o["value"])
                            or o["value"] in values):
                        bad(f"{key}: option")
                    text(o, "label", 64, True, key)
                    values.add(o["value"])
                if default not in values:
                    bad(f"{key}: default")
            elif kind == "text":
                only(item, common + ("max_length", "placeholder"), key)
                limit = item.get("max_length", 256)
                if (not isinstance(limit, int) or isinstance(limit, bool)
                        or not 1 <= limit <= SETTINGS_MAX_TEXT):
                    bad(f"{key}: max_length")
                text(item, "placeholder", 64, False, key)
                if (not isinstance(default, str) or len(default.encode()) > limit
                        or any(ord(c) < 0x20 or ord(c) == 0x7F for c in default)):
                    bad(f"{key}: default")
            else:
                bad(f"{key}: type")
            if key in keys:
                bad(f"{key}: duplicate")
            keys.add(key)
    return count


def check_keymaps(packs) -> int:
    """The number of layouts; raises Refused("malformed", why). Command names and
    key labels are checked as text: which command a name means is the app's
    table's to say when the layout is applied, so a layout for a newer
    MediaViewer applies what an older one knows."""
    def bad(why):
        raise Refused("malformed", "keymaps: " + why)

    if not isinstance(packs, list) or not 1 <= len(packs) <= 8:
        bad("1 to 8 layouts")
    ids = set()
    for pack in packs:
        if not isinstance(pack, dict) or any(k not in ("id", "name", "bindings") for k in pack):
            bad("layout")
        pid = pack.get("id")
        if (not isinstance(pid, str) or not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?", pid)
                or pid in ids):
            bad("id")
        ids.add(pid)
        if not isinstance(pack.get("name"), str) or not display_text(pack["name"], 64):
            bad(f"{pid}: name")
        bindings = pack.get("bindings")
        if not isinstance(bindings, list) or not 1 <= len(bindings) <= 256:
            bad(f"{pid}: bindings")
        commands = set()
        for b in bindings:
            if not isinstance(b, dict) or any(k not in ("command", "keys") for k in b):
                bad(f"{pid}: binding")
            command, keys = b.get("command"), b.get("keys")
            if (not isinstance(command, str)
                    or not re.fullmatch(r"[a-z0-9_.\-]+(?:/[a-z0-9_.\-]+)?", command)
                    or len(command) > 100 or command in commands):
                bad(f"{pid}: command")
            commands.add(command)
            if not isinstance(keys, list) or not 1 <= len(keys) <= 8:
                bad(f"{command}: keys")
            for k in keys:
                if (not isinstance(k, str) or len(k) > 32 or k != k.strip()
                        or any(not 0x20 <= ord(c) <= 0x7E for c in k)):
                    bad(f"{command}: key")
    return len(packs)


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

    # Code (API 3): Lua text files of the package, run in the order listed.
    scripts = []
    if "scripts" in doc:
        if api < SCRIPT_API:
            raise Refused("code_not_allowed")
        value = doc["scripts"]
        if high < SCRIPT_API or not isinstance(value, list) or not value or len(value) > MAX_SCRIPTS:
            raise Refused("malformed", "scripts")
        sizes = {f["path"]: f["size"] for f in files}
        for path in value:
            if not isinstance(path, str) or not path.endswith(".lua"):
                raise Refused("malformed", "scripts")
            if path not in sizes:
                raise Refused("unsafe_path", path)
            if sizes[path] > MAX_SCRIPT_BYTES or path in scripts:
                raise Refused("malformed", "scripts")
            scripts.append(path)
        if sum(sizes[p] for p in scripts) > MAX_SCRIPTS_TOTAL:
            raise Refused("malformed", "scripts")
    granted = set()
    if "permissions" in doc:
        value = doc["permissions"]
        if not scripts or not isinstance(value, list) or len(value) > 16:
            raise Refused("malformed", "permissions")
        for name in value:
            if not isinstance(name, str):
                raise Refused("malformed", "permissions")
            if name not in PERMISSIONS:
                if name == "network":  # there is no network permission, ever
                    raise Refused("malformed", "permissions")
                if name in LATER_PERMISSIONS or not strict:
                    raise Refused("needs_update")
                raise Refused("malformed", "permissions")
            if name in granted:
                raise Refused("malformed", "permissions")
            granted.add(name)

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
        elif name in ("settings", "keymaps") and CONTRIBUTION_POINTS[name] <= api:
            if high < CONTRIBUTION_POINTS[name]:
                raise Refused("malformed", f"{name} needs \"api\" max {CONTRIBUTION_POINTS[name]}")
            (check_settings if name == "settings" else check_keymaps)(value)
        elif name == "commands" and api >= SCRIPT_API:
            if not scripts or not isinstance(value, list) or not value or len(value) > MAX_COMMANDS:
                raise Refused("malformed", "commands")
            ids = set()
            for c in value:
                if not isinstance(c, dict):
                    raise Refused("malformed", "commands")
                cid, cname, run = c.get("id"), c.get("name"), c.get("run")
                if (not isinstance(cid, str) or not isinstance(cname, str) or not isinstance(run, str)
                        or len(cid) > 32 or not _id_part(cid) or not display_text(cname, 64)
                        or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]{0,63}", run) or cid in ids):
                    raise Refused("malformed", "commands")
                if strict and any(k not in COMMAND_FIELDS for k in c):
                    raise Refused("malformed", "commands")
                key = c.get("key", "")
                if not isinstance(key, str) or len(key) > 32 or any(not 0x20 < ord(ch) < 0x7F for ch in key):
                    raise Refused("malformed", "commands")
                if c.get("modes", "viewing") not in COMMAND_MODES:
                    raise Refused("malformed", "commands")
                payload = c.get("payload", "none")
                if payload not in COMMAND_PAYLOADS:
                    raise Refused("malformed", "commands")
                if payload != "none" and "selection" not in granted:
                    raise Refused("malformed", "commands: paths ride along only with selection")
                ids.add(cid)
        elif strict:
            # A point this API does not serve, in a manifest that claims no later one.
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
        # Theme schema 2 (shape) is an API 2 point, as settings and keymaps are.
        if parse_json(entries[t["path"]]).get("schema", 1) >= 2 and manifest["api"]["max"] < 2:
            raise Refused("malformed", f"{t['id']}: a schema 2 theme needs api.max 2")
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


STARTER_SCRIPT = '''-- Runs inside MediaViewer's sandbox: Lua 5.4, no files or network beyond the
-- permissions in addon.json, and stopped if one call uses too much memory or time.
-- docs/ADDONS.md, "Scripts".

function count_files()
  local n = mv.folder.count()
  mv.notify(n == 1 and "1 file in this folder" or (n .. " files in this folder"))
end
'''


def cmd_init(args) -> int:
    root = Path(args.dir)
    if root.exists() and any(root.iterdir()):
        print(f"{root} is not empty.", file=sys.stderr)
        return 1
    if not valid_id(args.id):
        print("The id is publisher.name: lowercase letters, digits and hyphens, with a dot "
              "(you.my-addon).", file=sys.stderr)
        return 1
    if args.script:
        root.mkdir(parents=True, exist_ok=True)
        source = {
            "id": args.id, "name": args.name, "version": "1.0.0",
            "description": "", "licence": "MIT",
            "publisher": {"name": args.publisher, "url": ""},
            "update_url": "",
            "api": {"min": SCRIPT_API, "max": SCRIPT_API},
            "scripts": ["main.lua"],
            "permissions": ["folder.read"],
            "contributes": {"commands": [{"id": "count-files", "name": "Count files",
                                          "key": "Ctrl+Shift+K", "run": "count_files"}]},
        }
        (root / "addon.json").write_text(json.dumps(source, indent=2, ensure_ascii=False) + "\n")
        (root / "main.lua").write_text(STARTER_SCRIPT)
        print(f"Started {root}. Edit addon.json and main.lua, then pack.")
        return 0
    (root / "themes").mkdir(parents=True)
    source = {
        "id": args.id, "name": args.name, "version": "1.0.0",
        "description": "", "licence": "MIT",
        "publisher": {"name": args.publisher, "url": ""},
        "update_url": "",
        "api": {"min": THEME_API, "max": THEME_API},
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
    # A script is Lua source the app loads as text: a compiled chunk is refused
    # when it runs ("attempt to load a binary chunk"), so refuse it here first.
    data_of = dict(files)
    for path in source.get("scripts", []) if isinstance(source.get("scripts"), list) else []:
        data = data_of.get(path) if isinstance(path, str) else None
        if data is None:
            continue
        if data.startswith(b"\x1bLua"):
            raise Refused("malformed", f"{path} is compiled Lua; MediaViewer loads source text only")
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            raise Refused("malformed", f"{path} is not UTF-8 text") from None
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
    contributes = m.get("contributes") or {}
    page = contributes.get("settings")
    if page:
        for section in page["sections"]:
            print(f"  Settings     {section['title']}: " +
                  ", ".join(f"{i['label']} ({i['type']})" for i in section["items"]))
    for pack in contributes.get("keymaps", []):
        print(f"  Key layout   {pack['name']}: {len(pack['bindings'])} command(s)")
    if m.get("scripts"):
        print(f"  Scripts      {', '.join(m['scripts'])}")
        print(f"  Permissions  {', '.join(m.get('permissions', [])) or 'none'}")
        for c in (m.get("contributes") or {}).get("commands", []):
            key = c.get("key") or "no key"
            print(f"  Command      {c['name']} ({key}) runs {c['run']}()")
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
    i.add_argument("--script", action="store_true",
                   help="a script add-on (a command that counts the open folder's files) instead of a theme")
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
