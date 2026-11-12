#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for driftwire_spk.py (docs/plans/nas-setup.md §4): the SPK's members, INFO, the
privilege and resource files, the scripts (checked by sh -n, and run against a stand-in
driftwire in a temporary folder laid out as DSM lays out a package), the wizard, and
reproducibility. No Synology needed; what DSM itself accepts is spike S5's real-unit check."""
from __future__ import annotations

import gzip
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tarfile
import tempfile
import time
import unittest

spec = importlib.util.spec_from_file_location('driftwire_spk', Path(__file__).with_name('driftwire_spk.py'))
spk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(spk)

BIN = b'\x7fELF' + bytes(60)


def members(data: bytes) -> dict[str, tarfile.TarInfo]:
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        return {m.name: m for m in t.getmembers()}


def read(data: bytes, name: str) -> bytes:
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        return t.extractfile(name).read()


class SpkTests(unittest.TestCase):
    def test_members_and_modes(self):
        data = spk.build_spk(BIN, 'x86_64', '1.2.3')
        m = members(data)
        for name in ['INFO', 'package.tgz', 'scripts/start-stop-status', 'scripts/postinst', 'conf/privilege',
                     'conf/resource', 'conf/mediaviewer-nas.sc', 'WIZARD_UIFILES/install_uifile',
                     'PACKAGE_ICON.PNG', 'PACKAGE_ICON_256.PNG']:
            self.assertIn(name, m)
        for name, info in m.items():
            self.assertEqual((info.uid, info.gid), (0, 0))
            want = 0o755 if name.startswith('scripts/') else 0o644
            self.assertEqual(stat.S_IMODE(info.mode), want, name)

    def test_info(self):
        info = read(spk.build_spk(BIN, 'aarch64', '1.2.3', build=7), 'INFO').decode()
        fields = dict(line.split('=', 1) for line in info.strip().splitlines())
        self.assertEqual(fields['package'], '"mediaviewer-nas"')
        self.assertEqual(fields['version'], '"1.2.3-0007"')
        self.assertEqual(fields['os_min_ver'], '"7.2-64570"')
        self.assertEqual(fields['arch'], '"rtd1296 rtd1619b armada37xx"')
        self.assertEqual(fields['silent_upgrade'], '"yes"')
        for v in fields.values():
            self.assertTrue(v.startswith('"') and v.endswith('"'))

    def test_runs_as_its_own_user_and_reads_the_share_only(self):
        data = spk.build_spk(BIN, 'x86_64', '1.0.0')
        self.assertEqual(json.loads(read(data, 'conf/privilege')), {'defaults': {'run-as': 'package'}})
        res = json.loads(read(data, 'conf/resource'))
        share = res['data-share']['shares'][0]
        self.assertEqual(share['name'], '{{wizard_share}}')
        self.assertEqual(share['permission'], {'ro': ['mediaviewer-nas']})
        self.assertIn('dst.ports="7878/tcp"', read(data, 'conf/mediaviewer-nas.sc').decode())
        wizard = json.loads(read(data, 'WIZARD_UIFILES/install_uifile'))
        keys = [s['key'] for step in wizard for item in step['items'] for s in item['subitems']]
        self.assertEqual(keys, ['wizard_share'])

    def test_the_binary_is_inside_executable(self):
        data = spk.build_spk(BIN, 'x86_64', '1.0.0')
        with tarfile.open(fileobj=io.BytesIO(gzip.decompress(read(data, 'package.tgz')))) as t:
            info = t.getmember('bin/driftwire')
            self.assertEqual(stat.S_IMODE(info.mode), 0o755)
            self.assertEqual(t.extractfile(info).read(), BIN)

    def test_reproducible(self):
        self.assertEqual(spk.build_spk(BIN, 'x86_64', '1.0.0'), spk.build_spk(BIN, 'x86_64', '1.0.0'))

    def test_refusals(self):
        for args in [(BIN, 'armv7', '1.0.0'), (BIN, 'x86_64', '1.0'), (b'', 'x86_64', '1.0.0')]:
            with self.assertRaises(ValueError):
                spk.build_spk(*args)
        with self.assertRaises(ValueError):
            spk.build_spk(BIN, 'x86_64', '1.0.0', build=0)

    def test_scripts_parse(self):
        data = spk.build_spk(BIN, 'x86_64', '1.0.0')
        for name in members(data):
            if not name.startswith('scripts/'):
                continue
            r = subprocess.run(['sh', '-n'], input=read(data, name), capture_output=True)
            self.assertEqual(r.returncode, 0, (name, r.stderr))

    def test_install_start_status_stop_against_a_stand_in(self):
        data = spk.build_spk(BIN, 'x86_64', '1.0.0')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            target, var, scripts = root / 'target', root / 'var', root / 'scripts'
            (target / 'bin').mkdir(parents=True)
            var.mkdir()
            scripts.mkdir()
            calls = root / 'calls.txt'
            # A stand-in driftwire: init writes a key file; serve sleeps until killed.
            (target / 'bin' / 'driftwire').write_text(
                '#!/bin/sh\n'
                f'echo "$@" >>"{calls}"\n'
                'case "$*" in *init*) touch "$3/identity"; exit 0;; esac\n'
                'exec sleep 60\n')
            os.chmod(target / 'bin' / 'driftwire', 0o755)
            for name in ['start-stop-status', 'postinst']:
                (scripts / name).write_bytes(read(data, f'scripts/{name}'))
                os.chmod(scripts / name, 0o755)
            env = dict(os.environ, SYNOPKG_PKGVAR=str(var), SYNOPKG_PKGDEST=str(target), wizard_share='Photos')
            run = lambda *a: subprocess.run([str(scripts / a[0]), *a[1:]], env=env, capture_output=True, timeout=30)
            self.assertEqual(run('postinst').returncode, 0)
            cfg = json.loads((var / 'serve' / 'serve.json').read_text())
            self.assertEqual(cfg['port'], 7878)
            self.assertEqual(cfg['roots'][0]['label'], 'Photos')
            self.assertEqual(cfg['roots'][0]['path'], '/var/packages/mediaviewer-nas/shares/Photos')
            self.assertRegex(cfg['roots'][0]['id'], r'^[0-9a-f]{16}$')
            self.assertEqual(stat.S_IMODE((var / 'serve').stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((var / 'serve' / 'serve.json').stat().st_mode), 0o600)
            # Run again (an upgrade): nothing is made twice.
            self.assertEqual(run('postinst').returncode, 0)
            self.assertEqual(calls.read_text().count('init'), 1)
            self.assertEqual(run('start-stop-status', 'status').returncode, 3)
            self.assertEqual(run('start-stop-status', 'start').returncode, 0)
            self.assertEqual(run('start-stop-status', 'status').returncode, 0)
            self.assertIn(f'serve --data {var}/serve', calls.read_text())
            self.assertEqual(run('start-stop-status', 'stop').returncode, 0)
            self.assertEqual(run('start-stop-status', 'status').returncode, 3)
            # A verified update the service staged is the one started.
            (var / 'bin').mkdir()
            shutil.copy(target / 'bin' / 'driftwire', var / 'bin' / 'driftwire')
            (var / 'bin' / 'driftwire').write_text((var / 'bin' / 'driftwire').read_text().replace('"$@"', '"staged $@"'))
            os.chmod(var / 'bin' / 'driftwire', 0o755)
            self.assertEqual(run('start-stop-status', 'start').returncode, 0)
            for _ in range(100):  # started in the background: its first line comes a moment later
                if 'staged serve' in calls.read_text():
                    break
                time.sleep(0.05)
            self.assertEqual(run('start-stop-status', 'stop').returncode, 0)
            self.assertIn('staged serve', calls.read_text())


if __name__ == '__main__':
    unittest.main()
