#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for addon-pack.py (plan/18 "Signed, verified, then loaded").

With MV_ADDON_VERIFY pointing at tools/addon-verify's binary (the portable
build sets it), every package is also checked by the app's own C++ verifier,
so the signer and the verifier cannot drift apart unnoticed.
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
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
        # The native add-on key is the update key (plan/18); two copies, one value.
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
    """Milestone H (plan/17 "The AI pack"): the Core pack and its pieces."""

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
        for name in ("mv_ai.dll", "onnxruntime.dll", "onnxruntime_providers_shared.dll",
                     "MediaViewer.Ai.Chrome.dll"):
            (core / name).write_bytes(b"MZ" + os.urandom(512))
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

    def test_the_family_ceiling(self):
        core = self.pack("ai", self.core)
        faces = self.pack("ai-faces", self.faces)
        self.assertEqual(addon_pack.main(["ceiling", str(self.out / "mediaviewer-addon-ai-win-x64.json"),
                                          str(self.out / "mediaviewer-addon-ai-faces-win-x64.json")]), 0)
        big = dict(faces)
        big["id"] = "ai-cuda"
        big["installed_size"] = 3_000_000_000 - core["installed_size"] + 1
        big["files"] = [{"path": "onnxruntime_providers_cuda.dll", "sha256": "0" * 64, "size": 1, "licence": "MIT"}]
        (self.out / "big.json").write_text(json.dumps(big))
        self.assertEqual(addon_pack.main(["ceiling", str(self.out / "mediaviewer-addon-ai-win-x64.json"),
                                          str(self.out / "big.json")]), 1)

    def test_model_list_passes_the_licence_gate(self):
        gate = importlib.util.spec_from_file_location("ai_models", Path(__file__).with_name("ai-models.py"))
        mod = importlib.util.module_from_spec(gate)
        gate.loader.exec_module(mod)
        self.assertEqual(mod.main(["check"]), 0)


if __name__ == "__main__":
    unittest.main()
