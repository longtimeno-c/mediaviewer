#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""The driftwire package repository (docs/design/28 "Installing"): signed apt and dnf
repositories, the tarballs with signed checksums, the Windows zip, and the one-line installers
for Linux and Windows, as a static site.

    driftwire_repo.py build --packages DIR --out SITE --base-url URL --key-id ID [--key-id ID]
                            [--gnupg-home DIR] [--valid-days 365] [--github-repo OWNER/NAME]
                            [--require-windows-signature]
    driftwire_repo.py smoke --site SITE
    driftwire_repo.py windows-site --zip ZIP --out SITE --base-url URL [--require-windows-signature]

`build` takes one version's .deb, .rpm and tarball for each architecture (release.yml's
linux-cli artefacts, from cmake/driftwire-cli), and the Windows zip when it is there
(tools/package/driftwire-windows.ps1), and writes, for URL = <Pages root>/driftwire:

    SITE/index.html                    links to driftwire/ (the Pages root)
    SITE/driftwire/index.html          what driftwire is and how to install it
    SITE/driftwire/install.sh          the installer, URL and the key's fingerprint baked in (on
                                       macOS it names the Homebrew tap of --github-repo's owner)
    SITE/driftwire/key.asc, key.gpg    the public key, armored and binary
    SITE/driftwire/apt/                pool/main/d/driftwire/*.deb, dists/stable/{Release,InRelease,
                                       Release.gpg}, dists/stable/main/binary-<arch>/Packages{,.gz}
    SITE/driftwire/rpm/                the signed .rpm files, repodata/ and repodata/repomd.xml.asc
    SITE/driftwire/driftwire.repo      for dnf, yum and zypper
    SITE/driftwire/driftwire-<version>-linux-<arch>.tar.gz, SHA256SUMS, SHA256SUMS.asc
    SITE/driftwire/driftwire-<version>-windows-x64.zip (in SHA256SUMS) and install.ps1, the
                                       Windows installer, with the zip's SHA-256 baked in

install.ps1 takes the zip from the site, not from the release page: the installer, the zip and
its checksum then go live together in one deployment, and a copy of the site served elsewhere
(DRIFTWIRE_REPO_URL, the tests) is whole. Whether driftwire.exe carries an Authenticode
signature is read from the file and baked in, and install.ps1 then insists on valid ones;
--require-windows-signature refuses an unsigned zip (a release with Trusted Signing).

It signs with the secret keys named by --key-id in the GnuPG home (--gnupg-home or GNUPGHOME).
A passphrase, when a key has one, comes from DRIFTWIRE_REPO_GPG_PASSPHRASE, never an argument.
Two keys sign everything twice (a key change, DEVELOPMENT "Linux package repository"); the
.rpm files carry the first one's signature (rpm keeps one). Every signature is checked against
the exported public key with gpgv and rpmkeys before the new site replaces SITE, so nothing
unsigned is ever written there, and the tree is checked for private key material. Apart from
the dates and the signatures, the same packages and key give the same site.

`windows-site` writes only install.ps1 and the zip into SITE/driftwire (no key, no Linux
packages): what tools/package/check-driftwire-install.ps1 serves on CI's Windows leg.

`smoke` serves SITE on 127.0.0.1 and checks it as a person would get it, on a Debian or Ubuntu
machine without driftwire: install.sh run as `curl | sh` runs it (DRIFTWIRE_REPO_URL pointing at
the local copy) installs driftwire with apt, `driftwire --version` names the site's version, apt
refuses an InRelease changed by one byte, --uninstall takes the package, the source, the key and
apt's lists of it away; then the tarball route, with apt-get, dnf, yum and zypper hidden, into a
scratch home as an unprivileged user, run and removed. It uses sudo when not root.
"""
import argparse
import functools
import gzip
import hashlib
import html
import http.server
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TEMPLATES = ROOT / 'packaging' / 'linux' / 'repo'
WINDOWS_TEMPLATES = ROOT / 'packaging' / 'windows'

# Debian architecture → the RPM's and the tarball's (cmake/driftwire-cli's CPack names).
ARCHES = {'amd64': 'x86_64', 'arm64': 'aarch64'}
DEB_ARCH = {rpm: deb for deb, rpm in ARCHES.items()}
VERSION = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
DEB_FILE = re.compile(rf'driftwire_(?P<version>{VERSION})_(?P<arch>amd64|arm64)\.deb')
RPM_FILE = re.compile(rf'driftwire-(?P<version>{VERSION})-(?P<release>[1-9][0-9]*)\.(?P<arch>x86_64|aarch64)\.rpm')
TAR_FILE = re.compile(rf'driftwire-(?P<version>{VERSION})-linux-(?P<arch>x86_64|aarch64)\.tar\.gz')
ZIP_FILE = re.compile(rf'driftwire-(?P<version>{VERSION})-windows-x64\.zip')
# install.sh accepts the same: https, or http on this machine for tests.
URL_CHARS = re.compile(r'[A-Za-z0-9._~:/%+-]+')
# The GitHub repository whose releases carry the command line; its owner's tap
# (<owner>/homebrew-tap, `brew install <owner>/tap/driftwire`) is the Mac's channel.
GITHUB_REPO = 'longtimeno-c/mediaviewer'
GITHUB_REPO_NAME = re.compile(r'[A-Za-z0-9-]+/[A-Za-z0-9._-]+')

TOOLS = ('apt-ftparchive', 'createrepo_c', 'gpg', 'gpgv', 'rpmsign', 'rpmkeys', 'rpm', 'dpkg-deb')

# Valid-Until: an apt client refuses a Release older than this, so a mirror or a network in
# the way cannot keep serving an old, correctly signed index to hold back an update for longer
# than a year. The cost: a stable release (which re-signs the site) must come within that year,
# or `apt update` on every installed machine fails ("Release file ... is expired", exit 100)
# and takes nothing new from the repository until the next release re-signs it.
VALID_DAYS = 365

PASSPHRASE_ENV = 'DRIFTWIRE_REPO_GPG_PASSPHRASE'


def run(args, **kwargs):
    kwargs.setdefault('check', True)
    kwargs.setdefault('capture_output', True)
    return subprocess.run([str(a) for a in args], **kwargs)


# ---- the packages ------------------------------------------------------------------------

class Packages:
    def __init__(self, version, debs, rpms, tarballs, windows_zip=None, windows_signed=False):
        self.version = version
        self.debs = debs            # Debian arch → path
        self.rpms = rpms            # RPM arch → path
        self.tarballs = tarballs    # RPM arch → path
        self.windows_zip = windows_zip
        self.windows_signed = windows_signed

    @property
    def deb_arches(self):
        return sorted(self.debs)


def deb_fields(path):
    out = run(['dpkg-deb', '--field', path, 'Package', 'Version', 'Architecture'], text=True).stdout
    return dict(line.split(': ', 1) for line in out.splitlines() if ': ' in line)


def rpm_fields(path):
    return run(['rpm', '-qp', '--nosignature', '--nodigest', '--qf',
                '%{NAME} %{VERSION} %{RELEASE} %{ARCH}', path], text=True).stdout.split()


def check_tarball(path, version, arch):
    top = f'driftwire-{version}-linux-{arch}'
    with tarfile.open(path, 'r:gz') as tar:
        members = tar.getmembers()
    names = set()
    for member in members:
        name = member.name.rstrip('/')
        parts = name.split('/')
        if parts[0] != top or '..' in parts or name.startswith('/') or not (member.isfile() or member.isdir()):
            raise ValueError(f'{path.name}: {member.name!r} is not a plain file or folder under {top}/')
        names.add(name)
    if f'{top}/bin/driftwire' not in names:
        raise ValueError(f'{path.name} has no {top}/bin/driftwire')


def pe_facts(data, name):
    """(machine, signed) of a PE file: its COFF machine, and whether its certificate table
    (data directory 4, where Authenticode keeps the signature) holds anything."""
    def u16(at):
        return struct.unpack_from('<H', data, at)[0]

    def u32(at):
        return struct.unpack_from('<I', data, at)[0]
    try:
        if data[:2] != b'MZ' or data[u32(0x3C):u32(0x3C) + 4] != b'PE\0\0':
            raise ValueError
        pe = u32(0x3C)
        opt = pe + 24
        dirs = {0x20B: opt + 112, 0x10B: opt + 96}[u16(opt)]
        signed = False
        if u32(dirs - 4) > 4:
            offset, size = struct.unpack_from('<II', data, dirs + 8 * 4)
            signed = size > 8 and offset + size <= len(data)
        return u16(pe + 4), signed
    except (ValueError, KeyError, struct.error):
        raise ValueError(f'{name} is not a PE file') from None


def check_windows_zip(path, version):
    """The zip driftwire-windows.ps1 packs: one folder, driftwire.exe (x64) in it. Returns
    whether driftwire.exe is Authenticode-signed."""
    top = f'driftwire-{version}-windows-x64'
    try:
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            for name in names:
                parts = name.rstrip('/').split('/')
                if parts[0] != top or '..' in parts or name.startswith('/') or '\\' in name or ':' in name:
                    raise ValueError(f'{path.name}: {name!r} is not a plain file or folder under {top}/')
            if f'{top}/driftwire.exe' not in names:
                raise ValueError(f'{path.name} has no {top}/driftwire.exe')
            exe = z.read(f'{top}/driftwire.exe')
    except zipfile.BadZipFile:
        raise ValueError(f'{path.name} is not a zip') from None
    machine, signed = pe_facts(exe, f'{path.name}: driftwire.exe')
    if machine != 0x8664:
        raise ValueError(f'{path.name}: driftwire.exe is not x64 (machine {machine:#06x})')
    return signed


def find_packages(folder):
    folder = Path(folder)
    if not folder.is_dir():
        raise ValueError(f'No packages folder: {folder}')
    debs, rpms, tarballs, versions = {}, {}, {}, set()
    windows_zip, windows_signed = None, False

    def add(kind, arch, path):
        if arch in kind:
            raise ValueError(f'Two packages for {arch}: {kind[arch].name} and {path.name}')
        kind[arch] = path

    for path in sorted(folder.iterdir()):
        name = path.name
        if not path.is_file() or not name.endswith(('.deb', '.rpm', '.tar.gz', '.zip')):
            continue
        if m := DEB_FILE.fullmatch(name):
            want = {'Package': 'driftwire', 'Version': m['version'], 'Architecture': m['arch']}
            if deb_fields(path) != want:
                raise ValueError(f'{name} is not what its name says: {deb_fields(path)}')
            add(debs, m['arch'], path)
        elif m := RPM_FILE.fullmatch(name):
            if rpm_fields(path) != ['driftwire', m['version'], m['release'], m['arch']]:
                raise ValueError(f'{name} is not what its name says: {" ".join(rpm_fields(path))}')
            add(rpms, m['arch'], path)
        elif m := TAR_FILE.fullmatch(name):
            check_tarball(path, m['version'], m['arch'])
            add(tarballs, m['arch'], path)
        elif m := ZIP_FILE.fullmatch(name):
            windows_signed = check_windows_zip(path, m['version'])
            windows_zip = path
        else:
            raise ValueError(f'Not a driftwire package: {name}')
        versions.add(m['version'])
    if not versions:
        raise ValueError(f'No driftwire packages in {folder}')
    if len(versions) != 1:
        raise ValueError("The packages' versions disagree: " + ', '.join(sorted(versions)))
    arches = set(debs) | {DEB_ARCH[a] for a in rpms} | {DEB_ARCH[a] for a in tarballs}
    for arch in sorted(arches):
        missing = [kind for kind, present in (('.deb', arch in debs), ('.rpm', ARCHES[arch] in rpms),
                                              ('tarball', ARCHES[arch] in tarballs)) if not present]
        if missing:
            raise ValueError(f'{arch} has no {" or ".join(missing)}: each architecture needs all three')
    return Packages(versions.pop(), debs, rpms, tarballs, windows_zip, windows_signed)


def check_base_url(url):
    url = url.rstrip('/')
    parts = urllib.parse.urlsplit(url)
    if not URL_CHARS.fullmatch(url) or parts.query or parts.fragment or parts.username or not parts.hostname:
        raise ValueError(f'Not a usable base URL: {url!r}')
    if parts.scheme != 'https' and not (parts.scheme == 'http' and parts.hostname in ('127.0.0.1', 'localhost')):
        raise ValueError(f'The base URL must be https:// (http:// only on 127.0.0.1 or localhost): {url}')
    return url


# ---- signing -----------------------------------------------------------------------------

class Signer:
    """gpg and rpmsign on the secret keys named, with the passphrase (if any) in a private
    file that only this run can read and that is gone when it ends."""

    def __init__(self, key_ids, gnupg_home, private_dir):
        self.env = dict(os.environ)
        if gnupg_home:
            self.env['GNUPGHOME'] = str(Path(gnupg_home).resolve())
        self.secret_args = []
        passphrase = os.environ.get(PASSPHRASE_ENV)
        if passphrase:
            if re.search(r'\s', str(private_dir)):
                raise ValueError('The temporary folder has a space in its path; rpmsign cannot take it')
            pass_file = Path(private_dir) / 'passphrase'
            fd = os.open(pass_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w', encoding='utf-8') as f:
                f.write(passphrase)
            self.secret_args = ['--passphrase-file', str(pass_file)]
        self.fingerprints = []
        for key_id in key_ids:
            fpr = self.resolve(key_id)
            if fpr in self.fingerprints:
                raise ValueError(f'The key {key_id} is named twice')
            self.fingerprints.append(fpr)
        if not self.fingerprints:
            raise ValueError('No --key-id')

    def gpg(self, *args, **kwargs):
        return run(['gpg', '--batch', '--no-tty', '--pinentry-mode', 'loopback', *args], env=self.env, **kwargs)

    def resolve(self, key_id):
        """The primary fingerprint of the one usable signing key key_id names."""
        try:
            out = self.gpg('--with-colons', '--fixed-list-mode', '--list-secret-keys', '--', key_id,
                           text=True).stdout
        except subprocess.CalledProcessError:
            raise ValueError(f'No secret key {key_id} in the GnuPG home') from None
        keys, primary = [], None
        for line in out.splitlines():
            fields = line.split(':')
            if fields[0] == 'sec':
                primary = fields
            elif fields[0] == 'fpr' and primary is not None:
                keys.append((fields[9], primary))
                primary = None
        if len(keys) != 1:
            raise ValueError(f'{key_id} names {len(keys)} secret keys; give the fingerprint')
        fpr, sec = keys[0]
        if sec[1] in ('e', 'r', 'd', 'i'):
            raise ValueError(f'The key {fpr} is expired, revoked, disabled or invalid')
        if 'S' not in sec[11]:
            raise ValueError(f'The key {fpr} cannot sign')
        return fpr

    def _sign(self, src, dst, mode):
        users = [arg for fpr in self.fingerprints for arg in ('--local-user', fpr)]
        try:
            self.gpg(*self.secret_args, '--yes', '--digest-algo', 'SHA512', *users, *mode,
                     '--output', dst, src)
        except subprocess.CalledProcessError as error:
            raise ValueError(f'gpg could not sign {Path(src).name}: '
                             + error.stderr.decode(errors='replace').strip()) from None

    def clearsign(self, src, dst):
        self._sign(src, dst, ['--clearsign'])

    def detach(self, src, dst):
        self._sign(src, dst, ['--armor', '--detach-sign'])

    def sign_rpm(self, path):
        extra = ' '.join(['--batch', '--no-tty', '--pinentry-mode', 'loopback', *self.secret_args])
        try:
            run(['rpmsign', '--addsign', '--define', f'_gpg_name {self.fingerprints[0]}',
                 '--define', f'_gpg_sign_cmd_extra_args {extra}', path], env=self.env)
        except subprocess.CalledProcessError as error:
            raise ValueError(f'rpmsign could not sign {Path(path).name}: '
                             + (error.stderr or error.stdout).decode(errors='replace').strip()) from None

    def export(self, armor):
        return self.gpg('--export', '--export-options', 'export-minimal', *(['--armor'] if armor else []),
                        *self.fingerprints).stdout


# ---- the site ------------------------------------------------------------------------------

def write(path, data, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode('utf-8'))
    os.chmod(path, mode)


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    os.chmod(dst, 0o644)


def fill(template, values):
    path = template if isinstance(template, Path) else TEMPLATES / template
    text = path.read_text(encoding='utf-8')
    for key, value in values.items():
        text = text.replace(f'@{key}@', value)
    left = sorted(set(re.findall(r'@[A-Z][A-Z_]+@', text)))
    if left:
        raise ValueError(f'{path.name}: nothing to fill {", ".join(left)} with')
    return text


def install_ps1_text(base, version, zip_path, signed, github_repo):
    """install.ps1 for this zip, served from base. ASCII only (it is read as whatever charset
    the server names)."""
    text = fill(WINDOWS_TEMPLATES / 'install.ps1', {
        'DRIFTWIRE_REPO_URL': base, 'DRIFTWIRE_VERSION': version, 'DRIFTWIRE_WINDOWS_ZIP': zip_path.name,
        'DRIFTWIRE_WINDOWS_SHA256': hashlib.sha256(zip_path.read_bytes()).hexdigest(),
        'DRIFTWIRE_WINDOWS_SIGNED': 'yes' if signed else 'no',
        'DRIFTWIRE_BREW_TAP': github_repo.split('/')[0] + '/tap'})
    if not text.isascii():
        raise ValueError('install.ps1 is not ASCII')
    return text


def build_windows(dw, packages, base, github_repo, require_signature):
    """The zip and install.ps1; the zip's line of SHA256SUMS."""
    if not packages.windows_zip:
        if require_signature:
            raise ValueError('--require-windows-signature, and there is no Windows zip')
        return []
    if require_signature and not packages.windows_signed:
        raise ValueError(f'{packages.windows_zip.name}: driftwire.exe is not signed (--require-windows-signature)')
    target = dw / packages.windows_zip.name
    copy(packages.windows_zip, target)
    write(dw / 'install.ps1', install_ps1_text(base, packages.version, target, packages.windows_signed, github_repo))
    return [target]


def rpm_repo_text(base):
    # install.sh writes the same file (rpm_repo_text) for the address it installs from.
    return (f'[driftwire]\nname=Driftwire\nbaseurl={base}/rpm/\nenabled=1\ngpgcheck=1\n'
            f'repo_gpgcheck=1\ngpgkey={base}/key.asc\ntype=rpm-md\n')


def build_apt(dw, packages, signer, valid_days):
    apt = dw / 'apt'
    for arch in packages.deb_arches:
        copy(packages.debs[arch], apt / 'pool/main/d/driftwire' / packages.debs[arch].name)
    dist = apt / 'dists/stable'
    for arch in packages.deb_arches:
        index = run(['apt-ftparchive', '--arch', arch, 'packages', 'pool/main/d/driftwire'], cwd=apt).stdout
        text = index.decode('utf-8')
        if text.count('Package: ') != 1 or 'Package: driftwire\n' not in text or f'Architecture: {arch}\n' not in text:
            raise ValueError(f'apt-ftparchive gave an unexpected index for {arch}')
        write(dist / f'main/binary-{arch}/Packages', index)
        write(dist / f'main/binary-{arch}/Packages.gz', gzip.compress(index, compresslevel=9, mtime=0))
    fields = {'Origin': 'Driftwire', 'Label': 'Driftwire', 'Suite': 'stable', 'Codename': 'stable',
              'Components': 'main', 'Architectures': ' '.join(packages.deb_arches),
              'Description': 'driftwire, the Driftwire command line',
              'ValidTime': str(valid_days * 86400)}
    options = [arg for key, value in fields.items() for arg in ('-o', f'APT::FTPArchive::Release::{key}={value}')]
    release = run(['apt-ftparchive', *options, 'release', '.'], cwd=dist).stdout
    if not re.search(rb'^Valid-Until: ', release, re.M):
        raise ValueError('apt-ftparchive wrote no Valid-Until (APT::FTPArchive::Release::ValidTime)')
    write(dist / 'Release', release)
    signer.clearsign(dist / 'Release', dist / 'InRelease')
    signer.detach(dist / 'Release', dist / 'Release.gpg')


def build_rpm(dw, packages, signer):
    rpm_dir = dw / 'rpm'
    for arch in sorted(packages.rpms):
        target = rpm_dir / packages.rpms[arch].name
        copy(packages.rpms[arch], target)
        signer.sign_rpm(target)
        os.chmod(target, 0o644)
    # gzip, not zstd, and no sqlite: older dnf and zypper read it, and it is the same each time.
    run(['createrepo_c', '--quiet', '--no-database', '--general-compress-type', 'gz',
         '--checksum', 'sha256', '--revision', packages.version, rpm_dir])
    signer.detach(rpm_dir / 'repodata/repomd.xml', rpm_dir / 'repodata/repomd.xml.asc')


def build_tarballs(dw, packages, signer, more=()):
    lines = []
    for arch in sorted(packages.tarballs):
        path = packages.tarballs[arch]
        copy(path, dw / path.name)
        lines.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n')
    for path in more:
        lines.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n')
    write(dw / 'SHA256SUMS', ''.join(lines))
    signer.detach(dw / 'SHA256SUMS', dw / 'SHA256SUMS.asc')


def grouped(fpr):
    return ' '.join(fpr[i:i + 4] for i in range(0, len(fpr), 4))


def check_github_repo(repo):
    if not GITHUB_REPO_NAME.fullmatch(repo or '') or '..' in repo:
        raise ValueError(f'Not a GitHub repository (OWNER/NAME): {repo!r}')
    return repo


def build_pages(site, dw, packages, base, signer, github_repo=GITHUB_REPO):
    arch_names = [ARCHES[a] for a in packages.deb_arches]
    tarballs = [packages.tarballs[a].name for a in sorted(packages.tarballs)]
    # Most readers are on x86_64; sorted, aarch64 would come first.
    example = packages.tarballs.get('x86_64', packages.tarballs[sorted(packages.tarballs)[0]]).name
    windows = ''
    if packages.windows_zip:
        windows = fill('index-windows.html', {'BASE_URL': html.escape(base),
                                              'WINDOWS_ZIP': html.escape(packages.windows_zip.name)})
        windows = re.sub(r'\A<!--.*?-->\n', '', windows, flags=re.S)   # its own header stays out of the page
    values = {
        'BASE_URL': html.escape(base),
        'VERSION': html.escape(packages.version),
        'ARCHES': ' and '.join(arch_names),
        'TARBALLS': '\n'.join(f'<li><a href="{html.escape(n)}">{html.escape(n)}</a></li>' for n in tarballs),
        'TARBALL_EXAMPLE': html.escape(example),
        'FINGERPRINTS': '\n'.join(grouped(f) for f in signer.fingerprints),
        'BREW_TAP': html.escape(github_repo.split('/')[0] + '/tap'),
        'GITHUB_URL': html.escape(f'https://github.com/{github_repo}'),
        'WINDOWS_SECTION': windows,
    }
    write(dw / 'index.html', fill('index.html', values))
    write(site / 'index.html',
          '<!doctype html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
          '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
          '<title>driftwire</title>\n<meta http-equiv="refresh" content="0; url=driftwire/">\n</head>\n'
          '<body>\n<p><a href="driftwire/">driftwire</a>: the Driftwire command line.</p>\n'
          '</body>\n</html>\n')
    script = fill('install.sh', {'DRIFTWIRE_REPO_URL': base, 'DRIFTWIRE_KEY_FINGERPRINTS': ' '.join(signer.fingerprints),
                                 'DRIFTWIRE_BREW_TAP': github_repo.split('/')[0] + '/tap',
                                 'DRIFTWIRE_RELEASES_URL': f'https://github.com/{github_repo}/releases/latest'})
    write(dw / 'install.sh', script)


def gpgv_signers(keyring, *files, output=None):
    """The primary fingerprints whose good signatures gpgv finds on the files."""
    args = ['gpgv', '--status-fd', '1', '--keyring', keyring]
    if output is not None:
        args += ['--output', output]
    result = run([*args, *files], check=False, text=True)
    if result.returncode != 0:
        raise ValueError(f'gpgv does not accept {Path(files[0]).name}: {result.stderr.strip()}')
    return {line.split()[-1] for line in result.stdout.splitlines() if line.startswith('[GNUPG:] VALIDSIG ')}


def verify(dw, signer, work):
    """Everything signed is checked against the published key before the site goes out."""
    keyring = dw / 'key.gpg'
    shown = signer.gpg('--with-colons', '--show-keys', keyring, text=True).stdout
    published = [f.split(':')[9] for f in shown.splitlines() if f.startswith('fpr:')]
    if not set(signer.fingerprints) <= set(published):
        raise ValueError('key.gpg does not hold the signing keys')
    packets = signer.gpg('--list-packets', keyring, text=True).stdout
    if 'secret' in packets.lower():
        raise ValueError('key.gpg holds secret key material')
    want = set(signer.fingerprints)
    dist = dw / 'apt/dists/stable'
    checks = [(dist / 'InRelease',), (dist / 'Release.gpg', dist / 'Release'),
              (dw / 'rpm/repodata/repomd.xml.asc', dw / 'rpm/repodata/repomd.xml'),
              (dw / 'SHA256SUMS.asc', dw / 'SHA256SUMS')]
    for files in checks:
        out = work / 'signed' if len(files) == 1 else None
        if gpgv_signers(keyring, *files, output=out) != want:
            raise ValueError(f'{files[0].name} is not signed by exactly the keys given')
        if out is not None and out.read_bytes() != (dist / 'Release').read_bytes():
            raise ValueError('InRelease does not hold Release')
    db = work / 'rpmdb'
    db.mkdir()
    run(['rpmkeys', '--dbpath', db, '--import', dw / 'key.asc'])
    for path in sorted((dw / 'rpm').glob('*.rpm')):
        result = run(['rpmkeys', '--dbpath', db, '--checksig', path], check=False, text=True)
        if result.returncode != 0 or not result.stdout.strip().endswith(': digests signatures OK'):
            raise ValueError(f'{path.name} is not signed by the key: {result.stdout.strip()} {result.stderr.strip()}')
    check_install_ps1(dw)
    for path in sorted(dw.parent.rglob('*')):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError(f'{path.name} is not a plain file or folder')
        if path.is_file() and b'-----BEGIN PGP PRIVATE KEY BLOCK-----' in path.read_bytes():
            raise ValueError(f'{path.relative_to(dw.parent)} holds private key material')


def check_install_ps1(dw):
    """install.ps1, when there is one, names the zip beside it and that zip's SHA-256."""
    script = dw / 'install.ps1'
    zips = sorted(dw.glob('driftwire-*-windows-x64.zip'))
    if not script.exists() and not zips:
        return
    text = script.read_text(encoding='ascii') if script.exists() else ''
    name = re.search(r"^    \$zipName = '([^']+)'$", text, re.M)
    digest = re.search(r"^    \$zipSha256 = '([0-9a-f]{64})'$", text, re.M)
    if len(zips) != 1 or not name or not digest or name[1] != zips[0].name:
        raise ValueError('install.ps1 and the Windows zip beside it do not go together')
    if hashlib.sha256(zips[0].read_bytes()).hexdigest() != digest[1]:
        raise ValueError(f"install.ps1 does not have {zips[0].name}'s SHA-256")


def build(packages_dir, out, base_url, key_ids, gnupg_home=None, valid_days=VALID_DAYS, github_repo=GITHUB_REPO,
          require_windows_signature=False):
    missing = [tool for tool in TOOLS if not shutil.which(tool)]
    if missing:
        raise ValueError('Missing tools: ' + ', '.join(missing))
    if not 1 <= valid_days <= 3650:
        raise ValueError('--valid-days is 1 to 3650')
    base = check_base_url(base_url)
    check_github_repo(github_repo)
    out = Path(out).resolve()
    check_replaceable(out)
    packages = find_packages(packages_dir)
    if not packages.debs:
        raise ValueError(f'No Linux packages in {packages_dir}')
    out.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.driftwire-site-', dir=out.parent))
    private = Path(tempfile.mkdtemp(prefix='driftwire-repo-'))
    try:
        signer = Signer(key_ids, gnupg_home, private)
        site = stage / 'site'
        dw = site / 'driftwire'
        dw.mkdir(parents=True)
        write(dw / 'key.asc', signer.export(armor=True))
        write(dw / 'key.gpg', signer.export(armor=False))
        build_apt(dw, packages, signer, valid_days)
        build_rpm(dw, packages, signer)
        windows = build_windows(dw, packages, base, github_repo, require_windows_signature)
        build_tarballs(dw, packages, signer, windows)
        write(dw / 'driftwire.repo', rpm_repo_text(base))
        build_pages(site, dw, packages, base, signer, github_repo)
        work = stage / 'verify'
        work.mkdir()
        verify(dw, signer, work)
        for path in site.rglob('*'):
            if path.is_dir():
                os.chmod(path, 0o755)
        os.chmod(site, 0o755)
        check_replaceable(out)
        if out.exists():
            out.rename(stage / 'old')
        site.rename(out)
    finally:
        shutil.rmtree(private, ignore_errors=True)
        shutil.rmtree(stage, ignore_errors=True)
    return packages.version


def check_replaceable(out, marker='driftwire/install.sh'):
    if out.exists() and (not out.is_dir() or (any(out.iterdir()) and not (out / marker).is_file())):
        raise ValueError(f'{out} exists and is not a site this built; not replacing it')


def windows_site(zip_path, out, base_url, require_signature=False, github_repo=GITHUB_REPO):
    """SITE/driftwire/{install.ps1, the zip}: the Windows half alone, unsigned by gpg."""
    base = check_base_url(base_url)
    check_github_repo(github_repo)
    zip_path = Path(zip_path)
    m = ZIP_FILE.fullmatch(zip_path.name)
    if not m or not zip_path.is_file():
        raise ValueError(f'Not a driftwire Windows zip: {zip_path}')
    signed = check_windows_zip(zip_path, m['version'])
    packages = Packages(m['version'], {}, {}, {}, zip_path, signed)
    out = Path(out).resolve()
    check_replaceable(out, 'driftwire/install.ps1')
    out.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.driftwire-site-', dir=out.parent))
    try:
        dw = stage / 'site/driftwire'
        dw.mkdir(parents=True)
        build_windows(dw, packages, base, github_repo, require_signature)
        check_install_ps1(dw)
        check_replaceable(out, 'driftwire/install.ps1')
        if out.exists():
            out.rename(stage / 'old')
        (stage / 'site').rename(out)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return m['version'], signed


# ---- smoke -----------------------------------------------------------------------------------

APT_SOURCES = Path('/etc/apt/sources.list.d/driftwire.sources')
APT_KEYRING = Path('/etc/apt/keyrings/driftwire.gpg')
HIDDEN = ('apt-get', 'dnf', 'yum', 'zypper')


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def site_version(dw):
    names = re.findall(rf'  driftwire-({VERSION})-linux-', (dw / 'SHA256SUMS').read_text())
    if not names or len(set(names)) != 1:
        raise ValueError('SHA256SUMS does not name one version')
    return names[0]


def apt_update_ours(sudo):
    # --error-on=any: with an index it took before, apt keeps that one and only warns.
    return run([*sudo, 'apt-get', 'update', '--error-on=any', '-o', f'Dir::Etc::SourceList={APT_SOURCES}',
                '-o', 'Dir::Etc::SourceParts=/nonexistent', '-o', 'APT::Get::List-Cleanup=0'],
               check=False, text=True)


def apt_lists(url, sudo):
    """apt's files of the source: its lists, and in partial/ (root's only) what it refused."""
    prefix = url.split('://', 1)[1].replace('/', '_') + '_apt_dists_stable_'
    found = run([*sudo, 'find', '/var/lib/apt/lists', '/var/lib/apt/lists/partial', '-maxdepth', '1',
                 '-type', 'f', '-name', prefix + '*'], text=True).stdout
    return sorted(found.split())


def expect(condition, message):
    if not condition:
        raise ValueError('smoke: ' + message)
    print(f'smoke: {message}: yes', flush=True)


def path_without_managers(where):
    """A PATH that has every program of this one but apt-get, dnf, yum and zypper."""
    where.mkdir()
    for folder in os.environ.get('PATH', '/usr/bin:/bin').split(os.pathsep) + ['/usr/sbin', '/sbin']:
        if not os.path.isdir(folder):
            continue
        for entry in sorted(os.listdir(folder)):
            target = os.path.join(folder, entry)
            if entry in HIDDEN or (where / entry).exists() or not os.access(target, os.X_OK) or os.path.isdir(target):
                continue
            (where / entry).symlink_to(target)
    return str(where)


def smoke(site):
    site = Path(site).resolve()
    dw = site / 'driftwire'
    version = site_version(dw)
    if not shutil.which('apt-get') or not shutil.which('dpkg'):
        raise ValueError('smoke runs on Debian or Ubuntu (apt)')
    if shutil.which('driftwire') or APT_SOURCES.exists() or APT_KEYRING.exists():
        raise ValueError('driftwire or its apt source is already on this machine; smoke needs one without')
    root = os.geteuid() == 0
    sudo = [] if root else ['sudo']
    with tempfile.TemporaryDirectory(prefix='driftwire-smoke-') as tmp:
        tmp = Path(tmp)
        os.chmod(tmp, 0o755)
        served = tmp / 'site'
        shutil.copytree(site, served)
        server = http.server.ThreadingHTTPServer(
            ('127.0.0.1', 0), functools.partial(QuietHandler, directory=str(served)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f'http://127.0.0.1:{server.server_address[1]}/driftwire'
        script = urllib.request.urlopen(url + '/install.sh').read().decode('utf-8')
        env = dict(os.environ, DRIFTWIRE_REPO_URL=url)

        def installer(*args, **kwargs):
            print(f'smoke: curl -fsSL {url}/install.sh | sh -s --', *args, flush=True)
            return subprocess.run(['sh', '-s', '--', *args], input=script, text=True,
                                  **{'env': env, 'check': True, **kwargs})

        inrelease = served / 'driftwire/apt/dists/stable/InRelease'
        good = inrelease.read_bytes()
        stamp = [os.stat(inrelease).st_mtime]

        def serve_inrelease(data):
            # Each change newer than the last, so the server never answers apt "not modified".
            stamp[0] += 10
            inrelease.write_bytes(data)
            os.utime(inrelease, (stamp[0], stamp[0]))

        try:
            serve_inrelease(good.replace(b'Label: Driftwire', b'Label: Driftwirf', 1))
            result = installer(check=False, capture_output=True)
            print(result.stdout + result.stderr, flush=True)
            expect(result.returncode != 0 and 'BADSIG' in result.stdout + result.stderr
                   and not Path('/usr/bin/driftwire').exists(),
                   'a first install refuses an InRelease changed by one byte')
            expect(not APT_SOURCES.exists() and not APT_KEYRING.exists() and not apt_lists(url, sudo),
                   'and takes the source, the key and the refused index (partial/ too) away again')
            serve_inrelease(good)

            installer()
            got = run(['driftwire', '--version'], text=True).stdout.strip()
            expect(got == f'driftwire {version}', f'apt installed driftwire {version}')
            expect(f'Signed-By: {APT_KEYRING}' in APT_SOURCES.read_text(), 'the source is pinned to its key')

            serve_inrelease(good.replace(b'Label: Driftwire', b'Label: Driftwirf', 1))
            result = apt_update_ours(sudo)
            expect(result.returncode != 0 and 'BADSIG' in result.stdout + result.stderr,
                   'apt update refuses it too, with the good index already taken')
            serve_inrelease(good)
            expect(apt_update_ours(sudo).returncode == 0, 'apt takes the signed InRelease again')

            # Refused once more and left so: apt keeps the refused InRelease in partial/.
            serve_inrelease(good.replace(b'Label: Driftwire', b'Label: Driftwirf', 1))
            apt_update_ours(sudo)
            serve_inrelease(good)
            refused = [name for name in apt_lists(url, sudo) if name.startswith('/var/lib/apt/lists/partial/')]
            expect(bool(refused), 'apt keeps a refused InRelease in partial/')

            installer('--uninstall')
            expect(not shutil.which('driftwire') and not Path('/usr/bin/driftwire').exists(), 'driftwire removed')
            expect(not APT_SOURCES.exists() and not APT_KEYRING.exists(), 'the source and the key removed')
            expect(not apt_lists(url, sudo), "apt's lists of the source removed, partial/ too")

            # The tarball route: no package manager, a scratch home, an unprivileged user.
            home = tmp / 'home'
            home.mkdir()
            user_env = dict(env, HOME=str(home), PATH=path_without_managers(tmp / 'bin'))
            as_user = []
            if root:
                os.chown(home, 65534, 65534)
                as_user = ['setpriv', '--reuid=65534', '--regid=65534', '--clear-groups', '--']
            print(f'smoke: the same, as {"nobody" if root else "this user"}, apt-get hidden', flush=True)
            subprocess.run([*as_user, 'sh', '-s', '--'], input=script, text=True, env=user_env, check=True)
            binary = home / '.local/bin/driftwire'
            got = run([*as_user, binary, '--version'], env=user_env, text=True).stdout.strip()
            expect(got == f'driftwire {version}', f'the tarball put driftwire {version} in ~/.local')
            subprocess.run([*as_user, 'sh', '-s', '--', '--uninstall'], input=script, text=True, env=user_env,
                           check=True)
            left = sorted(str(p.relative_to(home)) for p in home.rglob('*'))
            expect(not left, 'the tarball install removed, and nothing left in the home' + (f' ({left})' if left else ''))
        finally:
            server.shutdown()
            if shutil.which('driftwire') or APT_SOURCES.exists() or APT_KEYRING.exists():
                installer('--uninstall', check=False)
    print(f'smoke: the site of driftwire {version} installs, refuses a changed index and uninstalls')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    b = commands.add_parser('build', help='build the site')
    b.add_argument('--packages', required=True, help="a folder with one version's .deb, .rpm and tarball per architecture")
    b.add_argument('--out', required=True, help='the site folder (replaced when it is a site this built)')
    b.add_argument('--base-url', required=True, help='where SITE/driftwire is served, https://…/driftwire')
    b.add_argument('--key-id', required=True, action='append', help='a signing key (its fingerprint); twice during a key change')
    b.add_argument('--gnupg-home', help='the GnuPG home that holds the secret key (default GNUPGHOME)')
    b.add_argument('--valid-days', type=int, default=VALID_DAYS, help="the apt Release's Valid-Until, in days")
    b.add_argument('--github-repo', default=GITHUB_REPO,
                   help="OWNER/NAME whose releases carry driftwire; its owner's Homebrew tap is the Mac's channel")
    b.add_argument('--require-windows-signature', action='store_true',
                   help='refuse a Windows zip whose driftwire.exe is not Authenticode-signed (or none)')
    s = commands.add_parser('smoke', help='install from the site on this Debian or Ubuntu machine, and remove it')
    s.add_argument('--site', required=True)
    w = commands.add_parser('windows-site', help='install.ps1 and the Windows zip alone (CI\'s Windows leg)')
    w.add_argument('--zip', required=True, help='driftwire-<version>-windows-x64.zip')
    w.add_argument('--out', required=True, help='the site folder (replaced when it is one this wrote)')
    w.add_argument('--base-url', required=True, help='where SITE/driftwire is served')
    w.add_argument('--github-repo', default=GITHUB_REPO)
    w.add_argument('--require-windows-signature', action='store_true')
    args = parser.parse_args(argv)
    if args.command == 'build':
        version = build(args.packages, args.out, args.base_url, args.key_id, args.gnupg_home, args.valid_days,
                        args.github_repo, args.require_windows_signature)
        print(f'Built the driftwire {version} repository in {args.out} for {check_base_url(args.base_url)}')
    elif args.command == 'windows-site':
        version, signed = windows_site(args.zip, args.out, args.base_url, args.require_windows_signature,
                                       args.github_repo)
        print(f'Wrote install.ps1 for driftwire {version} ({"signed" if signed else "unsigned"}) in {args.out}')
    else:
        smoke(args.site)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode(errors='replace') if isinstance(error.stderr, bytes) else (error.stderr or '')
        sys.exit(f'{error}\n{detail.strip()}')
    except (ValueError, OSError) as error:
        sys.exit(str(error))
