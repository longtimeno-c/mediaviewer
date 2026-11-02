#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: MIT
"""The add-on SDK against its own rules and against the app's.

With MV_ADDON_VERIFY set to the built `mv_addon_verify`, every package here is
also read by the app's own C++ (src/addon), so the SDK's idea of what
MediaViewer accepts and refuses is proved against MediaViewer, not against a
second Python copy of it (docs/design/25 "Cross-check").
"""
import hashlib
import importlib.util
import json
import re
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("mvaddon", HERE / "mvaddon.py")
mvaddon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mvaddon)

VERIFY = os.environ.get("MV_ADDON_VERIFY", "")
SEED = bytes(range(32))

THEME = json.dumps(mvaddon.STARTER_THEME).encode()


def source(**changes) -> dict:
    doc = {
        "id": "acme.film-tones", "name": "Film Tones", "version": "1.2.0",
        "description": "Warm and cool chrome themes.", "licence": "MIT",
        "publisher": {"name": "Acme Pictures", "url": "https://acme.example"},
        "update_url": "https://acme.example/film-tones.mvaddon",
        "api": {"min": 1, "max": 1},
        "contributes": {"themes": [{"id": "dusk", "name": "Dusk", "path": "themes/dusk.json"}]},
    }
    doc.update(changes)
    return doc


def folder(tmp: Path, doc=None, files=None) -> Path:
    root = tmp / "addon"
    (root / "themes").mkdir(parents=True)
    (root / "addon.json").write_text(json.dumps(doc or source()))
    for rel, data in (files or {"themes/dusk.json": THEME}).items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return root


def signed(manifest: dict, files: dict, seed: bytes = SEED) -> bytes:
    """A package around a manifest written by hand, for the refusals `pack`
    would never produce."""
    text = json.dumps(manifest).encode()
    return mvaddon.write_package([("manifest.json", text),
                                  ("manifest.json.sig", mvaddon.ed25519_sign(seed, text))]
                                 + list(files.items()))


def manifest_for(files: dict, **changes) -> dict:
    doc = {"schema": 2}
    doc.update(source())
    doc["publisher"] = dict(doc["publisher"], key=mvaddon.ed25519_public(SEED).hex())
    doc["installed_size"] = sum(len(d) for d in files.values())
    doc["files"] = [{"path": p, "sha256": hashlib.sha256(d).hexdigest(), "size": len(d),
                     "licence": "MIT"} for p, d in files.items()]
    doc.update(changes)
    return doc


class AppSays:
    """What the app's own reader says about a package, when it is built."""

    @staticmethod
    def about(package: bytes, store=None):
        if not VERIFY:
            return None
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "p.mvaddon"
            path.write_bytes(package)
            cmd = [VERIFY, "--open", str(path)] + ([str(store)] if store else [])
            out = subprocess.run(cmd, capture_output=True, text=True, check=False)
            lines = [json.loads(line) for line in out.stdout.splitlines() if line.startswith("{")]
            return lines


class Ed25519(unittest.TestCase):
    # RFC 8032, section 7.1.
    VECTORS = [
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
         "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
         "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
         "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
         "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
         "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
         "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ]

    def test_rfc8032_vectors(self):
        for seed, public, message, signature in self.VECTORS:
            seed, message = bytes.fromhex(seed), bytes.fromhex(message)
            self.assertEqual(mvaddon.ed25519_public(seed).hex(), public)
            self.assertEqual(mvaddon.ed25519_sign(seed, message).hex(), signature)
            self.assertTrue(mvaddon.ed25519_verify(bytes.fromhex(public), message,
                                                   bytes.fromhex(signature)))

    def test_a_changed_message_or_signature_does_not_verify(self):
        public = mvaddon.ed25519_public(SEED)
        sig = mvaddon.ed25519_sign(SEED, b"manifest")
        self.assertTrue(mvaddon.ed25519_verify(public, b"manifest", sig))
        self.assertFalse(mvaddon.ed25519_verify(public, b"manifesu", sig))
        bad = bytearray(sig)
        bad[5] ^= 1
        self.assertFalse(mvaddon.ed25519_verify(public, b"manifest", bytes(bad)))
        self.assertFalse(mvaddon.ed25519_verify(public, b"manifest", sig[:63]))
        other = mvaddon.ed25519_public(bytes(32))
        self.assertFalse(mvaddon.ed25519_verify(other, b"manifest", sig))


class Pack(unittest.TestCase):
    def test_pack_then_check_and_the_app_agrees(self):
        with tempfile.TemporaryDirectory() as tmp:
            name, package = mvaddon.build_package(folder(Path(tmp)), SEED)
            self.assertEqual(name, "acme.film-tones-1.2.0.mvaddon")
            result = mvaddon.check_package(package)
            self.assertEqual(result["manifest"]["publisher"]["key"],
                             mvaddon.ed25519_public(SEED).hex())
            self.assertIn("dusk", result["themes"])
            said = AppSays.about(package, Path(tmp) / "store")
            if said is not None:
                self.assertEqual(len(said), 2)
                self.assertTrue(said[0]["ok"], said[0])
                self.assertEqual(said[0]["sha256"], result["sha256"])
                self.assertEqual(said[0]["publisher"]["fingerprint"],
                                 mvaddon.fingerprint(result["manifest"]["publisher"]["key"]))
                self.assertTrue(said[1]["ok"], said[1])

    def test_two_packs_of_the_same_folder_are_the_same_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = folder(Path(tmp))
            self.assertEqual(mvaddon.build_package(root, SEED)[1],
                             mvaddon.build_package(root, SEED)[1])

    def test_the_example_packs(self):
        _, package = mvaddon.build_package(ROOT / "examples/addons/film-tones", SEED)
        self.assertEqual(set(mvaddon.check_package(package)["themes"]), {"dusk", "paper"})
        said = AppSays.about(package)
        if said is not None:
            self.assertTrue(said[0]["ok"], said[0])

    def test_the_script_example_packs_and_the_app_agrees(self):
        # docs/design/25 PR 57: a command that, with folder.read, says how many
        # files the open folder holds.
        name, package = mvaddon.build_package(ROOT / "examples/addons/folder-count", SEED)
        self.assertEqual(name, "example.folder-count-1.0.0.mvaddon")
        m = mvaddon.check_package(package)["manifest"]
        self.assertEqual(m["scripts"], ["main.lua"])
        self.assertEqual(m["permissions"], ["folder.read"])
        with tempfile.TemporaryDirectory() as tmp:
            said = AppSays.about(package, Path(tmp) / "store")
            if said is not None:
                self.assertTrue(said[0]["ok"], said[0])
                self.assertTrue(said[0]["script"])
                self.assertEqual(said[0]["permissions"], ["folder.read"])
                self.assertEqual(said[0]["commands"][0]["key"], "Ctrl+Shift+K")
                self.assertIn("See the names of the files in the open folder", said[0]["can"])
                self.assertEqual(said[0]["cannot"], "Change your files or use the network")
                self.assertTrue(said[1]["ok"], said[1])

    def test_init_script_starts_a_folder_that_packs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "tool"
            self.assertEqual(mvaddon.main(["init", str(root), "--script", "--id", "you.tool",
                                           "--name", "Tool", "--publisher", "You"]), 0)
            _, package = mvaddon.build_package(root, SEED)
            self.assertEqual(mvaddon.check_package(package)["manifest"]["api"], {"min": 3, "max": 3})

    def test_pack_refuses_compiled_lua(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "tool"
            mvaddon.main(["init", str(root), "--script", "--id", "you.tool", "--name", "Tool",
                          "--publisher", "You"])
            (root / "main.lua").write_bytes(b"\x1bLuaT\x00\x19\x93")
            with self.assertRaises(mvaddon.Refused) as refused:
                mvaddon.build_package(root, SEED)
            self.assertIn("compiled", refused.exception.detail)

    def test_hidden_files_and_the_source_manifest_stay_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = folder(Path(tmp), files={"themes/dusk.json": THEME, ".DS_Store": b"x",
                                            ".git/config": b"x", "Thumbs.db": b"x"})
            _, package = mvaddon.build_package(root, SEED)
            self.assertEqual([n for n, _ in mvaddon.read_package(package)],
                             ["manifest.json", "manifest.json.sig", "themes/dusk.json"])

    def test_pack_refuses_what_the_app_would(self):
        cases = [
            (source(id="import"), "malformed"),
            (source(id="mediaviewer.tones"), "malformed"),
            (source(version="1.2"), "malformed"),
            (source(name="Film‮Tones"), "malformed"),
            (source(update_url="http://acme.example/a.mvaddon"), "malformed"),
            (source(native=["x.dll"]), "code_not_allowed"),
            (source(scripts=["main.lua"]), "malformed"),  # code under API 1
            (source(colour_scheme=True), "malformed"),
            (source(api={"min": mvaddon.API + 1, "max": mvaddon.API + 1}), "needs_update"),
            (source(schema=2), "malformed"),
        ]
        for doc, why in cases:
            with tempfile.TemporaryDirectory() as tmp, self.subTest(doc=doc):
                with self.assertRaises(mvaddon.Refused) as refused:
                    mvaddon.build_package(folder(Path(tmp), doc), SEED)
                self.assertEqual(refused.exception.why, why)

    def test_settings_keymaps_and_shape_and_the_app_agrees(self):
        page = {"title": "Places", "sections": [{"title": "Place", "items": [
            {"type": "toggle", "key": "show_town", "label": "Name the town", "default": True},
            {"type": "number", "key": "radius", "label": "Radius", "min": 1, "max": 50,
             "step": 1, "unit": "km", "default": 15, "enabled_by": "show_town"},
            {"type": "choice", "key": "names", "label": "Names", "default": "english",
             "options": [{"value": "english", "label": "English"},
                         {"value": "local", "label": "Local"}]},
            {"type": "text", "key": "home", "label": "Home", "default": ""}]}]}
        packs = [{"id": "vim", "name": "Vim", "bindings": [
            {"command": "next", "keys": ["L", "Right"]}, {"command": "toggle_mark", "keys": [""]}]}]
        theme = dict(json.loads(THEME), schema=2, shape={"radius": 0.5, "density": 0.9,
                                                          "type_scale": 1.1})
        doc = source(api={"min": 2, "max": 2},
                     contributes={"themes": [{"id": "dusk", "name": "Dusk", "path": "themes/dusk.json"}],
                                  "settings": page, "keymaps": packs})
        with tempfile.TemporaryDirectory() as tmp:
            _, package = mvaddon.build_package(
                folder(Path(tmp), doc, {"themes/dusk.json": json.dumps(theme).encode()}), SEED)
            mvaddon.check_package(package)
            said = AppSays.about(package, Path(tmp) / "store")
            if said is not None:
                self.assertTrue(said[0]["ok"], said[0])
                self.assertTrue(said[0]["settings"])
                self.assertEqual(said[0]["keymaps"][0]["name"], "Vim")
                self.assertTrue(said[1]["ok"], said[1])
        # Each refused by the SDK and by the app alike.
        broken = [
            ("settings", dict(page, sections=[])),
            ("settings", {"sections": [{"title": "A", "items": [
                {"type": "number", "key": "n", "label": "N", "min": 1, "max": 5, "default": 9}]}]}),
            # A C1 control in a label (U+0085 breaks the line in some renderers).
            ("settings", {"sections": [{"title": "A", "items": [
                {"type": "toggle", "key": "t", "label": "T\u0085x", "default": True}]}]}),
            # More than the three decimals the app stores.
            ("settings", {"sections": [{"title": "A", "items": [
                {"type": "number", "key": "n", "label": "N", "min": 0.0004, "max": 0.0014,
                 "default": 0.0004}]}]}),
            ("settings", {"sections": [{"title": "A", "items": [
                {"type": "toggle", "key": "t", "label": "T", "default": True, "colour": 1}]}]}),
            ("keymaps", [{"id": "Vim", "name": "Vim", "bindings": [{"command": "next", "keys": ["L"]}]}]),
            ("keymaps", [{"id": "vim", "name": "Vim", "bindings": [{"command": "next", "keys": []}]}]),
        ]
        for point, value in broken:
            with self.subTest(point=point, value=value):
                files = {"themes/dusk.json": THEME}
                manifest = manifest_for(files, api={"min": 2, "max": 2})
                manifest["contributes"] = dict(manifest["contributes"], **{point: value})
                self.refused(signed(manifest, files), "malformed")
        # Named under API 1 only: a mistake, for both.
        files = {"themes/dusk.json": THEME}
        manifest = manifest_for(files)
        manifest["contributes"] = dict(manifest["contributes"], settings=page)
        self.refused(signed(manifest, files), "malformed")
        # A shape out of range, or in a schema 1 theme.
        for bad_theme, detail in ((dict(theme, shape={"radius": 3}), "dusk: bad_shape"),
                                  (dict(json.loads(THEME), shape={"radius": 1}), "dusk: unknown_token")):
            files = {"themes/dusk.json": json.dumps(bad_theme).encode()}
            self.refused(signed(manifest_for(files, api={"min": 2, "max": 2}), files),
                         "invalid_theme", detail)
        # A schema 2 theme (shape) from an add-on that claims API 1 only: a
        # mistake, for both, as settings and keymaps under API 1 are.
        for schema2 in (theme, dict(json.loads(THEME), schema=2)):
            files = {"themes/dusk.json": json.dumps(schema2).encode()}
            self.refused(signed(manifest_for(files, api={"min": 1, "max": 1}), files),
                         "malformed", "dusk: a schema 2 theme needs api.max 2")

    def test_the_settings_and_keymap_examples_pack(self):
        for name in ("vim-keys", "settings-demo"):
            with self.subTest(example=name):
                _, package = mvaddon.build_package(ROOT / "examples/addons" / name, SEED)
                mvaddon.check_package(package)
                said = AppSays.about(package)
                if said is not None:
                    self.assertTrue(said[0]["ok"], said[0])

    def test_keygen_never_overwrites_a_key(self):
        with tempfile.TemporaryDirectory() as tmp:
            key = Path(tmp) / "publisher.key"
            self.assertEqual(mvaddon.main(["keygen", "--out", str(key)]), 0)
            first = key.read_text()
            self.assertEqual(len(mvaddon.load_key(key)), 32)
            if os.name == "posix":
                self.assertEqual(key.stat().st_mode & 0o077, 0)
            self.assertEqual(mvaddon.main(["keygen", "--out", str(key)]), 1)
            self.assertEqual(key.read_text(), first)

    def test_init_makes_a_folder_that_packs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "mine"
            self.assertEqual(mvaddon.main(["init", str(root), "--id", "you.my-addon",
                                           "--name", "My Add-on", "--publisher", "You"]), 0)
            name, package = mvaddon.build_package(root, SEED)
            self.assertEqual(name, "you.my-addon-1.0.0.mvaddon")
            said = AppSays.about(package)
            if said is not None:
                self.assertTrue(said[0]["ok"], said[0])


class Refusals(unittest.TestCase):
    """Each with its reason, from the SDK and (when built) from the app."""

    def refused(self, package: bytes, why: str, detail=None):
        with self.assertRaises(mvaddon.Refused) as refused:
            mvaddon.check_package(package)
        self.assertEqual(refused.exception.why, why)
        if detail is not None:
            self.assertEqual(refused.exception.detail, detail)
        said = AppSays.about(package)
        if said is not None:
            self.assertFalse(said[0]["ok"])
            self.assertEqual(said[0]["why"], why, said[0])
            if detail is not None:
                self.assertEqual(said[0]["detail"], detail)

    def test_one_changed_byte(self):
        files = {"themes/dusk.json": THEME}
        package = bytearray(signed(manifest_for(files), files))
        at = bytes(package).find(b'"canvas"')
        package[at + 1] ^= 1
        self.refused(bytes(package), "file_mismatch", "themes/dusk.json")
        package = bytearray(signed(manifest_for(files), files))
        at = bytes(package).find(b"1.2.0")
        package[at] = ord("9")
        self.refused(bytes(package), "bad_signature")

    def test_an_extra_entry(self):
        files = {"themes/dusk.json": THEME}
        manifest = manifest_for(files)
        self.refused(signed(manifest, dict(files, **{"payload.dll": b"MZ"})),
                     "unexpected_file", "payload.dll")

    def test_a_missing_file_or_signature(self):
        files = {"themes/dusk.json": THEME}
        self.refused(signed(manifest_for(files), {}), "file_missing", "themes/dusk.json")
        text = json.dumps(manifest_for(files)).encode()
        self.refused(mvaddon.write_package([("manifest.json", text)] + list(files.items())),
                     "missing_signature")

    def test_a_compressed_zip_is_not_a_package(self):
        import io
        import zipfile
        files = {"themes/dusk.json": THEME}
        text = json.dumps(manifest_for(files)).encode()
        for compression in (zipfile.ZIP_DEFLATED, zipfile.ZIP_STORED):
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w", compression) as z:
                z.writestr("manifest.json", text)
                z.writestr("manifest.json.sig", mvaddon.ed25519_sign(SEED, text))
                z.writestr("themes/dusk.json", THEME)
            if compression == zipfile.ZIP_DEFLATED:
                self.refused(buf.getvalue(), "bad_package")
            else:
                # Python's own writer, storing, happens to be strict enough.
                said = AppSays.about(buf.getvalue())
                try:
                    mvaddon.check_package(buf.getvalue())
                    ours = True
                except mvaddon.Refused:
                    ours = False
                if said is not None:
                    self.assertEqual(said[0]["ok"], ours)

    def test_unsafe_paths(self):
        for bad in ("../evil", "/abs", "a\\b", "C:x", "a//b", "folder/", "themes/COM1",
                    "nul.json", "Aux .json", "lpt\u00b9", "themes/dusk.", "themes /x", "a?b", "a*b"):
            with self.subTest(path=bad):
                files = {"themes/dusk.json": THEME}
                package = signed(manifest_for(files), dict(files, **{bad: b"x"}))
                self.refused(package, "unsafe_path")

    def test_code(self):
        files = {"themes/dusk.json": THEME}
        for key in ("native", "chrome", "main"):
            with self.subTest(key=key):
                self.refused(signed(manifest_for(files, **{key: "x"}), files), "code_not_allowed")

    def test_scripts(self):
        # docs/design/25 PR 57, each case also read by the app's own C++.
        lua = b"function f() mv.notify('hi') end\n"
        files = {"main.lua": lua}
        base = dict(api={"min": 3, "max": 3}, scripts=["main.lua"], contributes={})

        def doc(**changes):
            d = manifest_for(files, **base)
            d.update(changes)
            return d

        ok = signed(doc(permissions=["folder.read"],
                        contributes={"commands": [{"id": "hi", "name": "Hi", "run": "f"}]}), files)
        self.assertEqual(mvaddon.check_package(ok)["manifest"]["permissions"], ["folder.read"])
        said = AppSays.about(ok)
        if said is not None:
            self.assertTrue(said[0]["ok"], said[0])
        cases = [
            (doc(api={"min": 1, "max": 2}), "malformed"),
            (doc(scripts="main.lua"), "malformed"),
            (doc(scripts=["other.lua"]), "unsafe_path"),
            (doc(permissions=["network"]), "malformed"),
            (doc(permissions=["files.write"]), "needs_update"),
            (doc(permissions=["metadata.write"]), "needs_update"),
            (doc(permissions=["folder.read", "folder.read"]), "malformed"),
            (doc(contributes={"commands": [{"id": "hi", "name": "Hi", "run": "f",
                                            "payload": "marks"}]}), "malformed"),
            (doc(contributes={"commands": [{"id": "hi", "name": "Hi", "run": "f.g"}]}), "malformed"),
            (doc(native="x"), "code_not_allowed"),
        ]
        for manifest, why in cases:
            with self.subTest(manifest={k: manifest.get(k) for k in ("api", "scripts", "permissions",
                                                                       "contributes", "native")}):
                self.refused(signed(manifest, files), why)

    def test_screens_and_slots_and_the_app_agrees(self):
        # #400: the host serves contribution API 10. Screens (4), the chrome
        # slots and search providers (5) are filled by code, which a stranger's
        # add-on cannot do yet: code_not_allowed from both, never malformed.
        files = {"themes/dusk.json": THEME}
        themes = [{"id": "dusk", "name": "Dusk", "path": "themes/dusk.json"}]
        points = [k for k, api in mvaddon.CONTRIBUTION_POINTS.items() if api >= mvaddon.SCREENS_API]
        self.assertEqual(len(points), 8)  # screens and the seven slots
        for point in points:
            for api in ({"min": 1, "max": mvaddon.API}, {"min": 5, "max": 5},
                        {"min": 1, "max": mvaddon.API + 5}):
                with self.subTest(point=point, api=api):
                    manifest = manifest_for(files, api=api,
                                            contributes={"themes": themes, point: []})
                    self.refused(signed(manifest, files), "code_not_allowed")

    def test_the_api_range_and_the_app_agrees(self):
        files = {"themes/dusk.json": THEME}
        themes = [{"id": "dusk", "name": "Dusk", "path": "themes/dusk.json"}]

        def accepted(**changes):
            changes.setdefault("contributes", {"themes": themes})
            package = signed(manifest_for(files, **changes), files)
            mvaddon.check_package(package)
            said = AppSays.about(package)
            if said is not None:
                self.assertTrue(said[0]["ok"], said[0])

        # Anything up to the host's own API installs; past it, needs a newer one.
        for top in (1, 4, 5, mvaddon.API):
            with self.subTest(api=top):
                accepted(api={"min": top, "max": top})
        self.refused(signed(manifest_for(files, contributes={"themes": themes},
                                         api={"min": mvaddon.API + 1, "max": mvaddon.API + 1}),
                            files), "needs_update")
        # A point no API names: a mistake while the range ends at or below the
        # host's; left for a later host when the range reaches past it.
        for top in (mvaddon.API, 3):
            with self.subTest(unknown_under=top):
                self.refused(signed(manifest_for(files, api={"min": 1, "max": top},
                                                 contributes={"themes": themes, "toolbar": []}),
                                    files), "malformed")
        accepted(api={"min": 1, "max": mvaddon.API + 1},
                 contributes={"themes": themes, "toolbar": []})

    def test_the_sdk_knows_the_hosts_api_and_points(self):
        # One table on both sides: a host that serves a new API or point and
        # an SDK that does not would give different verdicts (#400).
        header = (Path(__file__).resolve().parents[2] / "src" / "addon" /
                  "open_manifest.h").read_text(encoding="utf-8")
        api = re.search(r"kContributionApi\s*=\s*(\d+);", header)
        self.assertIsNotNone(api)
        self.assertEqual(mvaddon.API, int(api.group(1)))
        script_api = re.search(r"kScriptApi\s*=\s*(\d+);", header)
        self.assertEqual(mvaddon.SCRIPT_API, int(script_api.group(1)))
        table = header[header.index("kContributionPoints{{"):]
        table = table[:table.index("}};")]
        points = {k: int(v) for k, v in re.findall(r'\{"([a-z_]+)",\s*(\d+)\}', table)}
        self.assertEqual(mvaddon.CONTRIBUTION_POINTS, points)
        self.assertEqual(mvaddon.SCREENS_API, points["screens"])

    def test_a_theme_nobody_could_read(self):
        theme = json.loads(THEME)
        theme["dark"]["title"] = "#2a2927"
        files = {"themes/dusk.json": json.dumps(theme).encode()}
        self.refused(signed(manifest_for(files), files), "invalid_theme", "dusk: low_contrast")
        theme = json.loads(THEME)
        del theme["light"]["accent"]
        files = {"themes/dusk.json": json.dumps(theme).encode()}
        self.refused(signed(manifest_for(files), files), "invalid_theme", "dusk: missing_token")
        theme = json.loads(THEME)
        theme["dark"]["glow"] = "#ffffff"
        files = {"themes/dusk.json": json.dumps(theme).encode()}
        self.refused(signed(manifest_for(files), files), "invalid_theme", "dusk: unknown_token")

    def test_another_publishers_package_under_an_installed_id(self):
        if not VERIFY:
            self.skipTest("needs the built mv_addon_verify")
        files = {"themes/dusk.json": THEME}
        with tempfile.TemporaryDirectory() as tmp:
            store = Path(tmp) / "store"
            first = AppSays.about(signed(manifest_for(files), files), store)
            self.assertTrue(first[1]["ok"])
            other = bytes(reversed(SEED))
            manifest = manifest_for(files, version="9.0.0")
            manifest["publisher"]["key"] = mvaddon.ed25519_public(other).hex()
            said = AppSays.about(signed(manifest, files, other), store)
            self.assertEqual(said[0]["why"], "other_publisher")
            older = AppSays.about(signed(manifest_for(files, version="1.0.0"), files), store)
            self.assertEqual(older[0]["why"], "downgrade")
            newer = AppSays.about(signed(manifest_for(files, version="1.3.0"), files), store)
            self.assertEqual(newer[0]["relation"], "update")
            self.assertTrue(newer[1]["ok"])


# Pack's API 2 test checks refusals the same way.
Pack.refused = Refusals.refused


if __name__ == "__main__":
    if VERIFY and not Path(VERIFY).exists():
        sys.exit(f"MV_ADDON_VERIFY names {VERIFY}, which does not exist")
    unittest.main()
