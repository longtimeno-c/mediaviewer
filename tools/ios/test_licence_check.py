#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/ios/licence_check.py must fail on what it exists to catch."""
from __future__ import annotations

import json
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import licence_check  # noqa: E402
import ort_telemetry_pin  # noqa: E402


def add_port(root: pathlib.Path, name: str, licence: str | None, text: str = "Permission is hereby granted") -> None:
    share = root / "share" / name
    share.mkdir(parents=True)
    (share / "copyright").write_text(text, encoding="utf-8")
    if licence is not None:
        (share / "vcpkg.spdx.json").write_text(
            json.dumps({"packages": [{"name": name, "licenseConcluded": licence}]}), encoding="utf-8"
        )


class Ports(unittest.TestCase):
    def setUp(self):
        self._dir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._dir.name)
        self.addCleanup(self._dir.cleanup)

    def run_check(self):
        return licence_check.main(["--installed", str(self.root)])

    def test_permissive_ports_pass(self):
        add_port(self.root, "libsodium", "ISC")
        add_port(self.root, "blake3", "CC0-1.0 OR Apache-2.0")
        self.assertEqual(self.run_check(), 0)

    def test_helper_folder_is_not_a_port(self):
        add_port(self.root, "libsodium", "ISC")
        (self.root / "share" / "unofficial-sodium").mkdir()
        self.assertEqual(self.run_check(), 0)

    def test_gpl_fails(self):
        add_port(self.root, "exiv2", "GPL-2.0-or-later")
        self.assertEqual(self.run_check(), 1)

    def test_lgpl_fails(self):
        add_port(self.root, "ffmpeg", "LGPL-2.1-only")
        self.assertEqual(self.run_check(), 1)

    def test_dual_licence_naming_gpl_fails(self):
        add_port(self.root, "either", "MIT OR GPL-3.0-only")
        self.assertEqual(self.run_check(), 1)

    def test_gpl_named_only_in_the_text_fails(self):
        add_port(self.root, "sneaky", "MIT", text="Parts are under the GNU Lesser General Public License.")
        self.assertEqual(self.run_check(), 1)

    def test_every_spelling_in_the_text_fails(self):
        for i, text in enumerate(("GNU LGPL", "GNU Lesser Public License", "LGPLv3", "GNU Lesser GPL", "licensed under the GPL",
                                  "GNU Library General Public Licence", "AGPL-3.0", "Lesser General Public License")):
            with self.subTest(text):
                self.tearDown_ports()
                add_port(self.root, f"port{i}", "MIT", text=text)
                self.assertEqual(self.run_check(), 1)

    def tearDown_ports(self):
        shutil.rmtree(self.root / "share", ignore_errors=True)

    def test_a_word_containing_the_letters_passes(self):
        add_port(self.root, "fine", "MIT", text="See libgplot and the MPL-free notice.")
        self.assertEqual(self.run_check(), 0)

    def test_non_permissive_expression_fails(self):
        add_port(self.root, "mpl", "MPL-2.0")
        self.assertEqual(self.run_check(), 1)

    def test_every_alternative_must_be_permissive(self):
        add_port(self.root, "either", "MIT OR MPL-2.0")
        self.assertEqual(self.run_check(), 1)

    def test_permissive_expressions_pass(self):
        add_port(self.root, "a", "(CC0-1.0 OR Apache-2.0)")
        add_port(self.root, "b", "Apache-2.0 WITH LLVM-exception")
        add_port(self.root, "c", "blessing")
        self.assertEqual(self.run_check(), 0)

    def test_unknown_exception_fails(self):
        add_port(self.root, "a", "Apache-2.0 WITH Some-exception")
        self.assertEqual(self.run_check(), 1)

    def test_reviewed_port_passes_for_its_exact_licence_only(self):
        reviewed = {"dual": {"expression": "MIT OR GPL-2.0-only", "reason": "taken under MIT"}}
        with mock.patch.object(licence_check, "REVIEWED_PORTS", reviewed):
            add_port(self.root, "dual", "MIT OR GPL-2.0-only", text="GNU General Public License")
            self.assertEqual(self.run_check(), 0)
            self.tearDown_ports()
            add_port(self.root, "dual", "GPL-2.0-only")
            self.assertEqual(self.run_check(), 1)

    def test_undeclared_licence_fails(self):
        add_port(self.root, "mystery", None)
        add_port(self.root, "shrug", "NOASSERTION")
        self.assertEqual(self.run_check(), 1)

    def test_missing_tree_fails(self):
        self.assertEqual(licence_check.main(["--installed", str(self.root / "absent")]), 1)

    def test_licence_file(self):
        add_port(self.root, "libsodium", "ISC")
        good = self.root / "LICENSE"
        good.write_text("MIT License", encoding="utf-8")
        self.assertEqual(licence_check.main(["--installed", str(self.root), "--licence", f"ort={good}"]), 0)
        good.write_text("GNU General Public License, version 3", encoding="utf-8")
        self.assertEqual(licence_check.main(["--installed", str(self.root), "--licence", f"ort={good}"]), 1)
        self.assertEqual(licence_check.main(["--installed", str(self.root), "--licence", "ort=/nonexistent"]), 1)


class Symbols(unittest.TestCase):
    def test_banned_names(self):
        for name in ("_avcodec_open2", "_heif_context_alloc", "_de265_decode", "_libraw_init", "_smb2_connect_share",
                     "__ZN5Exiv24Xmp", "_sws_scale", "_gnutls_init", "___gmpz_init", "___gmpn_add_n", "_gmp_randinit",
                     "_mpg123_open", "_nettle_sha256_init"):
            self.assertTrue(licence_check.BANNED_SYMBOLS.search(name), name)

    def test_ordinary_names(self):
        for name in ("_OrtGetApiBase", "_sqlite3_open", "_sodium_init", "_blake3_hasher_init", "_avail_bytes"):
            self.assertFalse(licence_check.BANNED_SYMBOLS.search(name), name)


class OtoolOutput(unittest.TestCase):
    LIBC = "/usr/lib/libSystem.B.dylib"

    def line(self, library: str) -> str:
        return f"\t{library} (compatibility version 1.0.0, current version 1.0.0)"

    def test_one_architecture(self):
        out = "\n".join(["/tmp/App:", self.line(self.LIBC)])
        self.assertEqual(licence_check.loaded_libraries(out), [self.LIBC])

    def test_a_fat_binary_has_a_header_per_architecture(self):
        out = "\n".join([
            "/tmp/App (architecture x86_64):", self.line(self.LIBC),
            "/tmp/App (architecture arm64):", self.line(self.LIBC), self.line("@rpath/libavcodec.dylib"),
        ])
        self.assertEqual(licence_check.loaded_libraries(out), [self.LIBC, self.LIBC, "@rpath/libavcodec.dylib"])


class Bundles(unittest.TestCase):
    def test_every_mach_o_in_an_app_is_found(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = pathlib.Path(tmp) / "MediaViewer.app"
            (app / "PlugIns" / "Share.appex").mkdir(parents=True)
            (app / "MediaViewer").write_bytes(b"\xcf\xfa\xed\xfe rest")
            (app / "PlugIns" / "Share.appex" / "Share").write_bytes(b"\xca\xfe\xba\xbe rest")
            (app / "Info.plist").write_bytes(b"bplist00")
            self.assertEqual([p.name for p in licence_check.programs_in(app)], ["MediaViewer", "Share"])

    def test_a_bundle_with_no_program_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(len(licence_check.check_binary(pathlib.Path(tmp))), 1)


class TelemetryPin(unittest.TestCase):
    GOOD = b"\0".join([b"x", ort_telemetry_pin.OPT_OUT, *sorted(ort_telemetry_pin.REVIEWED_ENDPOINTS), b"y"])

    def check(self, content: bytes, version: str = ort_telemetry_pin.REVIEWED_VERSION) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            library = pathlib.Path(tmp) / "onnxruntime"
            library.write_bytes(content)
            return ort_telemetry_pin.problems(library, version)

    def test_the_reviewed_library_passes(self):
        self.assertEqual(self.check(self.GOOD), [])

    def test_a_new_version_fails(self):
        self.assertTrue(self.check(self.GOOD, "9.9.9"))

    def test_a_renamed_opt_out_fails(self):
        self.assertTrue(self.check(self.GOOD.replace(ort_telemetry_pin.OPT_OUT, b"ORT_NO_TELEMETRY_NOW")))

    def test_a_new_collector_fails(self):
        self.assertTrue(self.check(self.GOOD + b"\0https://new.events.data.microsoft.com/OneCollector/2.0\0"))

    def test_a_missing_collector_fails(self):
        # The reviewed code has gone, so what the opt-out does has to be read again.
        self.assertTrue(self.check(b"\0".join([b"x", ort_telemetry_pin.OPT_OUT])))


if __name__ == "__main__":
    unittest.main()
