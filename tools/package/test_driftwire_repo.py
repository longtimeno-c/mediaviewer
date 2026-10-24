#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for driftwire_repo.py (docs/design/28 "Installing"): the site's layout, the apt
Release and its hashes, every signature checked with gpgv and rpmkeys, the .repo file, the
installer's baked address, the page, and each refusal. The packages are tiny ones made here
(dpkg-deb, rpmbuild, tarfile) and the keys are throwaway ones in a temporary GnuPG home. Skips
when the tools are missing (apt-utils, createrepo-c, rpm, gnupg on Debian and Ubuntu).
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
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('driftwire_repo', Path(__file__).with_name('driftwire_repo.py'))
repo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(repo)

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


def make_set(folder, version, arches=('amd64', 'arm64')):
    folder.mkdir(parents=True, exist_ok=True)
    for arch in arches:
        make_deb(folder, version, arch)
        make_rpm(folder, version, repo.ARCHES[arch])
        make_tarball(folder, version, repo.ARCHES[arch])
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
            'driftwire/driftwire.repo', 'driftwire/index.html', 'driftwire/install.sh',
            'driftwire/key.asc', 'driftwire/key.gpg',
            'driftwire/rpm/driftwire-1.2.3-1.aarch64.rpm', 'driftwire/rpm/driftwire-1.2.3-1.x86_64.rpm',
            'index.html'])
        self.assertIn('driftwire/rpm/repodata/repomd.xml', repodata)
        self.assertIn('driftwire/rpm/repodata/repomd.xml.asc', repodata)
        self.assertTrue(any(f.endswith('-primary.xml.gz') for f in repodata))
        self.assertFalse(any(f.endswith(('.sqlite', '.zst', '.bz2', '.xz')) for f in repodata))
        # Nothing of the build left beside the site.
        self.assertEqual(sorted(p.name for p in self.tmp.iterdir() if p.name.startswith('.driftwire-site-')), [])

    def test_one_architecture_only(self):
        packages = make_set(self.work / 'one', '1.2.3', ('amd64',))
        dw = self.build(packages)
        self.assertEqual(sorted(p.name for p in (dw / 'apt/dists/stable/main').iterdir()), ['binary-amd64'])
        release = parse_release((dw / 'apt/dists/stable/Release').read_text())
        self.assertEqual(release['Architectures'], 'amd64')
        self.assertEqual(sorted(p.name for p in (dw / 'rpm').glob('*.rpm')), ['driftwire-1.2.3-1.x86_64.rpm'])
        self.assertIn('x86_64', (dw / 'index.html').read_text())
        self.assertNotIn('aarch64', (dw / 'index.html').read_text())

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

    def test_pages(self):
        page = (self.dw / 'index.html').read_text()
        for needle in ('Version 1.2.3', f'curl -fsSL {BASE}/install.sh | sh', f'URIs: {BASE}/apt',
                       f'{BASE}/driftwire.repo', repo.grouped(self.key), 'driftwire-1.2.3-linux-x86_64.tar.gz',
                       'x86_64 and aarch64'):
            self.assertIn(needle, page)
        for banned in ('<script', '<link', '<img', '<iframe', 'src=', '@import', 'url('):
            self.assertNotIn(banned, page.lower())
        for href in re.findall(r'href="([^"]+)"', page):
            self.assertTrue(href == 'https://github.com/longtimeno-c/mediaviewer' or '://' not in href, href)
        self.assertIn('href="driftwire/"', (self.site / 'index.html').read_text())

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
        self.assertEqual(compared, 14)
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


if __name__ == '__main__':
    unittest.main()
