#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for driftwire_repo.py (docs/design/28 "Installing"): the site's layout, the apt
Release and its hashes, every signature checked with gpgv and rpmkeys, the .repo file, the
installers' baked values, the page, and each refusal; and the Windows installer run end to end
off Windows (tools/package/check-driftwire-install.ps1 in install.ps1's test mode, with pwsh).
The packages are tiny ones made here (dpkg-deb, rpmbuild, tarfile, zipfile and pe_fixture.py)
and the keys are throwaway ones in a temporary GnuPG home. Skips when the tools are missing
(apt-utils, createrepo-c, rpm, gnupg on Debian and Ubuntu; pwsh for the Windows installer).
"""
from datetime import timedelta
import email.utils
import gzip
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location('driftwire_repo', Path(__file__).with_name('driftwire_repo.py'))
repo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repo)
sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_fixture import make_pe  # noqa: E402

PWSH = shutil.which('pwsh')

BASE = 'https://example.github.io/mediaviewer/driftwire'
MISSING = [tool for tool in (*repo.TOOLS, 'rpmbuild', 'gpgconf', 'sh') if not shutil.which(tool)]


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, **kwargs)


def make_deb(folder, version, arch, package='driftwire'):
    root = Path(tempfile.mkdtemp(dir=folder))
    (root / 'DEBIAN').mkdir()
    (root / 'DEBIAN/control').write_text(
        f'Package: {package}\nVersion: {version}\nArchitecture: {arch}\nMaintainer: Test <t@invalid>\n'
        f'Description: test\n a test package\n')
    (root / 'usr/bin').mkdir(parents=True)
    (root / 'usr/bin/driftwire').write_text(f'#!/bin/sh\necho driftwire {version}\n')
    os.chmod(root / 'usr/bin/driftwire', 0o755)
    out = folder / f'driftwire_{version}_{arch}.deb'
    run('dpkg-deb', '--root-owner-group', '-b', root, out)
    shutil.rmtree(root)
    return out


def make_rpm(folder, version, arch):
    top = Path(tempfile.mkdtemp(dir=folder))
    spec_file = top / 'driftwire.spec'
    spec_file.write_text(
        f'Name: driftwire\nVersion: {version}\nRelease: 1\nSummary: test\nLicense: MIT\n'
        '%define debug_package %{nil}\n%define _build_id_links none\n'
        '%description\na test package\n'
        '%install\nmkdir -p %{buildroot}/usr/bin\n'
        f"printf '#!/bin/sh\\necho driftwire {version}\\n' > %{{buildroot}}/usr/bin/driftwire\n"
        'chmod 755 %{buildroot}/usr/bin/driftwire\n'
        '%files\n/usr/bin/driftwire\n')
    run('rpmbuild', '-bb', '--quiet', '--target', arch, '--define', f'_topdir {top}', spec_file)
    built = top / 'RPMS' / arch / f'driftwire-{version}-1.{arch}.rpm'
    out = folder / built.name
    shutil.move(built, out)
    shutil.rmtree(top)
    return out


def make_tarball(folder, version, arch):
    top = f'driftwire-{version}-linux-{arch}'
    out = folder / f'{top}.tar.gz'
    with tarfile.open(out, 'w:gz') as tar:
        for name in (top, f'{top}/bin'):
            info = tarfile.TarInfo(name)
            info.type, info.mode = tarfile.DIRTYPE, 0o755
            tar.addfile(info)
        data = f'#!/bin/sh\necho driftwire {version}\n'.encode()
        info = tarfile.TarInfo(f'{top}/bin/driftwire')
        info.size, info.mode = len(data), 0o755
        tar.addfile(info, io.BytesIO(data))
    return out


def make_zip(folder, version, signed=False, exe=None, extra=()):
    """driftwire-windows.ps1's zip: one folder, a driftwire.exe that is a PE file (and, run by
    sh, says its version: install.ps1's test mode), a DLL and the notices."""
    top = f'driftwire-{version}-windows-x64'
    out = folder / f'{top}.zip'
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(f'{top}/driftwire.exe', exe if exe is not None else
                   make_pe(['KERNEL32.dll', 'libsodium.dll'], signed=signed, says=f'driftwire {version}'))
        z.writestr(f'{top}/libsodium.dll', make_pe(['KERNEL32.dll'], signed=signed))
        z.writestr(f'{top}/LICENSE', 'GPL')
        for name, data in extra:
            z.writestr(name, data)
    return out


def make_set(folder, version, arches=('amd64', 'arm64'), windows=True):
    folder.mkdir(parents=True, exist_ok=True)
    for arch in arches:
        make_deb(folder, version, arch)
        make_rpm(folder, version, repo.ARCHES[arch])
        make_tarball(folder, version, repo.ARCHES[arch])
    if windows:
        make_zip(folder, version)
    return folder


def parse_release(text):
    fields, current = {}, None
    for line in text.splitlines():
        if line.startswith(' '):
            fields[current].append(line.split())
        else:
            key, _, value = line.partition(':')
            current = key
            fields[key] = value.strip() if value.strip() else []
    return fields


@unittest.skipIf(MISSING, 'needs ' + ', '.join(MISSING))
class DriftwireRepoTest(unittest.TestCase):
    @classmethod
    def make_key(cls, uid, passphrase=''):
        run('gpg', '--homedir', cls.gnupg, '--batch', '--pinentry-mode', 'loopback', '--passphrase', passphrase,
            '--quick-gen-key', uid, 'rsa2048', 'sign', 'never')
        out = run('gpg', '--homedir', cls.gnupg, '--batch', '--with-colons', '--list-secret-keys', uid,
                  text=True).stdout
        return next(line.split(':')[9] for line in out.splitlines() if line.startswith('fpr:'))

    @classmethod
    def setUpClass(cls):
        # A short path: gpg-agent's socket lives in the GnuPG home.
        cls.tmp = Path(tempfile.mkdtemp(prefix='dwrepo-'))
        cls.gnupg = cls.tmp / 'g'
        cls.gnupg.mkdir(mode=0o700)
        # No cached passphrases: each signing must bring its own, as in a release.
        (cls.gnupg / 'gpg-agent.conf').write_text('default-cache-ttl 0\nmax-cache-ttl 0\n')
        cls.key = cls.make_key('Driftwire test <test@invalid>')
        cls.key2 = cls.make_key('Driftwire next <next@invalid>')
        cls.locked = cls.make_key('Driftwire locked <locked@invalid>', passphrase='correct horse')
        cls.packages = make_set(cls.tmp / 'packages', '1.2.3')
        cls.site = cls.tmp / 'site'
        repo.build(cls.packages, cls.site, BASE + '/', [cls.key], cls.gnupg)
        cls.dw = cls.site / 'driftwire'

    @classmethod
    def tearDownClass(cls):
        subprocess.run(['gpgconf', '--homedir', str(cls.gnupg), '--kill', 'all'], check=False)
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.work = Path(tempfile.mkdtemp(dir=self.tmp))
        os.environ.pop(repo.PASSPHRASE_ENV, None)

    def build(self, packages=None, out=None, keys=None, **kwargs):
        out = out or self.work / 'site'
        repo.build(packages or self.packages, out, BASE, keys or [self.key], self.gnupg, **kwargs)
        return out / 'driftwire'

    def assertRefuses(self, pattern, packages=None, keys=None, base=BASE, out=None):
        out = out or self.work / 'refused'
        with self.assertRaisesRegex(ValueError, pattern):
            repo.build(packages or self.packages, out, base, keys or [self.key], self.gnupg)
        return out

    # ---- what the site holds ----

    def test_layout(self):
        files = sorted(str(p.relative_to(self.site)) for p in self.site.rglob('*') if p.is_file())
        repodata = [f for f in files if f.startswith('driftwire/rpm/repodata/')]
        self.assertEqual(sorted(f for f in files if f not in repodata), [
            'driftwire/SHA256SUMS', 'driftwire/SHA256SUMS.asc',
            'driftwire/apt/dists/stable/InRelease', 'driftwire/apt/dists/stable/Release',
            'driftwire/apt/dists/stable/Release.gpg',
            'driftwire/apt/dists/stable/main/binary-amd64/Packages',
            'driftwire/apt/dists/stable/main/binary-amd64/Packages.gz',
            'driftwire/apt/dists/stable/main/binary-arm64/Packages',
            'driftwire/apt/dists/stable/main/binary-arm64/Packages.gz',
            'driftwire/apt/pool/main/d/driftwire/driftwire_1.2.3_amd64.deb',
            'driftwire/apt/pool/main/d/driftwire/driftwire_1.2.3_arm64.deb',
            'driftwire/driftwire-1.2.3-linux-aarch64.tar.gz', 'driftwire/driftwire-1.2.3-linux-x86_64.tar.gz',
            'driftwire/driftwire-1.2.3-windows-x64.zip',
            'driftwire/driftwire.repo', 'driftwire/index.html', 'driftwire/install.ps1', 'driftwire/install.sh',
            'driftwire/key.asc', 'driftwire/key.gpg',
            'driftwire/rpm/driftwire-1.2.3-1.aarch64.rpm', 'driftwire/rpm/driftwire-1.2.3-1.x86_64.rpm',
            'index.html'])
        self.assertIn('driftwire/rpm/repodata/repomd.xml', repodata)
        self.assertIn('driftwire/rpm/repodata/repomd.xml.asc', repodata)
        self.assertTrue(any(f.endswith('-primary.xml.gz') for f in repodata))
        self.assertFalse(any(f.endswith(('.sqlite', '.zst', '.bz2', '.xz')) for f in repodata))
        # Nothing of the build left beside the site.
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir() if p.name.startswith('.driftwire-site-')), [])

    def test_one_architecture_only_and_no_windows_zip(self):
        packages = make_set(self.work / 'one', '1.2.3', ('amd64',), windows=False)
        dw = self.build(packages)
        self.assertEqual(sorted(p.name for p in (dw / 'apt/dists/stable/main').iterdir()), ['binary-amd64'])
        release = parse_release((dw / 'apt/dists/stable/Release').read_text())
        self.assertEqual(release['Architectures'], 'amd64')
        self.assertEqual(sorted(p.name for p in (dw / 'rpm').glob('*.rpm')), ['driftwire-1.2.3-1.x86_64.rpm'])
        page = (dw / 'index.html').read_text()
        self.assertIn('x86_64', page)
        self.assertNotIn('aarch64', page)
        # Without the zip: no Windows installer, and the page does not offer one.
        self.assertFalse((dw / 'install.ps1').exists())
        self.assertNotIn('install.ps1', page)
        self.assertNotIn('windows', (dw / 'SHA256SUMS').read_text())
        self.assertIn('brew install longtimeno-c/tap/driftwire', page)

    def test_release(self):
        dist = self.dw / 'apt/dists/stable'
        release = parse_release((dist / 'Release').read_text())
        for key, value in (('Origin', 'Driftwire'), ('Label', 'Driftwire'), ('Suite', 'stable'),
                           ('Codename', 'stable'), ('Components', 'main'), ('Architectures', 'amd64 arm64')):
            self.assertEqual(release[key], value, key)
        date = email.utils.parsedate_to_datetime(release['Date'])
        valid = email.utils.parsedate_to_datetime(release['Valid-Until'])
        self.assertEqual(valid - date, timedelta(days=repo.VALID_DAYS))
        listed = {name: (digest, int(size)) for digest, size, name in release['SHA256']}
        self.assertEqual(sorted(listed), [f'main/binary-{a}/{f}' for a in ('amd64', 'arm64')
                                          for f in ('Packages', 'Packages.gz')])
        for name, (digest, size) in listed.items():
            data = (dist / name).read_bytes()
            self.assertEqual((hashlib.sha256(data).hexdigest(), len(data)), (digest, size), name)

    def test_packages_indexes(self):
        for arch in ('amd64', 'arm64'):
            folder = self.dw / f'apt/dists/stable/main/binary-{arch}'
            index = (folder / 'Packages').read_bytes()
            self.assertEqual(gzip.decompress((folder / 'Packages.gz').read_bytes()), index)
            fields = dict(line.split(': ', 1) for line in index.decode().splitlines() if ': ' in line)
            self.assertEqual((fields['Package'], fields['Version'], fields['Architecture']), ('driftwire', '1.2.3', arch))
            deb = self.dw / 'apt' / fields['Filename']
            self.assertEqual(fields['Filename'], f'pool/main/d/driftwire/driftwire_1.2.3_{arch}.deb')
            self.assertEqual(fields['SHA256'], hashlib.sha256(deb.read_bytes()).hexdigest())
            self.assertEqual(deb.read_bytes(), (self.packages / deb.name).read_bytes())

    def test_signatures(self):
        keyring = self.dw / 'key.gpg'
        signed = [(self.dw / 'apt/dists/stable/InRelease',),
                  (self.dw / 'apt/dists/stable/Release.gpg', self.dw / 'apt/dists/stable/Release'),
                  (self.dw / 'rpm/repodata/repomd.xml.asc', self.dw / 'rpm/repodata/repomd.xml'),
                  (self.dw / 'SHA256SUMS.asc', self.dw / 'SHA256SUMS')]
        other = self.work / 'other.gpg'
        other.write_bytes(run('gpg', '--homedir', self.gnupg, '--batch', '--export', self.key2).stdout)
        for files in signed:
            run('gpgv', '--keyring', keyring, *files)
            self.assertNotEqual(subprocess.run(['gpgv', '--keyring', str(other), *map(str, files)],
                                               capture_output=True).returncode, 0, files[0].name)
        # A changed byte under a signature does not verify.
        changed = self.work / 'Release'
        changed.write_bytes((self.dw / 'apt/dists/stable/Release').read_bytes().replace(b'stable', b'stablf', 1))
        self.assertNotEqual(subprocess.run(['gpgv', '--keyring', str(keyring),
                                            str(self.dw / 'apt/dists/stable/Release.gpg'), str(changed)],
                                           capture_output=True).returncode, 0)
        # InRelease holds exactly Release.
        out = self.work / 'inrelease'
        run('gpgv', '--keyring', keyring, '--output', out, self.dw / 'apt/dists/stable/InRelease')
        self.assertEqual(out.read_bytes(), (self.dw / 'apt/dists/stable/Release').read_bytes())

    def test_rpms_signed(self):
        db = self.work / 'rpmdb'
        db.mkdir()
        for rpm in sorted((self.dw / 'rpm').glob('*.rpm')):
            before = subprocess.run(['rpmkeys', '--dbpath', str(db), '--checksig', str(rpm)],
                                    capture_output=True, text=True)
            self.assertIn('SIGNATURES NOT OK', before.stdout)
        run('rpmkeys', '--dbpath', db, '--import', self.dw / 'key.asc')
        for rpm in sorted((self.dw / 'rpm').glob('*.rpm')):
            out = run('rpmkeys', '--dbpath', db, '--checksig', rpm, text=True).stdout
            self.assertTrue(out.strip().endswith(': digests signatures OK'), out)
        self.assertIn('<revision>1.2.3</revision>', (self.dw / 'rpm/repodata/repomd.xml').read_text())

    def test_repo_file(self):
        self.assertEqual((self.dw / 'driftwire.repo').read_text(), (
            '[driftwire]\nname=Driftwire\n'
            f'baseurl={BASE}/rpm/\nenabled=1\ngpgcheck=1\nrepo_gpgcheck=1\ngpgkey={BASE}/key.asc\ntype=rpm-md\n'))

    def test_install_sh(self):
        script = (self.dw / 'install.sh').read_text()
        self.assertIn(f"DEFAULT_URL='{BASE}'\n", script)
        self.assertIn(f"KEY_FINGERPRINTS='{self.key}'\n", script)
        self.assertIn("BREW_TAP='longtimeno-c/tap'\n", script)
        self.assertIn("RELEASES_URL='https://github.com/longtimeno-c/mediaviewer/releases/latest'\n", script)
        self.assertNotIn('@DRIFTWIRE_', script)
        self.assertTrue(script.rstrip().endswith('\nmain "$@"'))
        run('sh', '-n', self.dw / 'install.sh')
        if shutil.which('shellcheck'):
            run('shellcheck', '-s', 'sh', self.dw / 'install.sh')
        # The installer and the site agree on the .repo file.
        body = re.search(r"rpm_repo_text\(\) \{\n  cat <<EOF\n(.*?)EOF\n", script, re.S)[1]
        self.assertEqual(body.replace('$URL', BASE), (self.dw / 'driftwire.repo').read_text())

    def test_install_sh_refuses_before_doing_anything(self):
        script = str(self.dw / 'install.sh')

        def installer(*args, **env):
            return subprocess.run(['sh', script, *args], capture_output=True, text=True,
                                  env={**os.environ, **env})

        result = installer('--help')
        self.assertEqual(result.returncode, 0)
        self.assertIn('--uninstall', result.stdout)
        result = installer('--yes')
        self.assertEqual(result.returncode, 1)
        self.assertIn('unknown option: --yes', result.stderr)
        for url in ('http://example.com/driftwire', 'ftp://example.com/x', "https://example.com/a'b",
                    'https://example.com/a b', 'https://example.com/$(id)'):
            result = installer(DRIFTWIRE_REPO_URL=url)
            self.assertEqual(result.returncode, 1, url)
            self.assertRegex(result.stderr, r'must start with https://|a character it may not have', url)
        result = subprocess.run(['sh', str(repo.TEMPLATES / 'install.sh')], capture_output=True, text=True,
                                env={k: v for k, v in os.environ.items() if k != 'DRIFTWIRE_REPO_URL'})
        self.assertEqual(result.returncode, 1)
        self.assertIn("installer's template", result.stderr)

    def test_install_sh_on_macos_names_homebrew_and_installs_nothing(self):
        # The Mac's channel is the Homebrew tap: install.sh says so and exits non-zero, with and
        # without Homebrew, and --uninstall points at brew too. uname is faked; nothing is run as root.
        bin_dir = self.work / 'bin'
        bin_dir.mkdir()
        (bin_dir / 'uname').write_text('#!/bin/sh\n[ "$1" = -s ] && echo Darwin || echo arm64\n')
        for tool in ('sh', 'cat', 'printf'):
            if shutil.which(tool):
                (bin_dir / tool).symlink_to(shutil.which(tool))
        for path in bin_dir.iterdir():
            os.chmod(path, 0o755)
        dw = self.build(github_repo='someone/fork')

        def installer(*args):
            return subprocess.run([shutil.which('sh'), str(dw / 'install.sh'), *args], capture_output=True,
                                  text=True, env={'PATH': str(bin_dir), 'HOME': str(self.work)})

        result = installer()
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('brew install someone/tap/driftwire', result.stdout)
        self.assertIn('https://brew.sh', result.stdout)
        self.assertIn('https://github.com/someone/fork/releases/latest', result.stdout)
        self.assertNotIn('+ ', result.stdout)   # no command was run
        (bin_dir / 'brew').write_text('#!/bin/sh\nexit 0\n')
        os.chmod(bin_dir / 'brew', 0o755)
        result = installer()
        self.assertEqual(result.returncode, 1)
        self.assertIn('brew install someone/tap/driftwire', result.stdout)
        self.assertNotIn('brew.sh', result.stdout)
        result = installer('--uninstall')
        self.assertEqual(result.returncode, 1)
        self.assertIn('brew uninstall driftwire', result.stdout)

    def test_refuses_a_bad_github_repo(self):
        for name in ('', 'noslash', 'a/b/c', 'a b/c', '../x', 'a/..'):
            with self.assertRaisesRegex(ValueError, 'Not a GitHub repository'):
                repo.build(self.packages, self.work / 'refused', BASE, [self.key], self.gnupg, github_repo=name)

    def test_pages(self):
        page = (self.dw / 'index.html').read_text()
        for needle in ('Version 1.2.3', f'curl -fsSL {BASE}/install.sh | sh', f'URIs: {BASE}/apt',
                       f'{BASE}/driftwire.repo', repo.grouped(self.key), 'driftwire-1.2.3-linux-x86_64.tar.gz',
                       'x86_64 and aarch64',
                       'tar -xzf driftwire-1.2.3-linux-x86_64.tar.gz --strip-components=1 -C ~/.local',
                       'brew install longtimeno-c/tap/driftwire', f'irm {BASE}/install.ps1 | iex',
                       f'&amp; ([scriptblock]::Create((irm {BASE}/install.ps1))) -Uninstall',
                       'href="driftwire-1.2.3-windows-x64.zip"'):
            self.assertIn(needle, page)
        for banned in ('<script', '<link', '<img', '<iframe', 'src=', '@import', 'url('):
            self.assertNotIn(banned, page.lower())
        self.assertEqual(page.count('SPDX-License-Identifier'), 1)   # the Windows part's header is not in it
        for href in re.findall(r'href="([^"]+)"', page):
            self.assertTrue(href == 'https://github.com/longtimeno-c/mediaviewer' or '://' not in href, href)
        self.assertIn('href="driftwire/"', (self.site / 'index.html').read_text())

    # ---- the Windows installer ----

    def test_install_ps1(self):
        script = (self.dw / 'install.ps1').read_text(encoding='ascii')
        digest = hashlib.sha256((self.dw / 'driftwire-1.2.3-windows-x64.zip').read_bytes()).hexdigest()
        for line in (f"$defaultUrl = '{BASE}'", "$version = '1.2.3'", "$zipName = 'driftwire-1.2.3-windows-x64.zip'",
                     f"$zipSha256 = '{digest}'", "$signed = 'no' -eq 'yes'", "$brewTap = 'longtimeno-c/tap'"):
            self.assertIn(f'\n    {line}\n', script)
        self.assertNotIn('@DRIFTWIRE_', script)
        self.assertTrue(script.rstrip().endswith('\nInstall-Driftwire -Remove:$Uninstall -Base $BaseUrl'))
        self.assertIn(f'{digest}  driftwire-1.2.3-windows-x64.zip\n', (self.dw / 'SHA256SUMS').read_text())
        self.assertEqual((self.dw / 'driftwire-1.2.3-windows-x64.zip').read_bytes(),
                         (self.packages / 'driftwire-1.2.3-windows-x64.zip').read_bytes())
        if PWSH:
            errors = subprocess.run([PWSH, '-NoProfile', '-Command',
                                     '$t = $null; $e = $null; [void][System.Management.Automation.Language.Parser]::'
                                     f'ParseFile("{self.dw / "install.ps1"}", [ref]$t, [ref]$e); $e.Count'],
                                    capture_output=True, text=True, check=True).stdout.strip()
            self.assertEqual(errors, '0')

    # Windows PowerShell 5.1's own commands that live in script modules (Microsoft.PowerShell.Archive,
    # and Get-FileHash and the like in Microsoft.PowerShell.Utility.psm1) do not load under a Windows
    # client's default policy, Restricted, which still runs `irm | iex`: install.ps1 calls only these.
    BINARY_COMMANDS = {'Add-Type', 'Get-AuthenticodeSignature', 'Get-ChildItem', 'Get-Variable', 'Invoke-WebRequest',
                       'Join-Path', 'Move-Item', 'New-Item', 'Out-Null', 'Remove-Item', 'Set-StrictMode', 'Test-Path',
                       'Where-Object', 'Write-Host'}

    @unittest.skipIf(not PWSH, 'needs PowerShell (pwsh)')
    def test_install_ps1_calls_no_script_module(self):
        listed = subprocess.run(
            [PWSH, '-NoProfile', '-Command',
             '$t = $null; $e = $null; $a = [System.Management.Automation.Language.Parser]::'
             f'ParseFile("{self.dw / "install.ps1"}", [ref]$t, [ref]$e); '
             '$a.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true) | '
             'ForEach-Object { $_.GetCommandName() }; '
             "'--'; "
             '$a.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) | '
             'ForEach-Object { $_.Name }'],
            capture_output=True, text=True, check=True).stdout.split('--\n')
        called, defined = set(listed[0].split()), set(listed[1].split())
        self.assertIn('Get-AuthenticodeSignature', called)
        test_mode_only = {'chmod', 'sh'}
        self.assertEqual(called - defined - test_mode_only - self.BINARY_COMMANDS, set())

    def test_a_signed_zip_is_baked_as_signed_and_can_be_required(self):
        packages = make_set(self.work / 'signed', '1.2.3', ('amd64',), windows=False)
        make_zip(packages, '1.2.3', signed=True)
        dw = self.build(packages, require_windows_signature=True)
        self.assertIn("\n    $signed = 'yes' -eq 'yes'\n", (dw / 'install.ps1').read_text())
        unsigned = make_set(self.work / 'unsigned', '1.2.3', ('amd64',))
        with self.assertRaisesRegex(ValueError, 'driftwire.exe is not signed'):
            self.build(unsigned, out=self.work / 'refused', require_windows_signature=True)
        none = make_set(self.work / 'none', '1.2.3', ('amd64',), windows=False)
        with self.assertRaisesRegex(ValueError, 'there is no Windows zip'):
            self.build(none, out=self.work / 'refused', require_windows_signature=True)

    def test_refuses_a_bad_windows_zip(self):
        def refused(pattern, **zip_args):
            packages = make_set(self.work / f'z{len(list(self.work.iterdir()))}', '1.2.3', ('amd64',), windows=False)
            make_zip(packages, zip_args.pop('version', '1.2.3'), **zip_args)
            self.assertFalse(self.assertRefuses(pattern, packages).exists())
        refused('versions disagree: 1.2.3, 1.2.4', version='1.2.4')
        refused('is not a plain file or folder under', extra=[('driftwire-1.2.3-windows-x64/../evil.exe', 'x')])
        refused('is not a plain file or folder under', extra=[('other/readme.txt', 'x')])
        refused('driftwire.exe is not x64', exe=make_pe(['KERNEL32.dll'], machine=0x14C, pe32=True))
        refused('driftwire.exe is not a PE file', exe=b'#!/bin/sh\necho hi\n')
        packages = make_set(self.work / 'nozip', '1.2.3', ('amd64',), windows=False)
        (packages / 'driftwire-1.2.3-windows-x64.zip').write_bytes(b'not a zip')
        self.assertRefuses('is not a zip', packages)

    def test_install_ps1_must_match_the_zip_beside_it(self):
        dw = self.build(out=self.work / 'site')
        repo.check_install_ps1(dw)
        (dw / 'driftwire-1.2.3-windows-x64.zip').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, "does not have driftwire-1.2.3-windows-x64.zip's SHA-256"):
            repo.check_install_ps1(dw)
        (dw / 'driftwire-1.2.3-windows-x64.zip').unlink()
        with self.assertRaisesRegex(ValueError, 'do not go together'):
            repo.check_install_ps1(dw)

    def test_windows_site_alone(self):
        packages = self.work / 'w'
        packages.mkdir()
        zip_path = make_zip(packages, '1.2.3')
        out = self.work / 'wsite'
        self.assertEqual(repo.windows_site(zip_path, out, 'http://127.0.0.1:8000/driftwire'), ('1.2.3', False))
        self.assertEqual(sorted(str(p.relative_to(out)) for p in out.rglob('*') if p.is_file()),
                         ['driftwire/driftwire-1.2.3-windows-x64.zip', 'driftwire/install.ps1'])
        self.assertIn("$defaultUrl = 'http://127.0.0.1:8000/driftwire'", (out / 'driftwire/install.ps1').read_text())
        repo.windows_site(zip_path, out, 'http://127.0.0.1:8000/driftwire')   # its own: replaced
        with self.assertRaisesRegex(ValueError, 'is not signed'):
            repo.windows_site(zip_path, self.work / 'w2', 'http://127.0.0.1:8000/driftwire', require_signature=True)
        precious = self.work / 'precious-w'
        precious.mkdir()
        (precious / 'notes.txt').write_text('mine')
        with self.assertRaisesRegex(ValueError, 'not a site this built'):
            repo.windows_site(zip_path, precious, 'http://127.0.0.1:8000/driftwire')

    @unittest.skipIf(not PWSH, 'needs PowerShell (pwsh)')
    def test_install_ps1_refuses_off_windows(self):
        result = subprocess.run([PWSH, '-NoProfile', '-NonInteractive', '-File', str(self.dw / 'install.ps1')],
                                capture_output=True, text=True,
                                env={k: v for k, v in os.environ.items() if not k.startswith('DRIFTWIRE_')})
        self.assertNotEqual(result.returncode, 0)
        out = result.stdout + result.stderr
        self.assertIn('this installer is for Windows', out)
        self.assertIn('brew install longtimeno-c/tap/driftwire', out)
        self.assertIn(f'curl -fsSL {BASE}/install.sh | sh', out)

    @unittest.skipIf(not PWSH, 'needs PowerShell (pwsh)')
    def test_install_ps1_end_to_end_in_test_mode(self):
        # check-driftwire-install.ps1, as CI's Windows leg runs it, in install.ps1's test mode:
        # installs (irm | iex), runs from a new process's PATH, updates, refuses a changed zip,
        # does nothing when cut off, uninstalls and keeps the user's other PATH entries.
        packages = self.work / 'e2e'
        packages.mkdir()
        zip_path = make_zip(packages, '1.2.3')
        newer_zip = make_zip(packages, '1.2.4')
        signed = self.work / 'e2e-signed'
        signed.mkdir()
        signed_zip = make_zip(signed, '1.2.3', signed=True)
        result = subprocess.run([PWSH, '-NoProfile', '-NonInteractive', '-File',
                                 str(Path(__file__).with_name('check-driftwire-install.ps1')),
                                 '-Zip', str(zip_path), '-NewerZip', str(newer_zip), '-SignedZip', str(signed_zip),
                                 '-Python', sys.executable],
                                capture_output=True, text=True, timeout=600,
                                env={k: v for k, v in os.environ.items() if not k.startswith('DRIFTWIRE_')})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        checks = [line for line in result.stdout.splitlines() if line.startswith('check: ') and line.endswith(': yes')]
        self.assertGreaterEqual(len(checks), 20, result.stdout)
        self.assertIn("check: the newer site's installer updates it to 1.2.4, still once on the PATH: yes", checks)
        self.assertIn('check: the signed build installs (test mode: Authenticode): yes', checks)
        self.assertIn('check: folders it did not make are kept (Driftwire.old-mine, .driftwire-cache, '
                      'Driftwire.old-0123abcd9): yes', checks)
        self.assertIn('check: and the folders beside it that it did not make: yes', checks)
        self.assertTrue(any(line.startswith('check: no old copy or unpacked download left beside the install '
                                            '(Driftwire.old-0123abcd, .driftwire-89abcdef gone)') for line in checks))
        self.assertIn('installs, updates, refuses a changed zip and uninstalls', result.stdout)

    def test_no_private_key_material(self):
        for path in self.site.rglob('*'):
            if path.is_file():
                self.assertNotIn(b'PRIVATE KEY', path.read_bytes(), path)
        packets = run('gpg', '--homedir', self.gnupg, '--batch', '--list-packets', self.dw / 'key.gpg', text=True).stdout
        self.assertIn(':public key packet:', packets)
        self.assertNotIn('secret', packets.lower())
        self.assertIn('BEGIN PGP PUBLIC KEY BLOCK', (self.dw / 'key.asc').read_text())

    def test_deterministic_but_for_dates_and_signatures(self):
        dw = self.build()
        varying = {'apt/dists/stable/Release', 'apt/dists/stable/InRelease', 'apt/dists/stable/Release.gpg',
                   'SHA256SUMS.asc'}
        compared = 0
        for path in sorted(self.dw.rglob('*')):
            name = str(path.relative_to(self.dw))
            if not path.is_file() or name in varying or name.startswith('rpm/'):
                continue
            self.assertEqual(path.read_bytes(), (dw / name).read_bytes(), name)
            compared += 1
        self.assertEqual(compared, 16)
        strip = re.compile(rb'^(Date|Valid-Until): .*\n', re.M)
        self.assertEqual(strip.sub(b'', (self.dw / 'apt/dists/stable/Release').read_bytes()),
                         strip.sub(b'', (dw / 'apt/dists/stable/Release').read_bytes()))

    def test_replaces_its_own_site(self):
        out = self.work / 'site'
        self.build(out=out)
        newer = make_set(self.work / 'newer', '1.2.4')
        dw = self.build(newer, out=out, valid_days=30)
        self.assertEqual(sorted(p.name for p in (dw / 'apt/pool/main/d/driftwire').iterdir()),
                         ['driftwire_1.2.4_amd64.deb', 'driftwire_1.2.4_arm64.deb'])
        self.assertIn('Version: 1.2.4', (dw / 'apt/dists/stable/main/binary-amd64/Packages').read_text())
        release = parse_release((dw / 'apt/dists/stable/Release').read_text())
        self.assertEqual(email.utils.parsedate_to_datetime(release['Valid-Until'])
                         - email.utils.parsedate_to_datetime(release['Date']), timedelta(days=30))
        self.assertEqual([p.name for p in self.work.iterdir() if p.name.startswith('.driftwire-site-')], [])

    def test_two_keys_sign_everything_twice(self):
        dw = self.build(keys=[self.key, self.key2])
        both = {self.key, self.key2}
        self.assertEqual(repo.gpgv_signers(dw / 'key.gpg', dw / 'apt/dists/stable/InRelease'), both)
        self.assertEqual(repo.gpgv_signers(dw / 'key.gpg', dw / 'rpm/repodata/repomd.xml.asc',
                                           dw / 'rpm/repodata/repomd.xml'), both)
        self.assertIn(f"KEY_FINGERPRINTS='{self.key} {self.key2}'", (dw / 'install.sh').read_text())
        sig = run('rpm', '-qp', '--qf', '%{RSAHEADER:pgpsig}', next((dw / 'rpm').glob('*.x86_64.rpm')),
                  text=True).stdout
        self.assertIn(self.key[-16:].lower(), sig.lower())

    def test_passphrase_from_the_environment(self):
        self.assertRefuses('could not sign', keys=[self.locked])
        os.environ[repo.PASSPHRASE_ENV] = 'correct horse'
        try:
            dw = self.build(keys=[self.locked])
        finally:
            os.environ.pop(repo.PASSPHRASE_ENV)
        self.assertEqual(repo.gpgv_signers(dw / 'key.gpg', dw / 'SHA256SUMS.asc', dw / 'SHA256SUMS'), {self.locked})
        db = self.work / 'rpmdb'
        db.mkdir()
        run('rpmkeys', '--dbpath', db, '--import', dw / 'key.asc')
        for rpm in (dw / 'rpm').glob('*.rpm'):
            self.assertIn('signatures OK', run('rpmkeys', '--dbpath', db, '--checksig', rpm, text=True).stdout)
        self.assertNotIn(b'correct horse', b''.join(p.read_bytes() for p in dw.rglob('*') if p.is_file()))

    # ---- refusals: nothing is written ----

    def test_refuses_versions_that_disagree(self):
        packages = make_set(self.work / 'mixed', '1.2.3', ('amd64',))
        (packages / 'driftwire-1.2.3-1.x86_64.rpm').unlink()
        make_rpm(packages, '1.2.4', 'x86_64')
        out = self.assertRefuses('versions disagree: 1.2.3, 1.2.4', packages)
        self.assertFalse(out.exists())

    def test_refuses_an_incomplete_architecture(self):
        packages = make_set(self.work / 'part', '1.2.3')
        (packages / 'driftwire-1.2.3-linux-aarch64.tar.gz').unlink()
        self.assertFalse(self.assertRefuses('arm64 has no tarball', packages).exists())

    def test_refuses_what_is_not_driftwire(self):
        packages = make_set(self.work / 'foreign', '1.2.3', ('amd64',))
        (packages / 'driftwire_1.2.3_amd64.deb').unlink()
        other = make_deb(packages, '1.2.3', 'amd64', package='other')
        self.assertRefuses('is not what its name says', packages)
        other.rename(packages / 'other_1.2.3_amd64.deb')
        self.assertRefuses('Not a driftwire package: other_1.2.3_amd64.deb', packages)

    def test_refuses_a_tarball_that_escapes(self):
        packages = make_set(self.work / 'escape', '1.2.3', ('amd64',))
        bad = packages / 'driftwire-1.2.3-linux-x86_64.tar.gz'
        with tarfile.open(bad, 'w:gz') as tar:
            info = tarfile.TarInfo('driftwire-1.2.3-linux-x86_64/../evil')
            tar.addfile(info, io.BytesIO(b''))
        self.assertRefuses('not a plain file or folder under', packages)

    def test_refuses_bad_base_urls(self):
        for url in ('http://example.com/driftwire', 'ftp://example.com/x', "https://example.com/a'b",
                    'https://example.com/a b', 'https:///x', 'https://u@example.com/x', 'https://example.com/x?y'):
            self.assertRefuses('base URL', base=url)
        self.assertEqual(repo.check_base_url('http://127.0.0.1:8000/driftwire/'), 'http://127.0.0.1:8000/driftwire')

    def test_refuses_an_unknown_key(self):
        self.assertFalse(self.assertRefuses('No secret key', keys=['nobody@invalid']).exists())

    def test_refuses_to_replace_something_else(self):
        out = self.work / 'precious'
        out.mkdir()
        (out / 'notes.txt').write_text('mine')
        self.assertRefuses('not a site this built', out=out)
        self.assertEqual((out / 'notes.txt').read_text(), 'mine')

    def test_a_failed_build_leaves_the_old_site(self):
        out = self.work / 'site'
        self.build(out=out)
        before = {str(p.relative_to(out)): p.read_bytes() for p in out.rglob('*') if p.is_file()}
        self.assertRefuses('could not sign', keys=[self.locked], out=out)
        after = {str(p.relative_to(out)): p.read_bytes() for p in out.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        self.assertEqual([p.name for p in self.work.iterdir() if p.name.startswith('.driftwire-site-')], [])


class PageTarballExampleTest(unittest.TestCase):
    """The page's tar command names the tarball most readers can run: x86_64 when the site has
    it (sorted, aarch64 comes first), else the one there is. No tools needed."""

    def page(self, rpm_arches):
        tmp = Path(tempfile.mkdtemp(prefix='dwpage-'))
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        tarballs = {a: tmp / f'driftwire-1.2.3-linux-{a}.tar.gz' for a in rpm_arches}
        debs = {d: tmp / f'driftwire_1.2.3_{d}.deb' for d, r in repo.ARCHES.items() if r in rpm_arches}
        packages = repo.Packages('1.2.3', debs, {}, tarballs)
        signer = type('Signer', (), {'fingerprints': ['0123456789ABCDEF0123456789ABCDEF01234567']})()
        repo.build_pages(tmp / 'site', tmp / 'site/driftwire', packages, BASE, signer)
        page = (tmp / 'site/driftwire/index.html').read_text()
        return re.search(r'tar -xzf (\S+) --strip-components=1', page).group(1)

    def test_x86_64_when_present(self):
        self.assertEqual(self.page(('aarch64', 'x86_64')), 'driftwire-1.2.3-linux-x86_64.tar.gz')

    def test_the_only_one_otherwise(self):
        self.assertEqual(self.page(('aarch64',)), 'driftwire-1.2.3-linux-aarch64.tar.gz')


if __name__ == '__main__':
    unittest.main()
