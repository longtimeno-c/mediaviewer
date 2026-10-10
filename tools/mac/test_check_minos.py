#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for check_minos.py's parsing (no macOS needed). The otool
samples are the formats Apple's cctools print."""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_minos  # noqa: E402

# Homebrew's libomp on a macOS 26 machine (issue #158). The tool versions
# after minos must not be read as the minimum.
THIN_BUILD_VERSION = """/opt/homebrew/opt/libomp/lib/libomp.dylib:
Load command 9
      cmd LC_UUID
  cmdsize 24
    uuid D0CAF578-FA67-33EB-81F6-0DF51F67F071
Load command 10
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 26.0
      sdk 26.5
   ntools 1
     tool 3
  version 1221.4
"""

FAT = """/usr/bin/true (architecture x86_64):
Load command 9
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 14.0
      sdk 15.2
Load command 10
      cmd LC_SOURCE_VERSION
  cmdsize 16
  version 1267.0
/usr/bin/true (architecture arm64):
Load command 9
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 15.0
      sdk 15.2
"""

VERSION_MIN = """/build/Sparkle:
Load command 8
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
  version 10.13
      sdk 14.2
"""

NO_MIN = """/build/odd.dylib:
Load command 0
      cmd LC_SEGMENT_64
  cmdsize 72
  segname __PAGEZERO
"""


class ParseTests(unittest.TestCase):
    def test_thin_build_version(self):
        self.assertEqual(check_minos.parse_minos(THIN_BUILD_VERSION), [("", "26.0")])

    def test_fat_reports_every_slice(self):
        self.assertEqual(check_minos.parse_minos(FAT), [("x86_64", "14.0"), ("arm64", "15.0")])

    def test_version_min_macosx(self):
        self.assertEqual(check_minos.parse_minos(VERSION_MIN), [("", "10.13")])

    def test_missing_load_command(self):
        self.assertEqual(check_minos.parse_minos(NO_MIN), [("", "")])


class CompareTests(unittest.TestCase):
    def test_version_key(self):
        self.assertEqual(check_minos.version_key("14.0"), check_minos.version_key("14"))
        self.assertLess(check_minos.version_key("10.13"), check_minos.version_key("14.0"))
        self.assertGreater(check_minos.version_key("14.0.1"), check_minos.version_key("14.0"))

    def test_newer_slice_is_a_problem(self):
        problems = check_minos.problems_for("libomp.dylib", check_minos.parse_minos(FAT), "14.0")
        self.assertEqual(problems, ["libomp.dylib (arm64): minos 15.0 > LSMinimumSystemVersion 14.0"])

    def test_equal_and_older_pass(self):
        self.assertEqual(check_minos.problems_for("x", [("", "14.0"), ("", "10.13")], "14.0"), [])

    def test_missing_minimum_is_a_problem(self):
        self.assertEqual(len(check_minos.problems_for("x", [("", "")], "14.0")), 1)


if __name__ == "__main__":
    unittest.main()
