#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for macpack.py's pure helpers (no macOS needed; they run
on Linux). The otool samples are the formats Apple's cctools print."""
from __future__ import annotations

import sys
import argparse
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import macpack  # noqa: E402
from unittest.mock import patch
import posixpath

OTOOL_L_EXE = """/build/MediaViewer:
\t@rpath/libheif.1.dylib (compatibility version 1.0.0, current version 1.19.5)
\t@rpath/libavcodec.61.dylib (compatibility version 61.0.0, current version 61.19.100)
\t@rpath/Sparkle.framework/Versions/B/Sparkle (compatibility version 1.6.0, current version 2.9.6)
\t/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit (compatibility version 45.0.0, current version 2487.0.0)
\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0, current version 1700.255.0)
"""

OTOOL_L_DYLIB = """/vcpkg/lib/libheif.1.19.5.dylib:
\t@rpath/libheif.1.dylib (compatibility version 1.0.0, current version 1.19.5)
\t@rpath/libde265.0.dylib (compatibility version 0.0.0, current version 0.0.0)
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1345.0.0)
"""

OTOOL_l = """Load command 17
          cmd LC_RPATH
      cmdsize 48
         path /opt/vcpkg/installed/arm64-osx-dynamic/lib (offset 12)
Load command 18
          cmd LC_RPATH
      cmdsize 32
         path @loader_path/../lib (offset 12)
Load command 19
      cmd LC_FUNCTION_STARTS
"""


class NotarizationTests(unittest.TestCase):
    def test_accepts_multiline_notarytool_json(self):
        with patch.object(macpack, "run", return_value='{\n  "id": "submission",\n  "status": "Accepted"\n}\n'):
            macpack.notarize(Path("app.zip"), "profile")

    def test_rejected_submission_fails_even_with_zero_exit_code(self):
        with patch.object(macpack, "run", return_value='{\n  "status": "Invalid"\n}\n'):
            with self.assertRaisesRegex(SystemExit, "Invalid"):
                macpack.notarize(Path("app.zip"), "profile")


class ReleaseSigningTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        app = root / "MediaViewer.app"
        (app / "Contents/Frameworks/Sparkle.framework").mkdir(parents=True)
        key = root / "sparkle.key"
        key.write_text("test-key", encoding="utf-8")
        self.args = argparse.Namespace(
            app=str(app), out_dir=str(root / "release"), sparkle_key_file=str(key),
            sparkle_bin=str(root / "sparkle/bin"), identity="test-identity",
            allow_no_updater=False, skip_notarize=True, notary_profile=None,
            download_url_prefix="https://example.test/v0.1.1/", phased_rollout_seconds=0)

    def release_with_feed(self, feed):
        def fake_run(cmd, **kwargs):
            if cmd[0] == "/usr/libexec/PlistBuddy":
                return "0.1.1"
            if Path(cmd[0]).name == "generate_appcast":
                self.assertIn("--ed-key-file", cmd)
                self.assertEqual(cmd[cmd.index("--ed-key-file") + 1], self.args.sparkle_key_file)
                self.assertNotIn("test-key", cmd)
                self.assertEqual(kwargs["timeout"], 300)
                (Path(cmd[-1]) / "appcast.xml").write_text(feed, encoding="utf-8")
            return ""
        with patch.object(macpack, "run", side_effect=fake_run), \
             patch.object(macpack, "sign_app"), patch.object(macpack, "make_dmg"), \
             patch.object(macpack, "codesign"):
            macpack.cmd_release(self.args)

    def test_ci_key_file_signs_feed_without_keychain(self):
        self.release_with_feed("<!-- sparkle-signatures -->\n<rss/>")

    def test_unsigned_feed_is_still_rejected(self):
        with self.assertRaisesRegex(SystemExit, "no feed signature"):
            self.release_with_feed("<rss/>")

    def test_missing_key_fails_before_signing_or_notarizing(self):
        Path(self.args.sparkle_key_file).unlink()
        with patch.object(macpack, "run") as runner, self.assertRaisesRegex(SystemExit, "key file not found"):
            macpack.cmd_release(self.args)
        runner.assert_not_called()

    def test_stalled_command_has_bounded_wait(self):
        with self.assertRaisesRegex(SystemExit, "timed out"):
            macpack.run([sys.executable, "-c", "import time; time.sleep(30)"], timeout=0.1)


class ParseTests(unittest.TestCase):
    def test_exe_deps(self):
        self.assertEqual(macpack.parse_otool_deps(OTOOL_L_EXE), [
            "@rpath/libheif.1.dylib",
            "@rpath/libavcodec.61.dylib",
            "@rpath/Sparkle.framework/Versions/B/Sparkle",
            "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit",
            "/usr/lib/libc++.1.dylib",
        ])

    def test_dylib_drops_own_id(self):
        self.assertEqual(macpack.parse_otool_deps(OTOOL_L_DYLIB, "@rpath/libheif.1.dylib"),
                         ["@rpath/libde265.0.dylib", "/usr/lib/libSystem.B.dylib"])

    def test_rpaths(self):
        self.assertEqual(macpack.parse_otool_rpaths(OTOOL_l),
                         ["/opt/vcpkg/installed/arm64-osx-dynamic/lib", "@loader_path/../lib"])

    def test_classify(self):
        self.assertTrue(macpack.is_system_dep("/usr/lib/libc++.1.dylib"))
        self.assertTrue(macpack.is_system_dep("/System/Library/Frameworks/Metal.framework/Metal"))
        self.assertFalse(macpack.is_system_dep("/opt/homebrew/lib/libx.dylib"))
        self.assertTrue(macpack.is_sparkle_dep("@rpath/Sparkle.framework/Versions/B/Sparkle"))


class ResolveTests(unittest.TestCase):
    def setUp(self):
        # These fixtures model dyld's POSIX paths even when run on Windows.
        self.paths = patch.object(macpack.os, "path", posixpath)
        self.paths.start()
        self.addCleanup(self.paths.stop)

    def test_rpath_in_order(self):
        present = {"/vcpkg/lib/libheif.1.dylib"}
        got = macpack.resolve_dep("@rpath/libheif.1.dylib", "/build", "/build",
                                  ["/nowhere", "/vcpkg/lib"], [], exists=present.__contains__)
        self.assertEqual(got, "/vcpkg/lib/libheif.1.dylib")

    def test_loader_path_rpath_uses_referrer(self):
        present = {"/vcpkg/lib/libde265.0.dylib"}
        got = macpack.resolve_dep("@rpath/libde265.0.dylib", "/vcpkg/bin", "/build",
                                  ["@loader_path/../lib"], [], exists=present.__contains__)
        self.assertEqual(got, "/vcpkg/lib/libde265.0.dylib")

    def test_extra_dir_last_resort(self):
        present = {"/prefix/lib/libraw_r.23.dylib"}
        got = macpack.resolve_dep("@rpath/libraw_r.23.dylib", "/build", "/build",
                                  ["/stale/build/tree"], ["/prefix/lib"], exists=present.__contains__)
        self.assertEqual(got, "/prefix/lib/libraw_r.23.dylib")

    def test_unresolvable(self):
        self.assertIsNone(macpack.resolve_dep("@rpath/libnope.dylib", "/b", "/b", ["/x"], [],
                                              exists=lambda _: False))

    def test_absolute(self):
        got = macpack.resolve_dep("/opt/x/libfoo.dylib", "/b", "/b", [], [],
                                  exists={"/opt/x/libfoo.dylib"}.__contains__)
        self.assertEqual(got, "/opt/x/libfoo.dylib")


class BundleCheckTests(unittest.TestCase):
    def test_clean_bundle(self):
        refs = {"Contents/MacOS/MediaViewer": ["@rpath/libheif.1.dylib", "/usr/lib/libc++.1.dylib",
                                               "@rpath/Sparkle.framework/Versions/B/Sparkle"]}
        self.assertEqual(macpack.check_bundle_refs(refs, {"libheif.1.dylib"}), [])

    def test_leak_outside_bundle(self):
        refs = {"Contents/MacOS/MediaViewer": ["/opt/homebrew/lib/libheif.1.dylib",
                                               "@rpath/libmissing.dylib"]}
        problems = macpack.check_bundle_refs(refs, {"libheif.1.dylib"})
        self.assertEqual(len(problems), 2)


class CrashpadHandlerTests(unittest.TestCase):
    """PR 11: the handler is a helper tool, signed inside-out before the app."""

    def test_handler_lands_in_helpers_and_is_signed_before_the_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src = root / "crashpad_handler"
            src.write_bytes(b"\xcf\xfa\xed\xfe")
            app = root / "MediaViewer.app"
            (app / "Contents" / "Frameworks").mkdir(parents=True)
            dst = macpack.copy_crashpad_handler(src, app)
            self.assertEqual(dst, app / "Contents" / "Helpers" / "crashpad_handler")
            self.assertTrue(dst.exists())
            self.assertTrue(dst.stat().st_mode & 0o111)
            signed = []
            with patch.object(macpack, "codesign", side_effect=lambda p, *a, **k: signed.append(p)), \
                 patch.object(macpack, "run"):
                macpack.sign_app(app, "-", Path("QuickLook.entitlements"), hardened=True)
            self.assertIn(dst, signed)
            self.assertLess(signed.index(dst), signed.index(app))

    def test_assemble_accepts_the_handler_option(self):
        with patch.object(macpack, "cmd_assemble") as assemble:
            macpack.main(["assemble", "--app", "a", "--exe", "e", "--appex-exe", "x",
                          "--info-plist", "i", "--appex-plist", "p", "--appex-entitlements", "q",
                          "--icon-png", "c", "--font", "f", "--dylib-dir", "d",
                          "--crashpad-handler", "h"])
        self.assertEqual(assemble.call_args[0][0].crashpad_handler, "h")
class UniversalFeedTests(unittest.TestCase):
    def test_universal_app_with_arch_restriction_fails(self):
        self.assertIsNotNone(macpack.universal_feed_problem(
            "<sparkle:hardwareRequirements>arm64</sparkle:hardwareRequirements>", ["arm64", "x86_64"]))

    def test_universal_app_without_restriction_passes(self):
        self.assertIsNone(macpack.universal_feed_problem("<item/>", ["x86_64", "arm64"]))

    def test_single_arch_app_may_be_restricted(self):
        self.assertIsNone(macpack.universal_feed_problem(
            "<sparkle:hardwareRequirements>arm64</sparkle:hardwareRequirements>", ["arm64"]))


class LipoMergeTests(unittest.TestCase):
    """lipo_merge.merge_trees with a fake `lipo`: a file's "architectures" are
    the words after the magic bytes."""
    MAGIC = b"\xcf\xfa\xed\xfe"

    def _mk(self, root: Path, files: dict[str, bytes]) -> Path:
        for rel, data in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        return root

    def _macho(self, *archs: str) -> bytes:
        return self.MAGIC + " ".join(archs).encode()

    @staticmethod
    def _archs(path: Path) -> frozenset[str]:
        return frozenset(path.read_bytes()[4:].decode().split())

    @staticmethod
    def _create(arm: Path, x64: Path, out: Path) -> None:
        out.write_bytes(arm.read_bytes() + b" " + x64.read_bytes()[4:])

    def _merge(self, arm: dict[str, bytes], x64: dict[str, bytes]):
        import lipo_merge
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            a = self._mk(t / "arm", arm)
            b = self._mk(t / "x64", x64)
            out = t / "out"
            warnings = lipo_merge.merge_trees(a, b, out, archs_of=self._archs,
                                              lipo_create=self._create)
            result = {p.relative_to(out).as_posix(): p.read_bytes()
                      for p in out.rglob("*") if p.is_file()}
        return warnings, result

    def test_thin_binaries_are_joined_and_universal_ones_left_alone(self):
        warnings, out = self._merge(
            {"Contents/MacOS/MediaViewer": self._macho("arm64"),
             "Contents/Frameworks/Sparkle": self._macho("arm64", "x86_64"),
             "Contents/Info.plist": b"plist"},
            {"Contents/MacOS/MediaViewer": self._macho("x86_64"),
             "Contents/Frameworks/Sparkle": self._macho("arm64", "x86_64"),
             "Contents/Info.plist": b"plist"})
        self.assertEqual(warnings, [])
        self.assertEqual(self._archs_of_bytes(out["Contents/MacOS/MediaViewer"]),
                         {"arm64", "x86_64"})
        self.assertEqual(out["Contents/Frameworks/Sparkle"], self._macho("arm64", "x86_64"))
        self.assertEqual(out["Contents/Info.plist"], b"plist")

    def _archs_of_bytes(self, data: bytes) -> set[str]:
        return set(data[4:].decode().split())

    def test_layout_mismatch_fails(self):
        with self.assertRaises(SystemExit):
            self._merge({"Contents/a.dylib": self._macho("arm64")}, {})

    def test_macho_in_one_app_only_fails(self):
        with self.assertRaises(SystemExit):
            self._merge({"Contents/x": self._macho("arm64")}, {"Contents/x": b"text"})

    def test_data_file_difference_warns_and_keeps_arm64(self):
        warnings, out = self._merge({"Contents/Resources/f": b"one"},
                                    {"Contents/Resources/f": b"two"})
        self.assertEqual(len(warnings), 1)
        self.assertEqual(out["Contents/Resources/f"], b"one")

    def test_code_signature_dirs_are_ignored(self):
        warnings, out = self._merge({"Contents/_CodeSignature/CodeResources": b"a"},
                                    {"Contents/_CodeSignature/CodeResources": b"b"})
        self.assertEqual(warnings, [])

if __name__ == "__main__":
    unittest.main()
