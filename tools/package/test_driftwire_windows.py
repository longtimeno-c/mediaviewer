#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for driftwire-windows.ps1 (docs/design/28 "Installing"): reading a PE file's import
and delay-load tables, the closure of what driftwire.exe loads (ours from the build, the
Visual C++ runtime from the redistributable folder, Windows' own left out, anything else
refused), the staged folder and the copies to sign, and the zip with the signed copies in it.
The PE files are tiny ones made here (pe_fixture.py); Windows runs the real thing in CI.
Skips without PowerShell (pwsh)."""
from pathlib import Path
import shutil
import re
import subprocess
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_fixture import make_pe  # noqa: E402

SCRIPT = Path(__file__).with_name('driftwire-windows.ps1')
PWSH = shutil.which('pwsh')


@unittest.skipIf(not PWSH, 'needs PowerShell (pwsh)')
class DriftwireWindowsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='dwwin-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def ps(self, *args, check=True):
        result = subprocess.run([PWSH, '-NoProfile', '-NonInteractive', '-File', str(SCRIPT), *map(str, args)],
                                capture_output=True, text=True)
        if check and result.returncode != 0:
            self.fail(f'driftwire-windows.ps1 {" ".join(map(str, args))} failed:\n{result.stdout}\n{result.stderr}')
        return result

    @staticmethod
    def said(result):
        """What the script said, as one line: PowerShell's error view colours it
        and wraps it at the console's width ('     | ' on each line), which
        differs between a desktop and a CI runner."""
        text = re.sub(r'\x1b\[[0-9;]*m', '', result.stdout + result.stderr)
        text = re.sub(r'^\s*\|\s?', '', text, flags=re.M)
        return ' '.join(text.split())

    def write(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    # ---- reading imports ----

    def test_imports_and_delay_imports(self):
        exe = self.write(self.tmp / 'a.exe', make_pe(['KERNEL32.dll', 'libsodium.dll'], delay=['crypt32.dll']))
        self.assertEqual(self.ps('-Action', 'Imports', '-Path', exe).stdout.split(),
                         ['KERNEL32.dll', 'libsodium.dll', 'crypt32.dll'])
        old = self.write(self.tmp / 'b.exe', make_pe(['KERNEL32.dll'], machine=0x14C, pe32=True))
        self.assertEqual(self.ps('-Action', 'Imports', '-Path', old).stdout.split(), ['KERNEL32.dll'])

    def test_not_a_pe_file(self):
        bad = self.write(self.tmp / 'x.exe', b'#!/bin/sh\necho hello\n' + b'\0' * 64)
        result = self.ps('-Action', 'Imports', '-Path', bad, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not a PE file', self.said(result))

    # ---- staging and packing ----

    def tree(self, exe_imports, extra_bin=()):
        """A build tree, a redistributable folder and a System32, as on the runner."""
        bin_dir = self.tmp / 'build/bin/Release'
        self.write(bin_dir / 'driftwire.exe', make_pe(exe_imports))
        self.write(bin_dir / 'libsodium.dll', make_pe(['KERNEL32.dll', 'ADVAPI32.dll', 'VCRUNTIME140.dll']))
        self.write(bin_dir / 'blake3.dll', make_pe(['KERNEL32.dll', 'api-ms-win-crt-heap-l1-1-0.dll']))
        # Built beside it for the app, and not driftwire's.
        self.write(bin_dir / 'avcodec-61.dll', make_pe(['KERNEL32.dll']))
        self.write(bin_dir / 'mediaviewer_lab.exe', make_pe(['avcodec-61.dll']))
        for name, data in extra_bin:
            self.write(bin_dir / name, data)
        redist = self.tmp / 'redist/x64/Microsoft.VC143.CRT'
        self.write(redist / 'vcruntime140.dll', make_pe(['KERNEL32.dll', 'api-ms-win-crt-runtime-l1-1-0.dll']))
        self.write(redist / 'vcruntime140_1.dll', make_pe(['vcruntime140.dll', 'KERNEL32.dll']))
        self.write(redist / 'msvcp140.dll', make_pe(['vcruntime140.dll', 'KERNEL32.dll']))
        self.write(redist / 'concrt140.dll', make_pe(['KERNEL32.dll']))
        system = self.tmp / 'System32'
        for name in ('kernel32.dll', 'advapi32.dll', 'ws2_32.dll', 'ucrtbase.dll'):
            self.write(system / name, make_pe([]))
        return redist, system

    def stage(self, redist, system, check=True):
        return self.ps('-Action', 'Stage', '-BuildDir', self.tmp / 'build', '-Version', '1.2.3',
                       '-OutDir', self.tmp / 'out', '-RedistDir', redist, '-SystemDir', system, check=check)

    def test_stage_carries_what_it_loads_and_nothing_else(self):
        redist, system = self.tree(['KERNEL32.dll', 'libsodium.dll', 'BLAKE3.DLL', 'MSVCP140.dll',
                                    'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll', 'WS2_32.dll',
                                    'api-ms-win-crt-runtime-l1-1-0.dll'])
        self.stage(redist, system)
        top = self.tmp / 'out/driftwire-1.2.3-windows-x64'
        self.assertEqual(sorted(p.name for p in top.iterdir()), sorted([
            'driftwire.exe', 'libsodium.dll', 'blake3.dll', 'msvcp140.dll', 'vcruntime140.dll',
            'vcruntime140_1.dll', 'LICENSE', 'NOTICE', 'COPYRIGHT.txt']))
        # Ours only, to sign: Microsoft's runtime keeps Microsoft's signature.
        self.assertEqual(sorted(p.name for p in (self.tmp / 'out/sign').iterdir()),
                         ['blake3.dll', 'driftwire.exe', 'libsodium.dll'])
        self.assertIn('License: ISC', (top / 'COPYRIGHT.txt').read_text())
        self.assertEqual((top / 'vcruntime140.dll').read_bytes(), (redist / 'vcruntime140.dll').read_bytes())

    def test_a_dll_from_nowhere_is_refused(self):
        redist, system = self.tree(['KERNEL32.dll', 'libsodium.dll', 'ftxui-dom.dll'])
        result = self.stage(redist, system, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('needs ftxui-dom.dll (through driftwire.exe)', self.said(result))
        # Found through a DLL, it names that DLL.
        redist, system = self.tree(['KERNEL32.dll', 'libsodium.dll'],
                                   extra_bin=[('libsodium.dll', make_pe(['KERNEL32.dll', 'zlib1.dll']))])
        result = self.stage(redist, system, check=False)
        self.assertIn('needs zlib1.dll (through libsodium.dll)', self.said(result))

    def test_a_32_bit_program_is_refused(self):
        redist, system = self.tree([], extra_bin=[('driftwire.exe', make_pe(['KERNEL32.dll'], machine=0x14C, pe32=True))])
        result = self.stage(redist, system, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('driftwire.exe is not x64', self.said(result))

    def test_pack_zips_the_folder_with_the_signed_copies(self):
        redist, system = self.tree(['KERNEL32.dll', 'libsodium.dll', 'blake3.dll', 'VCRUNTIME140.dll'])
        self.stage(redist, system)
        # The signing step changes the copies in sign/ in place.
        signed_exe = make_pe(['KERNEL32.dll', 'libsodium.dll', 'blake3.dll', 'VCRUNTIME140.dll'], signed=True)
        (self.tmp / 'out/sign/driftwire.exe').write_bytes(signed_exe)
        self.ps('-Action', 'Pack', '-Version', '1.2.3', '-OutDir', self.tmp / 'out', '-NoRun')
        with zipfile.ZipFile(self.tmp / 'out/driftwire-1.2.3-windows-x64.zip') as z:
            names = sorted(n for n in z.namelist() if not n.endswith('/'))
            self.assertEqual(names, sorted(f'driftwire-1.2.3-windows-x64/{n}' for n in (
                'driftwire.exe', 'libsodium.dll', 'blake3.dll', 'vcruntime140.dll', 'LICENSE', 'NOTICE',
                'COPYRIGHT.txt')))
            self.assertEqual(z.read('driftwire-1.2.3-windows-x64/driftwire.exe'), signed_exe)

    def test_pack_refuses_a_signed_file_that_was_not_staged(self):
        redist, system = self.tree(['KERNEL32.dll', 'libsodium.dll'])
        self.stage(redist, system)
        self.write(self.tmp / 'out/sign/other.dll', make_pe([]))
        result = self.ps('-Action', 'Pack', '-Version', '1.2.3', '-OutDir', self.tmp / 'out', '-NoRun', check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('other.dll', self.said(result))
        self.assertFalse((self.tmp / 'out/driftwire-1.2.3-windows-x64.zip').exists())


if __name__ == '__main__':
    unittest.main()
