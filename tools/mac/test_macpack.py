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


if __name__ == "__main__":
    unittest.main()
