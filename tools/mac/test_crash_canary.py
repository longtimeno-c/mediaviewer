#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for crash_canary.py (PR 11 macOS crash-reporting verify). Pure
Python: they run on Linux in CI like test_macpack.py."""
from __future__ import annotations

import io
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import crash_canary  # noqa: E402


class MakeTests(unittest.TestCase):
    def test_synthesised_pair(self):
        with tempfile.TemporaryDirectory() as tmp:
            with redirect_stdout(io.StringIO()):
                self.assertEqual(crash_canary.main(["make", "--out", tmp, "--width", "16", "--height", "4"]), 0)
            folder = Path(tmp) / crash_canary.FOLDER
            canary = (folder / crash_canary.CANARY).read_bytes()
            companion = (folder / crash_canary.COMPANION).read_bytes()
            self.assertEqual(canary[:2], b"BM")
            self.assertIn(crash_canary.MARKER, canary[:64 * 1024])
            self.assertNotIn(crash_canary.MARKER, companion)
            bgr, _ = crash_canary.pixel_patterns()
            self.assertIn(bytes.fromhex(bgr[:6]), companion)

    def test_copy_of_source_never_touches_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "IMG_0001.CR2"
            original = bytes(range(256)) * 32
            src.write_bytes(original)
            with redirect_stdout(io.StringIO()):
                self.assertEqual(crash_canary.main(["make", "--source", str(src), "--out", tmp]), 0)
            self.assertEqual(src.read_bytes(), original)
            out = (Path(tmp) / crash_canary.FOLDER / crash_canary.CANARY).read_bytes()
            self.assertEqual(out[0x1000:0x1000 + len(crash_canary.MARKER)], crash_canary.MARKER)


class ScanTests(unittest.TestCase):
    def dump(self, body: bytes, scrubbed: bool) -> bytes:
        header = struct.pack("<IIIII", 0x504D444D, 42899, 0, 32, crash_canary.SCRUB_MARKER if scrubbed else 0)
        return header + bytes(12) + body

    def test_hits_in_both_encodings_and_pixels(self):
        body = b"x /Users/alice/" + "PRIVATE_FOLDER_canary".encode("utf-16-le") + bytes.fromhex("C35A177EE129")
        hits = crash_canary.scan(body, ["alice", "private_folder_CANARY"], ["C35A17 7EE129"])
        labels = sorted({label.split(" ")[0] for label, _ in hits})
        self.assertEqual(len(hits), 3)
        self.assertIn("'alice'", labels)

    def test_clean_scrubbed_dump_passes_and_unscrubbed_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            clean = Path(tmp) / "a.dmp"
            clean.write_bytes(self.dump(b"/Users/_____/____", True))
            raw = Path(tmp) / "b.dmp"
            raw.write_bytes(self.dump(b"/Users/_____/____", False))
            with redirect_stdout(io.StringIO()):
                self.assertEqual(crash_canary.main(["scan", str(clean), "--forbid", "alice"]), 0)
                self.assertEqual(crash_canary.main(["scan", str(raw), "--forbid", "alice"]), 1)
                self.assertEqual(crash_canary.main(["scan", str(clean), "--pixel-hex", "ABC"]), 2)


if __name__ == "__main__":
    unittest.main()
