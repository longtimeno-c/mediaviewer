#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for addon-pack.py (docs/design/18 "Signed, verified, then loaded").

With MV_ADDON_VERIFY pointing at tools/addon-verify's binary (the portable
build sets it), every package is also checked by the app's own C++ verifier,
so the signer and the verifier cannot drift apart unnoticed.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("addon_pack", Path(__file__).with_name("addon-pack.py"))
addon_pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(addon_pack)


def raw_public(key) -> str:
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()


class AddonPackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.key = Ed25519PrivateKey.generate()
        seed = self.key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                      serialization.NoEncryption())
        self.key_file = self.tmp / "addon.key"
        self.key_file.write_text(seed.hex())
        self.src = self.tmp / "build"
        self.src.mkdir()
        (self.src / "mv_import.dll").write_bytes(b"MZ" + os.urandom(4096))
        (self.src / "MediaViewer.Import.Chrome.dll").write_bytes(b"MZ" + os.urandom(2048))
        (self.src / "MediaViewer.Import.Chrome.deps.json").write_text("{}")
        (self.src / "unrelated.pdb").write_bytes(b"not shipped")
        self.out = self.tmp / "dist"

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def pack(self):
        addon_pack.main(["pack", "--platform", "win-x64", "--src", str(self.src), "--version", "1.2.3",
                         "--key", str(self.key_file), "--out", str(self.out)])
        return self.out / "mediaviewer-addon-import-win-x64.json"

    def extract(self, manifest: Path) -> Path:
        m = json.loads(manifest.read_bytes())
        staged = self.tmp / "staged"
        shutil.rmtree(staged, ignore_errors=True)
        with zipfile.ZipFile(manifest.parent / m["archive"]["path"]) as z:
            z.extractall(staged)
        shutil.copy(manifest, staged / "manifest.json")
        shutil.copy(str(manifest) + ".sig", staged / "manifest.json.sig")
        return staged

    def cpp_verify(self, staged: Path) -> str:
        tool = os.environ.get("MV_ADDON_VERIFY")
        if not tool:
            self.skipTest("MV_ADDON_VERIFY not set (the portable build sets it)")
        r = subprocess.run([tool, str(staged), raw_public(self.key), "win-x64"], capture_output=True, text=True)
        return r.stdout.strip()

    def test_manifest_lists_only_shipped_files_and_verifies(self):
        manifest = self.pack()
        m = json.loads(manifest.read_bytes())
        self.assertEqual(m["schema"], 1)
        self.assertEqual(m["id"], "import")
        self.assertEqual(m["platform"], "win-x64")
        self.assertEqual(m["host_api"], {"min": 1, "max": 1})
        paths = [f["path"] for f in m["files"]]
        self.assertIn("mv_import.dll", paths)
        self.assertIn("LICENSES/NOTICE.txt", paths)
        self.assertNotIn("unrelated.pdb", paths)
        self.assertTrue(all(f["licence"] for f in m["files"]))
        pub = raw_public(self.key)
        self.assertEqual(addon_pack.main(["verify", str(manifest), "--public-key", pub]), 0)
        self.assertEqual(len((self.out / "mediaviewer-addon-import-win-x64.json.sig").read_bytes()), 64)

    def test_cpp_verifier_accepts_the_package(self):
        self.assertEqual(self.cpp_verify(self.extract(self.pack())), "ok")

    def test_cpp_verifier_refuses_a_tampered_file(self):
        staged = self.extract(self.pack())
        dll = staged / "mv_import.dll"
        data = bytearray(dll.read_bytes())
        data[100] ^= 1
        dll.write_bytes(bytes(data))
        self.assertEqual(self.cpp_verify(staged), "file_mismatch")

    def test_cpp_verifier_refuses_an_extra_file(self):
        staged = self.extract(self.pack())
        (staged / "version.dll").write_bytes(b"planted")
        self.assertEqual(self.cpp_verify(staged), "unexpected_file")

    def test_cpp_verifier_refuses_a_tampered_manifest(self):
        staged = self.extract(self.pack())
        text = (staged / "manifest.json").read_text().replace('"1.2.3"', '"9.9.9"')
        (staged / "manifest.json").write_text(text)
        self.assertEqual(self.cpp_verify(staged), "bad_signature")

    def test_tampered_archive_is_refused_before_it_is_opened(self):
        manifest = self.pack()
        archive = manifest.parent / json.loads(manifest.read_bytes())["archive"]["path"]
        with archive.open("ab") as f:
            f.write(b"x")
        self.assertEqual(addon_pack.main(["verify", str(manifest), "--public-key", raw_public(self.key)]), 1)

    def test_pinned_keys_agree(self):
        # The native add-on key is the update key (docs/design/18); two copies, one value.
        cs = (ROOT / "src.managed/MediaViewer.Updater/UpdateKeys.cs").read_text()
        hex_cs = re.search(r'ProductionPublicKeyHex\s*=\s*"([0-9a-f]{64})"', cs)[1]
        cpp = (ROOT / "src/addon/manifest.cpp").read_text()
        block = re.search(r"kPinnedKey\[32\]\s*=\s*\{(.*?)\};", cpp, re.S)[1]
        hex_cpp = "".join(f"{int(b, 16):02x}" for b in re.findall(r"0x([0-9a-fA-F]{2})", block))
        self.assertEqual(hex_cpp, hex_cs)

    def test_release_refuses_a_key_the_app_does_not_pin(self):
        with self.assertRaises(ValueError):
            addon_pack.main(["pack", "--platform", "win-x64", "--src", str(self.src), "--version", "1.2.3",
                             "--key", str(self.key_file), "--out", str(self.out), "--require-pinned-key"])
        self.assertFalse(self.out.exists() and any(self.out.iterdir()))

    def test_pinned_key_reads_the_updater_key(self):
        self.assertRegex(addon_pack.pinned_public_key_hex(), r"^[0-9a-f]{64}$")

    def test_missing_file_fails_the_pack(self):
        (self.src / "mv_import.dll").unlink()
        with self.assertRaises(FileNotFoundError):
            self.pack()


class AiPackTest(unittest.TestCase):
    """Milestone H (docs/design/17 "The AI pack"): the Core pack and its pieces."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        key = Ed25519PrivateKey.generate()
        seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                 serialization.NoEncryption())
        self.key_file = self.tmp / "addon.key"
        self.key_file.write_text(seed.hex())
        self.out = self.tmp / "dist"
        core = self.tmp / "ai"
        (core / "models/clip-b32").mkdir(parents=True)
        (core / "models/clip-tokenizer").mkdir(parents=True)
        for name in ("mv_ai.dll", "onnxruntime.dll", "onnxruntime_providers_shared.dll", "pdfium.dll",
                     "MediaViewer.Ai.Chrome.dll"):
            (core / name).write_bytes(b"MZ" + os.urandom(512))
        (core / "pdfium-licenses").mkdir()
        (core / "pdfium-licenses/pdfium.txt").write_text("BSD-3-Clause")
        (core / "MediaViewer.Ai.Chrome.deps.json").write_text("{}")
        (core / "sqlite3.dll").write_bytes(b"not shipped: the app has it")
        (core / "models/clip-b32/image.onnx").write_bytes(os.urandom(1024))
        (core / "models/clip-b32/model.json").write_text("{}")
        (core / "models/clip-tokenizer/vocab.json").write_text("{}")
        (core / "licences.json").write_text(json.dumps({
            "models/clip-b32/image.onnx": "MIT", "models/clip-b32/model.json": "GPL-3.0-or-later",
            "models/clip-tokenizer/vocab.json": "MIT"}))
        self.core = core
        faces = self.tmp / "ai-faces"
        (faces / "models/faces").mkdir(parents=True)
        (faces / "models/faces/yunet.onnx").write_bytes(os.urandom(256))
        (faces / "licences.json").write_text(json.dumps({"models/faces/yunet.onnx": "MIT"}))
        self.faces = faces

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def pack(self, addon, src, platform="win-x64"):
        addon_pack.main(["pack", "--addon", addon, "--platform", platform, "--src", str(src),
                         "--version", "1.0.0", "--key", str(self.key_file), "--out", str(self.out)])
        return json.loads((self.out / f"mediaviewer-addon-{addon}-{platform}.json").read_bytes())

    def test_core_pack_lists_runtime_models_and_licences(self):
        m = self.pack("ai", self.core)
        self.assertEqual(m["id"], "ai")
        self.assertEqual(m["host_api"], {"min": 2, "max": 2})
        self.assertEqual(m["native"], "mv_ai.dll")
        self.assertNotIn("part_of", m)
        lic = {f["path"]: f["licence"] for f in m["files"]}
        self.assertEqual(lic["onnxruntime.dll"], "MIT")
        # #354: PDFium and the notices of what is built into it.
        self.assertTrue(lic["pdfium.dll"].startswith("BSD-3-Clause AND "))
        self.assertIn("LICENSES/pdfium/pdfium.txt", lic)
        self.assertNotIn("pdfium-licenses/pdfium.txt", lic)
        self.assertEqual(lic["models/clip-b32/image.onnx"], "MIT")
        self.assertEqual(lic["mv_ai.dll"], "GPL-3.0-or-later")
        self.assertNotIn("sqlite3.dll", lic)
        self.assertNotIn("licences.json", lic)

    def test_a_piece_has_no_native_entry_and_names_its_parent(self):
        m = self.pack("ai-faces", self.faces)
        self.assertEqual(m["part_of"], "ai")
        self.assertEqual(m["native"], "")
        self.assertEqual(m["chrome"], "")
        self.assertEqual(m["name"], "AI Faces")

    def test_the_nvidia_piece_carries_only_onnx_runtime(self):
        cuda = self.tmp / "ai-cuda"
        cuda.mkdir()
        for name in ("onnxruntime.dll", "onnxruntime_providers_shared.dll", "onnxruntime_providers_cuda.dll"):
            (cuda / name).write_bytes(b"MZ" + os.urandom(512))
        (cuda / "LICENSE").write_text("MIT License")
        (cuda / "ThirdPartyNotices.txt").write_text("notices")
        # The user's CUDA runtime is never picked up, even when it sits beside the build.
        (cuda / "cudart64_13.dll").write_bytes(b"MZ")
        m = self.pack("ai-cuda", cuda)
        self.assertEqual(m["part_of"], "ai")
        self.assertEqual(m["native"], "")
        lic = {f["path"]: f["licence"] for f in m["files"]}
        self.assertEqual(lic["onnxruntime_providers_cuda.dll"], "MIT")
        self.assertEqual(lic["LICENSES/LICENSE"], "MIT")
        self.assertIn("LICENSES/ThirdPartyNotices.txt", lic)
        self.assertFalse(any(p.startswith("cud") for p in lic))

    def test_the_mac_pack_is_arm64_only(self):
        m = self.pack("ai-faces", self.faces, "macos")
        self.assertEqual(m["arch"], "arm64")

    def test_non_commercial_weights_fail_the_pack(self):
        (self.faces / "licences.json").write_text(json.dumps({"models/faces/yunet.onnx": "CC-BY-NC-4.0"}))
        with self.assertRaises(ValueError):
            self.pack("ai-faces", self.faces)

    def test_the_ai_family_has_no_ceiling(self):
        # Retired 2026-10-18 (docs/design/12): the pieces fetch their models.
        self.assertNotIn("ai", addon_pack.CEILINGS)
        self.pack("ai", self.core)
        big = self.pack("ai-faces", self.faces)
        big["installed_size"] = 50_000_000_000
        (self.out / "big.json").write_text(json.dumps(big))
        self.assertEqual(addon_pack.main(["ceiling", str(self.out / "mediaviewer-addon-ai-win-x64.json"),
                                          str(self.out / "big.json")]), 0)

    def remote_entry(self, **change):
        r = {"url": "https://huggingface.co/Xenova/clip-vit-base-patch32/resolve/" + "a" * 40 + "/onnx/text_model_fp16.onnx",
             "path": "models/clip-b32/text.onnx", "size": 1234, "sha256": "b" * 64, "licence": "MIT"}
        r.update(change)
        return r

    def test_fetched_files_are_signed_into_the_manifest_not_shipped(self):
        (self.core / "remote.json").write_text(json.dumps([self.remote_entry()]))
        m = self.pack("ai", self.core)
        self.assertEqual(m["remote"], [self.remote_entry()])
        self.assertEqual(m["download_size"], 1234)
        paths = {f["path"] for f in m["files"]}
        self.assertNotIn("remote.json", paths)
        self.assertNotIn("models/clip-b32/text.onnx", paths)

    def test_a_fetched_file_must_be_a_pinned_hugging_face_file(self):
        for bad in (self.remote_entry(url="https://example.com/x.onnx"),
                    self.remote_entry(url=self.remote_entry()["url"] + "?download=1"),
                    self.remote_entry(sha256="nope"),
                    self.remote_entry(licence="CC-BY-NC-4.0"),
                    self.remote_entry(path="../escape.onnx"),
                    self.remote_entry(path="models/clip-b32/image.onnx")):  # also shipped
            with self.subTest(bad=bad):
                (self.core / "remote.json").write_text(json.dumps([bad]))
                with self.assertRaises(ValueError):
                    self.pack("ai", self.core)

    def test_remote_staging_lists_upstream_files_and_keeps_exports(self):
        gate = importlib.util.spec_from_file_location("ai_models", Path(__file__).with_name("ai-models.py"))
        mod = importlib.util.module_from_spec(gate)
        gate.loader.exec_module(mod)
        out = self.tmp / "staged-ai"
        self.assertEqual(mod.main(["stage", "--piece", "ai", "--out", str(out), "--upstream", "remote",
                                   "--skip-exports"]), 0)
        remote = json.loads((out / "remote.json").read_text())
        self.assertTrue(remote)
        self.assertTrue(all(r["url"].startswith("https://huggingface.co/") for r in remote))
        self.assertFalse(any((out / r["path"]).exists() for r in remote))
        self.assertTrue((out / "models/clip-l14/model.json").exists())
        tiers = json.loads((out / "models/tiers.json").read_text())
        default = tiers["profiles"][tiers["default"]]
        self.assertIn("models/clip-tokenizer", default["folders"]["ai"])
        self.assertIn("models/whisper-tokenizer", default["folders"]["ai-audio"])
        # The picker's summary: names by role, bytes fetched per piece, licences.
        self.assertIn("CLIP ViT-L/14", default["summary"]["models"]["pictures"])
        self.assertGreater(default["summary"]["download_bytes"]["ai"], 0)
        self.assertTrue(set(default["summary"]["licences"]) <= mod.ALLOWED_LICENCES)
        # Potato has no sound search, and the cheapest towers that pass.
        potato = tiers["profiles"]["potato"]
        self.assertNotIn("sounds", potato["summary"]["models"])
        self.assertIn("models/whisper-tiny", potato["folders"]["ai-audio"])
        self.assertIn("models/faces-sface", potato["folders"]["ai-faces"])

    def test_every_piece_staged_remote_meets_the_apps_rules(self):
        # read_remote and collect apply the app's own path and URL rules
        # (src/addon/manifest.cpp): every piece, staged as the release stages it, packs.
        mod = load_tool("ai_models", "ai-models.py")
        data = mod.load()
        for piece, spec in data["pieces"].items():
            with self.subTest(piece=piece):
                out = self.tmp / f"staged-{piece}"
                with mock.patch("urllib.request.urlopen", side_effect=no_network), \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(mod.main(["stage", "--piece", piece, "--out", str(out), "--upstream", "remote",
                                               "--skip-exports"]), 0)
                upstream = [e for f in spec["folders"].values() for e in f["files"] if "export" not in e]
                remote = addon_pack.read_remote(out)
                self.assertEqual(len(remote), len(upstream))
                if piece == "ai":  # the Core's code, beside its models
                    for name in addon_pack.ADDONS["ai"]["files"]["win-x64"]["include"]:
                        if name != "models":
                            shutil.copy2(self.core / name, out / name)
                with contextlib.redirect_stdout(io.StringIO()):
                    m = self.pack(piece, out)
                self.assertEqual(m.get("remote", []), remote)

    def test_model_list_passes_the_licence_gate(self):
        gate = importlib.util.spec_from_file_location("ai_models", Path(__file__).with_name("ai-models.py"))
        mod = importlib.util.module_from_spec(gate)
        gate.loader.exec_module(mod)
        self.assertEqual(mod.main(["check"]), 0)

    def test_a_tier_must_name_a_model_of_each_role_in_its_piece(self):
        gate = importlib.util.spec_from_file_location("ai_models", Path(__file__).with_name("ai-models.py"))
        mod = importlib.util.module_from_spec(gate)
        gate.loader.exec_module(mod)
        data = mod.load()
        self.assertEqual(mod.tier_problems(data), [])
        prof = data["tiers"]["profiles"][data["tiers"]["default"]]

        def broken(change):
            d = json.loads(json.dumps(data))
            change(d["tiers"]["profiles"][d["tiers"]["default"]], d)
            return mod.tier_problems(d)

        # A picture tower from the audio piece, a missing role, a tokenizer
        # folder (no model.json), an unknown slot, an unknown default.
        self.assertTrue(broken(lambda p, d: p["pictures"].update(high=prof["sounds"])))
        self.assertTrue(broken(lambda p, d: p.pop("documents")))
        self.assertTrue(broken(lambda p, d: p.update(faces="models/clip-tokenizer")))
        self.assertTrue(broken(lambda p, d: p["speech"].update(huge="models/whisper-small")))
        self.assertTrue(broken(lambda p, d: d["tiers"].update(default="nope")))
        # A tier may leave one slot empty (Intel has no high tower), not both.
        self.assertEqual(broken(lambda p, d: p["pictures"].update(high=None)), [])
        self.assertTrue(broken(lambda p, d: p.update(pictures={"high": None, "fast": None})))


class DriftwirePackTest(unittest.TestCase):
    """docs/design/28: Driftwire is code only; the hosts render its window."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        key = Ed25519PrivateKey.generate()
        seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                 serialization.NoEncryption())
        self.key_file = self.tmp / "addon.key"
        self.key_file.write_text(seed.hex())
        self.out = self.tmp / "dist"
        self.src = self.tmp / "driftwire"
        self.src.mkdir()
        (self.src / "mv_driftwire.dll").write_bytes(b"MZ" + os.urandom(1024))
        (self.src / "libmv_driftwire.dylib").write_bytes(b"\xcf\xfa\xed\xfe" + os.urandom(1024))
        (self.src / "mv_driftwire.pdb").write_bytes(b"not shipped")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_the_package_carries_the_code_one_window_and_a_keyless_command(self):
        for platform, native in (("win-x64", "mv_driftwire.dll"), ("macos", "libmv_driftwire.dylib")):
            addon_pack.main(["pack", "--addon", "driftwire", "--platform", platform, "--src", str(self.src),
                             "--version", "1.0.0", "--key", str(self.key_file), "--out", str(self.out)])
            m = json.loads((self.out / f"mediaviewer-addon-driftwire-{platform}.json").read_bytes())
            self.assertEqual(m["id"], "driftwire")
            self.assertEqual(m["name"], "Driftwire")
            self.assertEqual(m["host_api"], {"min": 3, "max": 3})
            self.assertEqual(m["native"], native)
            self.assertEqual(m["chrome"], "")
            lic = {f["path"]: f["licence"] for f in m["files"]}
            self.assertEqual(lic[native], "GPL-3.0-or-later")
            self.assertNotIn("mv_driftwire.pdb", lic)
            c = m["contributes"]
            self.assertEqual(sorted(c), ["api", "commands", "context_menu", "screens", "status_items"])
            self.assertEqual(c["screens"][0]["place"], "window")
            self.assertEqual(c["context_menu"][0]["id"], "send_files")
            # Issue #410: no new default key until the command table has one.
            self.assertNotIn("windows", c["commands"][0])
            self.assertNotIn("mac", c["commands"][0])
        with zipfile.ZipFile(self.out / "mediaviewer-addon-driftwire-macos.zip") as z:
            notice = z.read("LICENSES/NOTICE.txt").decode()
        self.assertIn("libsodium", notice)
        self.assertIn("No account, no cloud", notice)


class PlacesPackTest(unittest.TestCase):
    """docs/design/27: Places carries its own code and its own data."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        key = Ed25519PrivateKey.generate()
        self.key = key
        seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                 serialization.NoEncryption())
        self.key_file = self.tmp / "addon.key"
        self.key_file.write_text(seed.hex())
        self.out = self.tmp / "dist"
        self.src = self.tmp / "places"
        self.src.mkdir()
        (self.src / "mv_places.dll").write_bytes(b"MZ" + os.urandom(1024))
        (self.src / "libmv_places.dylib").write_bytes(b"\xcf\xfa\xed\xfe" + os.urandom(1024))
        (self.src / "mv_places.pdb").write_bytes(b"not shipped")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def stage(self):
        addon_pack.main(["stage-places", "--src", str(self.src),
                         "--blob", str(ROOT / "tests/data/geo/places-test.mvgeo")])

    def pack(self, platform="win-x64"):
        addon_pack.main(["pack", "--addon", "places", "--platform", platform, "--src", str(self.src),
                         "--version", "1.0.0", "--key", str(self.key_file), "--out", str(self.out)])
        return self.out / f"mediaviewer-addon-places-{platform}.json"

    def test_the_package_carries_the_code_and_the_place_database(self):
        self.stage()
        for platform, native in (("win-x64", "mv_places.dll"), ("macos", "libmv_places.dylib")):
            m = json.loads(self.pack(platform).read_bytes())
            self.assertEqual(m["id"], "places")
            self.assertEqual(m["host_api"], {"min": 3, "max": 3})
            self.assertEqual(m["native"], native)
            self.assertEqual(m["chrome"], "")  # the hosts render its tree
            lic = {f["path"]: f["licence"] for f in m["files"]}
            self.assertEqual(lic[native], "GPL-3.0-or-later")
            self.assertEqual(lic["data/places.mvgeo"], "CC-BY-4.0 AND LicenseRef-PublicDomain")
            self.assertIn("LICENSES/NOTICE.txt", lic)
            self.assertNotIn("mv_places.pdb", lic)
            # Owner, 2026-10-12: the Place section, search and settings; no bar item, no window.
            self.assertEqual(sorted(m["contributes"]), ["api", "info_sections", "search_providers", "settings"])
            self.assertEqual(m["contributes"]["info_sections"][0]["id"], "place")
            self.assertEqual(m["contributes"]["search_providers"][0]["prefixes"], ["place:", "in:"])
            keys = [i["key"] for s in m["contributes"]["settings"]["sections"] for i in s["items"]]
            self.assertEqual(keys, ["show_map", "show_town", "town_radius_km", "label_names"])
            self.assertIn("Offline", m["description"])
        with zipfile.ZipFile(self.out / "mediaviewer-addon-places-win-x64.zip") as z:
            notice = z.read("LICENSES/NOTICE.txt").decode()
        self.assertIn("GeoNames", notice)
        self.assertIn("CC-BY 4.0", notice)
        self.assertIn("Natural Earth", notice)

    def test_no_place_database_no_package(self):
        with self.assertRaises(FileNotFoundError):
            self.pack()

    def test_staging_refuses_a_file_that_is_not_a_place_database(self):
        bogus = self.tmp / "bogus.mvgeo"
        bogus.write_bytes(b"not a database")
        with self.assertRaises(ValueError):
            addon_pack.main(["stage-places", "--src", str(self.src), "--blob", str(bogus)])

    def test_data_licences_join_the_allowed_set_but_nothing_else_does(self):
        self.assertTrue(addon_pack.licence_allowed("CC-BY-4.0"))
        self.assertTrue(addon_pack.licence_allowed("CC-BY-4.0 AND LicenseRef-PublicDomain"))
        self.assertFalse(addon_pack.licence_allowed("CC-BY-NC-4.0"))
        self.assertFalse(addon_pack.licence_allowed("CC-BY-4.0 AND CC-BY-NC-4.0"))
        self.assertFalse(addon_pack.licence_allowed("CC-BY-SA-4.0"))

    def test_cpp_verifier_accepts_the_package(self):
        tool = os.environ.get("MV_ADDON_VERIFY")
        if not tool:
            self.skipTest("MV_ADDON_VERIFY not set (the portable build sets it)")
        self.stage()
        manifest = self.pack()
        m = json.loads(manifest.read_bytes())
        staged = self.tmp / "staged"
        with zipfile.ZipFile(manifest.parent / m["archive"]["path"]) as z:
            z.extractall(staged)
        shutil.copy(manifest, staged / "manifest.json")
        shutil.copy(str(manifest) + ".sig", staged / "manifest.json.sig")
        r = subprocess.run([tool, str(staged), raw_public(self.key), "win-x64"], capture_output=True, text=True)
        verdict = r.stdout.strip().splitlines()[0] if r.stdout.strip() else r.stdout
        # The settings page is PR 56's contribution point: a host without it
        # (no src/addon/settings_values.h) refuses the page, and so the add-on.
        if (ROOT / "src/addon/settings_values.h").exists():
            self.assertEqual(verdict, "ok")
        else:
            self.assertEqual(verdict, "malformed")
            # Everything else in the package is what this host takes: the same
            # package without the page verifies (host table 3, the data licences).
            doc = json.loads((ROOT / addon_pack.ADDONS["places"]["manifest_json"]).read_text(encoding="utf-8"))
            del doc["contributes"]["settings"]
            bare = self.tmp / "manifest-no-settings.json"
            bare.write_text(json.dumps(doc), encoding="utf-8")
            saved = addon_pack.ADDONS["places"]["manifest_json"]
            addon_pack.ADDONS["places"]["manifest_json"] = str(bare)  # absolute: ROOT / it is it
            try:
                shutil.rmtree(self.out, ignore_errors=True)
                manifest = self.pack()
            finally:
                addon_pack.ADDONS["places"]["manifest_json"] = saved
            shutil.copy(manifest, staged / "manifest.json")
            shutil.copy(str(manifest) + ".sig", staged / "manifest.json.sig")
            r = subprocess.run([tool, str(staged), raw_public(self.key), "win-x64"], capture_output=True, text=True)
            self.assertEqual(r.stdout.strip().splitlines()[0] if r.stdout.strip() else r.stdout, "ok")


def load_tool(name: str, file: str):
    gate = importlib.util.spec_from_file_location(name, Path(__file__).with_name(file))
    mod = importlib.util.module_from_spec(gate)
    gate.loader.exec_module(mod)
    return mod


def no_network(*_args, **_kwargs):
    raise AssertionError("no request may be made")


def renamed(data: dict, old: str, new: str) -> None:
    """The catalogue with model `old` called `new` everywhere."""
    entry = data["models"].pop(f"models/{old}")
    entry["model"]["id"] = new
    data["models"][f"models/{new}"] = entry
    for spec in data["kinds"].values():
        spec["models"] = [new if i == old else i for i in spec["models"]]


class EnhancePackTest(unittest.TestCase):
    """docs/plans/enhance.md E1: the catalogue's gate, its staging and the pack.
    Nothing here touches the network: the weights are `remote`."""

    FIXTURE = ROOT / "tests/data/enhance/fixture-models.json"

    @classmethod
    def setUpClass(cls):
        cls.models = load_tool("enhance_models", "enhance-models.py")

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.key = Ed25519PrivateKey.generate()
        seed = self.key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                                      serialization.NoEncryption())
        self.key_file = self.tmp / "addon.key"
        self.key_file.write_text(seed.hex())
        self.out = self.tmp / "dist"
        self.src = self.tmp / "enhance"
        self.src.mkdir()
        (self.src / "mv_enhance.dll").write_bytes(b"MZ" + os.urandom(1024))
        (self.src / "libmv_enhance.dylib").write_bytes(b"\xcf\xfa\xed\xfe" + os.urandom(1024))
        (self.src / "mv_enhance.pdb").write_bytes(b"not shipped")
        offline = mock.patch("urllib.request.urlopen", side_effect=no_network)
        offline.start()
        self.addCleanup(offline.stop)
        fetch = mock.patch.object(self.models.ai_models, "fetch", side_effect=no_network)
        fetch.start()
        self.addCleanup(fetch.stop)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_models(self, *argv):
        """enhance-models.py in process: (exit code, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = self.models.main(list(argv))
        return code, out.getvalue(), err.getvalue()

    def fixture(self) -> dict:
        return json.loads(self.FIXTURE.read_text(encoding="utf-8"))

    def stage(self, *extra):
        code, _, err = self.run_models("stage", "--models", str(self.FIXTURE), "--out", str(self.src), *extra)
        self.assertEqual(code, 0, err)

    def pack(self, platform="win-x64"):
        addon_pack.main(["pack", "--addon", "enhance", "--platform", platform, "--src", str(self.src),
                         "--version", "1.0.0", "--key", str(self.key_file), "--out", str(self.out)])
        return self.out / f"mediaviewer-addon-enhance-{platform}.json"

    def test_the_real_catalogue_passes_the_gate(self):
        code, out, err = self.run_models("check")
        self.assertEqual(code, 0, err)
        self.assertRegex(out, r"^ok: \d+ models")
        data = json.loads(self.models.MODELS.read_text(encoding="utf-8"))
        self.assertEqual(sorted(data["kinds"]), ["audio", "still", "video"])
        # As CI runs it: the command line, isolated.
        r = subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("enhance-models.py")), "check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_the_fixture_passes_the_gate(self):
        self.assertEqual(self.run_models("check", "--models", str(self.FIXTURE))[:2], (0, "ok: 3 models\n"))
        # Its pins are Local search's, already verified there (ai-models.json).
        ai = json.loads((ROOT / "tools/package/ai-models.json").read_text(encoding="utf-8"))
        pinned = {(f["repo"], f["revision"], f["file"], f["size"], f["sha256"])
                  for piece in ai["pieces"].values() for folder in piece["folders"].values() for f in folder["files"]}
        for folder in self.fixture()["models"].values():
            for f in folder["files"]:
                self.assertIn((f["repo"], f["revision"], f["file"], f["size"], f["sha256"]), pinned)

    def refusal(self, change) -> str:
        """check's complaint about the fixture after `change(data)`, or '' if it passes."""
        data = self.fixture()
        change(data)
        path = self.tmp / "broken.json"
        path.write_text(json.dumps(data), encoding="utf-8")
        code, _, err = self.run_models("check", "--models", str(path))
        self.assertEqual(code != 0, bool(err))
        return err

    def test_the_gate_refuses_what_the_rule_cannot_take(self):
        m = "models/fixture-standard"
        cases = {
            "is CC-BY-NC-4.0": lambda d: d["models"][m]["files"][0].update(licence="CC-BY-NC-4.0"),
            "over the 5 GB per-file cap": lambda d: d["models"][m]["files"][0].update(size=5_000_000_001),
            "is not pinned": lambda d: d["models"][m]["files"][0].update(revision="a" * 39),
            "unknown kind": lambda d: d["kinds"].update(animation=d["kinds"]["video"]),
            "kinds.video: missing": lambda d: d["kinds"].pop("video"),
            "fixture-standard does not serve audio": lambda d: d["kinds"]["audio"]["models"].append("fixture-standard"),
            "'photo-x' is not a model": lambda d: d["kinds"]["still"]["models"].append("photo-x"),
            "kinds.audio does not list it": lambda d: d["kinds"]["audio"].update(models=[]),
            "outside 0..1": lambda d: d["models"][m]["model"]["knobs"][1].update(default=1.5),
            "not one of its options": lambda d: d["models"][m]["model"]["knobs"][0].update(default="8"),
            "some of the model's own": lambda d: d["models"][m]["model"]["knobs"][0].update(kinds=["audio"]),
            "toggle's default": lambda d: d["models"]["models/fixture-small"]["model"]["knobs"][1].update(default=1),
            "not a safe relative path": lambda d: d["models"][m]["files"][0].update(**{"as": "../model.onnx"}),
            "a model folder is models/<id>": lambda d: d["models"].update({"photo": d["models"].pop(m)}),
            "runs nowhere": lambda d: d["models"][m]["model"].update(accelerators=[]) or
            d["models"][m]["model"].pop("cpu_unit_ms"),
            "not a plain Hugging Face URL": lambda d: d["models"][m]["files"][0].update(file="x.onnx?download=1"),
        }
        for want, change in cases.items():
            with self.subTest(want=want):
                self.assertIn(want, self.refusal(change))
        # The app's own rules (src/addon/manifest.cpp): one name it refuses
        # refuses the whole signed add-on, so the gate refuses it first.
        for name in ("aux.onnx", "model.", "a|b.onnx", "COM1", "x/nul.txt", "lpt1.onnx"):
            with self.subTest(**{"as": name}):
                self.assertIn("not a path the app takes",
                              self.refusal(lambda d, n=name: d["models"][m]["files"][0].update(**{"as": n})))
        for mid in ("con", "aux", "lpt1"):
            with self.subTest(id=mid):
                self.assertIn(f"{mid} is a name the app refuses",
                              self.refusal(lambda d, i=mid: renamed(d, "fixture-standard", i)))
        self.assertIn("not a plain Hugging Face URL",
                      self.refusal(lambda d: d["models"][m]["files"][0].update(file="onnx/a@b.onnx")))
        self.assertEqual(self.refusal(lambda d: renamed(d, "fixture-standard", "aux1")), "")
        self.assertEqual(self.refusal(lambda d: None), "")
        # json would keep the last of two same-named models; the gate does not.
        text = self.FIXTURE.read_text(encoding="utf-8")
        dup = json.dumps({m: self.fixture()["models"][m]})[1:-1]
        twice = self.tmp / "twice.json"
        twice.write_text(text.replace('"models": {\n', '"models": {\n' + dup + ",\n", 1), encoding="utf-8")
        code, _, err = self.run_models("check", "--models", str(twice))
        self.assertEqual(code, 1)
        self.assertIn(f"{m}: duplicate model id", err)

    def test_remote_staging_writes_the_catalogue_and_fetches_nothing(self):
        # A copy from an earlier bundle and a model since removed are not packed.
        (self.src / "models/fixture-small").mkdir(parents=True)
        (self.src / "models/fixture-small/model.onnx").write_bytes(b"stale")
        (self.src / "models/gone").mkdir()
        (self.src / "models/gone/model.json").write_text("{}")
        self.stage()  # --upstream remote is the default
        data = self.fixture()
        index = json.loads((self.src / "models/enhance.json").read_text())
        self.assertEqual(index["schema"], 1)
        self.assertEqual(index["kinds"]["still"]["models"], ["fixture-large", "fixture-standard", "fixture-small"])
        self.assertEqual(index["kinds"]["audio"], {"name": "Audio", "unit": "a minute of sound",
                                                   "cpu_budget_ms": 20000, "models": ["fixture-small"]})
        for folder, entry in data["models"].items():
            text = (self.src / folder / "model.json").read_text(encoding="utf-8")
            self.assertEqual(json.loads(text), entry["model"])
            self.assertEqual(list(json.loads(text)), list(entry["model"]))  # as written, in order
        self.assertFalse((self.src / "models/fixture-small/model.onnx").exists())
        self.assertFalse((self.src / "models/gone").exists())
        remote = json.loads((self.src / "remote.json").read_text())
        self.assertEqual(len(remote), 3)
        for r in remote:
            self.assertEqual(set(r), {"url", "path", "size", "sha256", "licence"})
            self.assertRegex(r["url"], r"^https://huggingface\.co/[^?#]+/resolve/[0-9a-f]{40}/[^?#]+$")
            self.assertRegex(r["path"], r"^models/fixture-[a-z]+/model\.onnx$")
        self.assertEqual(sum(r["size"] for r in remote), 1274342603 + 38696353 + 232589)
        lic = json.loads((self.src / "licences.json").read_text())
        self.assertEqual(lic, {p: "GPL-3.0-or-later" for p in
                               ["models/enhance.json"] + [f"{f}/model.json" for f in data["models"]]})
        # The real catalogue stages no model and lists nothing to fetch.
        code, _, err = self.run_models("stage", "--out", str(self.src))
        self.assertEqual(code, 0, err)
        self.assertFalse((self.src / "remote.json").exists())
        self.assertEqual(sorted(p.name for p in (self.src / "models").iterdir()), ["enhance.json"])

    def test_bundle_staging_is_refused(self):
        # The add-on reads weights only from data/cache/models, never from its
        # pack: a bundled pack would offer no model at all.
        code, _, err = self.run_models("stage", "--models", str(self.FIXTURE), "--out", str(self.src),
                                       "--upstream", "bundle")
        self.assertEqual(code, 2)
        self.assertIn("data/cache/models/<id>/<as>", err)
        self.assertFalse((self.src / "models").exists())
        self.assertFalse((self.src / "remote.json").exists())

    STAGED = ROOT / "tests/data/enhance/staged"
    RESTAGE = ("python3 -I tools/package/enhance-models.py stage --models tests/data/enhance/fixture-models.json "
               "--out tests/data/enhance/staged")

    def test_the_committed_stage_is_what_stage_writes(self):
        # tests/test_enhance*.cpp read tests/data/enhance/staged as the add-on
        # reads its pack, so this script and the C++ reader cannot drift apart.
        out = self.tmp / "staged"
        code, _, err = self.run_models("stage", "--models", str(self.FIXTURE), "--out", str(out))
        self.assertEqual(code, 0, err)

        def tree(root):
            return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*") if p.is_file()}

        committed, fresh = tree(self.STAGED), tree(out)
        self.assertEqual(sorted(committed), sorted(fresh), f"regenerate it: {self.RESTAGE}")
        for rel, data in fresh.items():
            self.assertEqual(committed[rel], data, f"{rel} differs; regenerate it: {self.RESTAGE}")

    def test_the_add_on_reads_the_same_licence_allow_list(self):
        # One allow-list for both add-ons: catalog.cpp's copy drops a model
        # whose licence it lacks, silently, so the two must be equal.
        cpp = (ROOT / "src/addons/enhance/catalog.cpp").read_text(encoding="utf-8")
        block = re.search(r"\bkLicences\b[^{;]*\{(.*?)\}\s*;", cpp, re.S)
        self.assertIsNotNone(block, "kLicences not found in src/addons/enhance/catalog.cpp")
        listed = re.findall(r'"([^"\\]*)"', block[1])
        self.assertEqual(len(listed), len(set(listed)))
        self.assertEqual(set(listed), self.models.ai_models.ALLOWED_LICENCES)
        # A std::array larger than its list holds empty names an empty licence would match.
        size = re.search(r"std::array<std::string_view,\s*(\d+)>\s*kLicences\b", cpp)
        if size:
            self.assertEqual(int(size[1]), len(listed))

    def test_staging_refuses_a_catalogue_the_gate_refuses(self):
        data = self.fixture()
        data["models"]["models/fixture-small"]["files"][0]["licence"] = "CC-BY-NC-4.0"
        bad = self.tmp / "bad.json"
        bad.write_text(json.dumps(data), encoding="utf-8")
        code, _, err = self.run_models("stage", "--models", str(bad), "--out", str(self.src))
        self.assertEqual(code, 1)
        self.assertIn("CC-BY-NC-4.0", err)
        self.assertFalse((self.src / "models").exists())

    def test_the_package_carries_code_and_catalogue_and_signs_the_pins(self):
        self.stage()
        remote = json.loads((self.src / "remote.json").read_text())
        doc = json.loads((ROOT / "src/addons/enhance/manifest.json").read_text(encoding="utf-8"))
        for platform, native in (("win-x64", "mv_enhance.dll"), ("macos", "libmv_enhance.dylib")):
            m = json.loads(self.pack(platform).read_bytes())
            self.assertEqual(m["id"], "enhance")
            self.assertEqual(m["name"], "Enhance")
            self.assertEqual(m["host_api"], {"min": 3, "max": 3})
            self.assertEqual(m["native"], native)
            self.assertEqual(m["chrome"], "")  # the hosts render its Models page
            self.assertNotIn("part_of", m)
            self.assertEqual(m["remote"], remote)
            self.assertEqual(m["download_size"], sum(r["size"] for r in remote))
            lic = {f["path"]: f["licence"] for f in m["files"]}
            self.assertEqual(lic[native], "GPL-3.0-or-later")
            self.assertEqual(lic["models/enhance.json"], "GPL-3.0-or-later")
            self.assertEqual(lic["models/fixture-large/model.json"], "GPL-3.0-or-later")
            self.assertIn("LICENSES/NOTICE.txt", lic)
            for gone in ["mv_enhance.pdb", "remote.json", "licences.json"] + [r["path"] for r in remote]:
                self.assertNotIn(gone, lic)
            self.assertEqual(m["installed_size"], sum(f["size"] for f in m["files"]))
            self.assertEqual(m["description"], doc["description"])
            self.assertEqual(m["contributes"], doc["contributes"])
            self.assertEqual(m["contributes"]["screens"], [{"id": "models", "title": "Enhance", "place": "settings"}])
            self.assertEqual(m.get("arch"), "arm64" if platform == "macos" else None)
        with zipfile.ZipFile(self.out / "mediaviewer-addon-enhance-win-x64.zip") as z:
            notice = z.read("LICENSES/NOTICE.txt").decode()
            self.assertFalse(any(n.endswith(".onnx") for n in z.namelist()))
        self.assertIn("Hugging Face", notice)

    def test_the_pack_refuses_a_pin_over_the_cap_before_writing(self):
        self.stage()
        remote = json.loads((self.src / "remote.json").read_text())
        remote[0]["size"] = 5_000_000_000  # at the cap: taken
        (self.src / "remote.json").write_text(json.dumps(remote))
        self.pack()
        shutil.rmtree(self.out)
        remote[0]["size"] = 5_000_000_001
        (self.src / "remote.json").write_text(json.dumps(remote))
        with self.assertRaisesRegex(ValueError, "over the 5 GB per-file cap"):
            self.pack()
        self.assertFalse(self.out.exists())
        # Enhance's alone: Local search has none since 2026-10-18.
        self.assertEqual([a for a, s in addon_pack.ADDONS.items() if s.get("remote_file_cap")], ["enhance"])

    def test_the_pack_refuses_what_the_app_refuses(self):
        self.stage()
        text = (self.src / "remote.json").read_text()
        remote = json.loads(text)
        url = remote[0]["url"]
        for bad in ({"path": "models/fixture-small/aux.onnx"}, {"path": "models/fixture-small/model."},
                    {"path": "models/fixture-small/a|b.onnx"}, {"path": "models/con/model.onnx"},
                    {"url": url[:url.rindex("/")] + "/a@b.onnx"}):
            with self.subTest(bad=bad):
                (self.src / "remote.json").write_text(json.dumps([dict(remote[0], **bad)] + remote[1:]))
                with self.assertRaises(ValueError):
                    self.pack()
                self.assertFalse(self.out.exists())
        (self.src / "remote.json").write_text(text)
        (self.src / "models/con").mkdir()
        (self.src / "models/con/model.json").write_text("{}")
        with self.assertRaisesRegex(ValueError, "models/con/model.json: not a path the app takes"):
            self.pack()

    # (path under models/fixture-small/, whether the app takes it): the cases
    # src/addon/manifest.cpp safe_relative_path decides, either way.
    PATHS = [("model.onnx", True), ("aux.onnx", False), ("AUX", False), ("nul .txt", False), ("com1.onnx", False),
             ("LPT9", False), ("com0", False), ("com\u00b9.onnx", False), ("lpt\u00b3", False), ("model.", False),
             ("model ", False), ("a|b", False), ("a<b", False), ("a>b", False), ('a"b', False), ("a?b", False),
             ("a*b", False), ("a:b", False), ("a\\b", False), ("a\x01b", False), ("a//b", False), ("a/../b", False),
             ("com10.onnx", True), ("aux1.onnx", True), ("conx", True), (" con", True), ("lpt\u00b4", True),
             ("com\u2071", True), (".hidden", True), ("\u00e9t\u00e9.onnx", True), ("a b.onnx", True),
             ("a..b", True), ("x" * 491, True), ("x" * 492, False)]
    URL = "https://huggingface.co/opencv/face_detection_yunet/resolve/" + "a" * 40 + "/m.onnx"
    URLS = [(URL, True), (URL.replace("https:", "http:"), False), (URL + "?download=1", False), (URL + "#x", False),
            (URL[:-6] + "a@b.onnx", False), (URL.replace("/resolve/", "/blob/"), False),
            (URL.replace("/resolve/", "/resolve/../"), False), (URL[:-6] + "\u00e9.onnx", False),
            (URL[:-6] + "a\\b.onnx", False), (URL[:-6] + "a b.onnx", False), (URL[:-6] + "a\x7fb.onnx", False),
            ("https://huggingface.co/", False), ("https://huggingface.co.evil/x/resolve/y", False),
            (URL[:-6] + "x" * (1024 - len(URL) + 6), True), (URL[:-6] + "x" * (1025 - len(URL) + 6), False)]

    def test_the_path_and_url_rules_are_the_apps(self):
        for path, taken in self.PATHS:
            self.assertEqual(addon_pack.safe_relative_path("models/fixture-small/" + path), taken, path)
        for url, taken in self.URLS:
            self.assertEqual(addon_pack.remote_url_ok(url), taken, url)
        # And the app agrees: each case signed unchecked, then judged by its verifier.
        self.stage()
        base = json.loads((self.src / "remote.json").read_text())[0]
        cases = ([({"path": "models/fixture-small/" + p}, "ok" if t else "unsafe_path") for p, t in self.PATHS] +
                 [({"url": u}, "ok" if t else "malformed") for u, t in self.URLS])
        for i, (change, want) in enumerate(cases):
            with self.subTest(change=change):
                with mock.patch.object(addon_pack, "read_remote", return_value=[dict(base, **change)]):
                    manifest = self.pack()
                m = json.loads(manifest.read_bytes())
                staged = self.tmp / f"case-{i}"
                with zipfile.ZipFile(manifest.parent / m["archive"]["path"]) as z:
                    z.extractall(staged)
                shutil.copy(manifest, staged / "manifest.json")
                shutil.copy(str(manifest) + ".sig", staged / "manifest.json.sig")
                self.assertEqual(self.verdict(staged), want)

    def verdict(self, folder: Path) -> str:
        tool = os.environ.get("MV_ADDON_VERIFY")
        if not tool:
            self.skipTest("MV_ADDON_VERIFY not set (the portable build sets it)")
        r = subprocess.run([tool, str(folder), raw_public(self.key), "win-x64"], capture_output=True, text=True)
        return r.stdout.strip().splitlines()[0] if r.stdout.strip() else r.stdout

    def test_cpp_verifier_accepts_the_package(self):
        self.stage()
        manifest = self.pack()
        m = json.loads(manifest.read_bytes())
        staged = self.tmp / "staged"
        with zipfile.ZipFile(manifest.parent / m["archive"]["path"]) as z:
            z.extractall(staged)
        shutil.copy(manifest, staged / "manifest.json")
        shutil.copy(str(manifest) + ".sig", staged / "manifest.json.sig")
        self.assertEqual(self.verdict(staged), "ok")

    def test_the_dev_sideload_unpacks_the_fixture_build(self):
        sideload = load_tool("enhance_sideload", "enhance-sideload.py")
        build = self.tmp / "build"
        (build / "addons").mkdir(parents=True)
        shutil.copytree(self.src, build / "addons/enhance")
        addons = self.tmp / "addons"
        with mock.patch.object(sideload.enhance_models.ai_models, "fetch", side_effect=no_network), \
                contextlib.redirect_stdout(io.StringIO()):
            code = sideload.main(["--build", str(build), "--key", str(self.key_file), "--addons", str(addons),
                                  "--platform", "win-x64"])
        self.assertEqual(code, 0)
        target = addons / "enhance/1.0.0"
        m = json.loads((target / "manifest.json").read_bytes())
        self.assertEqual(len(m["remote"]), 3)
        self.assertTrue((target / "mv_enhance.dll").is_file())
        self.assertTrue((target / "models/enhance.json").is_file())
        self.assertFalse((target / "remote.json").exists())
        self.assertEqual(sorted(p.name for p in addons.parent.iterdir() if p.name.startswith(".mv-")), [])
        if os.environ.get("MV_ADDON_VERIFY"):  # as the app verifies it on load
            self.assertEqual(self.verdict(target), "ok")


if __name__ == "__main__":
    unittest.main()
