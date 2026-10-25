#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for macpack.py's pure helpers (no macOS needed; they run
on Linux). The otool samples are the formats Apple's cctools print."""
from __future__ import annotations

import os
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
            download_url_prefix="https://example.test/v0.1.1/", phased_rollout_seconds=0,
            team_id="ABCDE12345")

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

    def test_developer_id_release_without_a_team_fails_before_signing(self):
        """#501: without the team there is no Driftwire app group, so no keychain."""
        self.args.team_id = None
        with patch.object(macpack, "run") as runner, patch.object(macpack, "sign_app") as signer, \
             self.assertRaisesRegex(SystemExit, "team-id"):
            macpack.cmd_release(self.args)
        runner.assert_not_called()
        signer.assert_not_called()

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


class AddonSigningTests(unittest.TestCase):
    """Milestone G: the Import add-on is signed with the app's identity (library
    validation) and notarized before addon-pack.py packs it."""

    def _addon(self, root: Path) -> Path:
        addon = root / "import"
        (addon / "Import.bundle" / "Contents" / "MacOS").mkdir(parents=True)
        (addon / "Import.bundle" / "Contents" / "MacOS" / "Import").write_bytes(b"\xca\xfe\xba\xbe")
        (addon / "libmv_import.dylib").write_bytes(b"\xca\xfe\xba\xbe")
        return addon

    def test_dylib_and_bundle_are_signed_and_verified(self):
        with tempfile.TemporaryDirectory() as tmp:
            addon = self._addon(Path(tmp))
            signed, ran = [], []
            with patch.object(macpack, "codesign", side_effect=lambda p, *a, **k: signed.append(p)), \
                 patch.object(macpack, "run", side_effect=lambda cmd, **k: ran.append(cmd)):
                macpack.sign_addon(addon, "Developer ID Application: Test (TEAM)")
            self.assertEqual(signed, [addon / "libmv_import.dylib", addon / "Import.bundle"])
            verified = [c[-1] for c in ran if c[:2] == ["codesign", "--verify"]]
            self.assertEqual(verified, [str(p) for p in signed])

    def test_empty_folder_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(macpack, "codesign"), patch.object(macpack, "run"):
                with self.assertRaises(SystemExit):
                    macpack.sign_addon(Path(tmp), "-")

    def test_nested_runtime_dylib_is_signed_before_the_bundle(self):
        """Milestone H: the AI pack keeps ONNX Runtime in a subfolder; it is
        signed (deepest first) but nothing inside the bundle is signed twice."""
        with tempfile.TemporaryDirectory() as tmp:
            addon = Path(tmp) / "ai"
            (addon / "AI.bundle" / "Contents" / "MacOS").mkdir(parents=True)
            (addon / "AI.bundle" / "Contents" / "MacOS" / "AI").write_bytes(b"\xca\xfe\xba\xbe")
            (addon / "AI.bundle" / "Contents" / "Frameworks").mkdir()
            (addon / "AI.bundle" / "Contents" / "Frameworks" / "libx.dylib").write_bytes(b"x")
            (addon / "runtime").mkdir()
            (addon / "runtime" / "libonnxruntime.1.dylib").write_bytes(b"\xca\xfe\xba\xbe")
            (addon / "libmv_ai.dylib").write_bytes(b"\xca\xfe\xba\xbe")
            (addon / "models").mkdir()
            (addon / "models" / "clip.onnx").write_bytes(b"model")
            signed = []
            with patch.object(macpack, "codesign", side_effect=lambda p, *a, **k: signed.append(p)), \
                 patch.object(macpack, "run"):
                macpack.sign_addon(addon, "Developer ID Application: Test (TEAM)")
            self.assertEqual(signed, [addon / "runtime" / "libonnxruntime.1.dylib",
                                      addon / "libmv_ai.dylib", addon / "AI.bundle"])

    def test_code_free_piece_signs_nothing_and_skips_notarization(self):
        with tempfile.TemporaryDirectory() as tmp:
            piece = Path(tmp) / "ai-faces"
            (piece / "models").mkdir(parents=True)
            (piece / "models" / "faces.onnx").write_bytes(b"model")
            with patch.object(macpack, "codesign") as sign, patch.object(macpack, "run"):
                self.assertEqual(macpack.sign_addon(piece, "-", piece=True), [])
                sign.assert_not_called()
            with patch.object(macpack, "run"), patch.object(macpack, "notarize") as notarize:
                macpack.main(["addon", "--dir", str(piece), "--piece", "--identity", "I",
                              "--notary-profile", "p"])
                notarize.assert_not_called()

    def test_addon_command_notarizes_unless_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            addon = self._addon(Path(tmp))
            for skip, calls in ((True, 0), (False, 1)):
                with patch.object(macpack, "sign_addon") as sign, patch.object(macpack, "run"), \
                     patch.object(macpack, "notarize") as notarize:
                    argv = ["addon", "--dir", str(addon), "--identity", "Developer ID Application: T"]
                    argv += ["--skip-notarize"] if skip else ["--notary-profile", "p"]
                    macpack.main(argv)
                    sign.assert_called_once()
                    self.assertEqual(notarize.call_count, calls)

    def test_addon_command_requires_identity_and_profile(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop("MV_SIGN_IDENTITY", None)
            os.environ.pop("MV_NOTARY_PROFILE", None)
            with self.assertRaises(SystemExit):
                macpack.main(["addon", "--dir", "x"])
            with self.assertRaises(SystemExit):
                macpack.main(["addon", "--dir", "x", "--identity", "I"])


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

    def test_app_is_signed_with_its_entitlements_and_the_appex_with_its_own(self):
        """Issue #72: the hardened runtime needs the Photos entitlement on the
        app itself; the sandboxed extension keeps its own file."""
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp) / "MediaViewer.app"
            (app / "Contents" / "Frameworks").mkdir(parents=True)
            calls = {}
            with patch.object(macpack, "codesign",
                              side_effect=lambda p, *a, **k: calls.__setitem__(p, k.get("entitlements"))), \
                 patch.object(macpack, "run"):
                macpack.sign_app(app, "-", Path("QuickLook.entitlements"), hardened=True)
            self.assertEqual(calls[app], macpack.APP_ENTITLEMENTS)
            appex = app / "Contents" / "PlugIns" / f"{macpack.APPEX_NAME}.appex"
            self.assertEqual(calls[appex], Path("QuickLook.entitlements"))

    def test_a_team_adds_driftwires_app_group_from_the_bundle_id(self):
        """#501: "<team>.<bundle id>.driftwire", on top of the file's own."""
        import plistlib
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp) / "MediaViewer.app"
            (app / "Contents").mkdir(parents=True)
            with open(app / "Contents" / "Info.plist", "wb") as f:
                plistlib.dump({"CFBundleIdentifier": "io.github.longtimeno-c.mediaviewer"}, f)
            self.assertEqual(macpack.app_entitlements(app, None, Path(tmp) / "e.plist"), macpack.APP_ENTITLEMENTS)
            out = macpack.app_entitlements(app, "ABCDE12345", Path(tmp) / "e.plist")
            with open(out, "rb") as f:
                ent = plistlib.load(f)
            self.assertEqual(ent[macpack.APP_GROUPS], ["ABCDE12345.io.github.longtimeno-c.mediaviewer.driftwire"])
            self.assertIs(ent.get("com.apple.security.personal-information.photos-library"), True)
            with self.assertRaisesRegex(SystemExit, "Team ID"):
                macpack.app_entitlements(app, "not a team", Path(tmp) / "e.plist")

    def test_app_entitlements_grant_photos_and_no_sandbox(self):
        import plistlib
        with open(macpack.APP_ENTITLEMENTS, "rb") as f:
            ent = plistlib.load(f)
        self.assertIs(ent.get("com.apple.security.personal-information.photos-library"), True)
        # Cmd+E on a Photos item asks Photos to show it (docs/design/26 "Show in Photos").
        self.assertIs(ent.get("com.apple.security.automation.apple-events"), True)
        # The viewer opens any folder: sandboxing the app is a different decision.
        self.assertNotIn("com.apple.security.app-sandbox", ent)

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

    def test_a_folder_with_the_command_line_alone_is_joined(self):
        # release.yml's macos job joins the two driftwire programs this way.
        warnings, out = self._merge({"driftwire": self._macho("arm64")}, {"driftwire": self._macho("x86_64")})
        self.assertEqual(warnings, [])
        self.assertEqual(self._archs_of_bytes(out["driftwire"]), {"arm64", "x86_64"})

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

    def test_final_cut_pro_pieces_may_be_arm64_only(self):
        # docs/design/23: the Intel build has no AI pack, so no agent or extension.
        fcp = {"Contents/Library/LaunchAgents/x.search.plist": b"job",
               "Contents/PlugIns/MediaViewerSearch.appex/Contents/MacOS/MediaViewerSearch": self._macho("arm64"),
               "Contents/PlugIns/MediaViewerSearch.appex/Contents/_CodeSignature/CodeResources": b"sig"}
        warnings, out = self._merge({"Contents/MacOS/MediaViewer": self._macho("arm64"), **fcp},
                                    {"Contents/MacOS/MediaViewer": self._macho("x86_64")})
        self.assertEqual(warnings, [])
        for rel, data in fcp.items():
            self.assertEqual(out[rel], data)  # untouched, signature included
        self.assertEqual(self._archs_of_bytes(out["Contents/MacOS/MediaViewer"]), {"arm64", "x86_64"})

    def test_other_arm64_only_files_still_fail(self):
        with self.assertRaises(SystemExit):
            self._merge({"Contents/Helpers/SomethingElse": self._macho("arm64")}, {})


class FinalCutProPackTests(unittest.TestCase):
    """docs/design/23: the extension is signed before the app, with the assemble
    entitlements or (a release) the ones it carries. The agent is the app's
    own executable."""

    def _app(self, root: Path) -> Path:
        app = root / "MediaViewer.app"
        (app / "Contents" / "Frameworks").mkdir(parents=True)
        (app / "Contents" / "PlugIns" / f"{macpack.FCP_APPEX_NAME}.appex").mkdir(parents=True)
        return app

    def _sign(self, app: Path, **kw) -> list:
        calls = []
        with patch.object(macpack, "codesign", side_effect=lambda p, *a, **k: calls.append((p, k))), \
             patch.object(macpack, "run"):
            macpack.sign_app(app, "-", Path("QuickLook.entitlements"), hardened=True, **kw)
        return calls

    def test_assemble_signs_with_the_given_entitlements(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = self._app(Path(tmp))
            calls = dict((str(p), k) for p, k in self._sign(
                app, fcp_appex_entitlements=Path("Extension.entitlements")))
            appex = str(app / "Contents" / "PlugIns" / f"{macpack.FCP_APPEX_NAME}.appex")
            self.assertEqual(calls[appex]["entitlements"], Path("Extension.entitlements"))
            self.assertFalse(calls[appex]["preserve_entitlements"])

    def test_release_keeps_its_entitlements_and_signs_it_before_the_app(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = self._app(Path(tmp))
            calls = self._sign(app)
            paths = [p for p, _ in calls]
            appex = app / "Contents" / "PlugIns" / f"{macpack.FCP_APPEX_NAME}.appex"
            k = calls[paths.index(appex)][1]
            self.assertTrue(k["preserve_entitlements"])
            self.assertIsNone(k["entitlements"])
            self.assertLess(paths.index(appex), paths.index(app))

    def test_assemble_refuses_a_partial_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            for name in ("e", "x", "i", "p", "q", "c", "f", "appex"):
                (t / name).write_bytes(b"")
            with patch.object(macpack, "make_icns"), self.assertRaises(SystemExit) as cm:
                macpack.main(["assemble", "--app", str(t / "a.app"), "--exe", str(t / "e"),
                              "--appex-exe", str(t / "x"), "--info-plist", str(t / "i"),
                              "--appex-plist", str(t / "p"), "--appex-entitlements", str(t / "q"),
                              "--icon-png", str(t / "c"), "--font", str(t / "f"), "--dylib-dir", tmp,
                              "--fcp-appex-exe", str(t / "appex")])
            self.assertIn("--fcp-agent-plist", str(cm.exception))

class CommandLineTests(unittest.TestCase):
    """docs/design/28 "Installing": the driftwire program, signed bare (no
    entitlements: no keychain on macOS, #501), notarized in a zip (it cannot
    be stapled), and packed for the Homebrew tap."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binary = self.root / "driftwire"
        self.binary.write_bytes(b"\xca\xfe\xba\xbe universal program")

    def test_the_archive_says_what_the_program_runs_on(self):
        self.assertEqual(macpack.cli_arch_label(["x86_64", "arm64"]), "universal")
        self.assertEqual(macpack.cli_arch_label(["arm64"]), "arm64")
        for archs in (["x86_64"], [], ["arm64e"]):
            with self.assertRaises(SystemExit):
                macpack.cli_arch_label(archs)

    def test_archive_layout_modes_and_repeatability(self):
        import tarfile
        top = macpack.stage_cli(self.binary, "1.2.3", "universal", self.root / "stage")
        self.assertEqual(top.name, "driftwire-1.2.3-macos-universal")
        out = self.root / "a.tar.gz"
        digest = macpack.pack_cli_archive(top, out, mtime=1700000000)
        with tarfile.open(out) as tar:
            members = {m.name: m for m in tar.getmembers()}
        top_name = "driftwire-1.2.3-macos-universal"
        self.assertEqual(sorted(members), [top_name] + sorted(
            f"{top_name}/{n}" for n in ("COPYRIGHT.txt", "LICENSE", "NOTICE", "driftwire", "driftwire.1")))
        self.assertEqual(members[f"{top_name}/driftwire"].mode, 0o755)
        self.assertEqual(members[f"{top_name}/driftwire.1"].mode, 0o644)
        self.assertTrue(all(m.uid == 0 and m.gid == 0 for m in members.values()))
        with tarfile.open(out) as tar:
            self.assertEqual(tar.extractfile(f"{top_name}/driftwire").read(), self.binary.read_bytes())
            copyright_text = tar.extractfile(f"{top_name}/COPYRIGHT.txt").read().decode()
        self.assertIn("License: ISC", copyright_text)   # libsodium's notice goes with the program
        again = self.root / "b.tar.gz"
        self.assertEqual(macpack.pack_cli_archive(top, again, mtime=1700000000), digest)
        self.assertEqual(out.read_bytes(), again.read_bytes())

    def cli(self, *extra, archs="x86_64 arm64", says="driftwire 1.2.3"):
        ran, signed = [], []

        def fake_run(cmd, **kwargs):
            ran.append(cmd)
            if cmd[:2] == ["lipo", "-archs"]:
                return archs
            if cmd[-1] == "--version":
                return says + "\n"
            if cmd[0] == "ditto":
                Path(cmd[-1]).write_bytes(b"zip")
            return ""
        with patch.object(macpack, "run", side_effect=fake_run), \
             patch.object(macpack, "codesign", side_effect=lambda p, *a, **k: signed.append((p, a, k))), \
             patch.object(macpack, "notarize") as notarize:
            macpack.main(["cli", "--binary", str(self.binary), "--version", "1.2.3",
                          "--out-dir", str(self.root / "out"), *extra])
        return ran, signed, notarize

    def test_signed_with_a_stable_identifier_and_the_hardened_runtime_then_notarized(self):
        ran, signed, notarize = self.cli("--identity", "Developer ID Application: T (TEAM)",
                                         "--notary-profile", "p")
        (path, args, kwargs), = signed
        self.assertEqual(path.name, "driftwire")
        self.assertEqual(args, ("Developer ID Application: T (TEAM)",))
        self.assertEqual(kwargs, {"hardened": True, "identifier": "io.github.longtimeno-c.driftwire"})
        self.assertIn(["codesign", "--verify", "--strict", "--verbose=2", str(path)], ran)
        notarize.assert_called_once()
        self.assertTrue(str(notarize.call_args.args[0]).endswith("driftwire-1.2.3-macos-universal.zip"))
        self.assertEqual(notarize.call_args.args[1], "p")
        self.assertTrue((self.root / "out" / "driftwire-1.2.3-macos-universal.tar.gz").is_file())

    def test_an_ad_hoc_build_is_packed_unnotarized_and_an_arm64_one_says_so(self):
        _, signed, notarize = self.cli("--identity", "-", "--skip-notarize", archs="arm64")
        self.assertEqual(signed[0][1], ("-",))
        notarize.assert_not_called()
        self.assertTrue((self.root / "out" / "driftwire-1.2.3-macos-arm64.tar.gz").is_file())

    def test_a_program_of_another_version_is_refused_before_notarizing(self):
        with self.assertRaisesRegex(SystemExit, "driftwire 1.2.2"):
            self.cli("--identity", "-", "--notary-profile", "p", says="driftwire 1.2.2")
        self.assertFalse((self.root / "out" / "driftwire-1.2.3-macos-universal.tar.gz").exists())

    def test_codesign_names_the_identifier(self):
        with patch.object(macpack, "run") as runner:
            macpack.codesign(Path("driftwire"), "I", hardened=True, identifier="io.example.x")
        cmd = runner.call_args.args[0]
        self.assertEqual(cmd[cmd.index("--identifier") + 1], "io.example.x")
        self.assertIn("--timestamp", cmd)
        self.assertEqual(cmd[cmd.index("--options") + 1], "runtime")

    def test_cli_requires_identity_and_profile(self):
        with patch.dict(os.environ, {}, clear=False):
            os.environ.pop("MV_SIGN_IDENTITY", None)
            os.environ.pop("MV_NOTARY_PROFILE", None)
            for argv in (["cli", "--binary", "x", "--version", "1.2.3"],
                         ["cli", "--binary", "x", "--version", "1.2.3", "--identity", "I"]):
                with self.assertRaises(SystemExit):
                    macpack.main(argv)



if __name__ == "__main__":
    unittest.main()
