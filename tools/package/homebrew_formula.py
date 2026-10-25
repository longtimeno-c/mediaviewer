#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""The driftwire formula of the Homebrew tap (docs/design/28 "Installing").

    homebrew_formula.py render --version V (--sha256 H | --archive FILE) --url URL
                               [--homepage URL] [--out Formula/driftwire.rb]
    homebrew_formula.py readme --github-repo OWNER/NAME [--out README.md]

`render` fills packaging/macos/driftwire.rb.in for one release asset,
driftwire-V-macos-universal.tar.gz (tools/mac/macpack.py cli): a binary formula, the program
and its manual page, macOS 14 or later. An -arm64 archive (the release's Intel leg did not
build) gets `depends_on arch: :arm64`, so an Intel Mac is told rather than given a program it
cannot run. With --archive it hashes the file and checks that it holds what the formula
installs. release.yml's driftwire-homebrew job runs it on the asset as published and pushes the
result to <owner>/homebrew-tap. `readme` fills that repository's README.md
(packaging/macos/homebrew-tap-README.md), which the job adds when the tap has none.
"""
import argparse
import hashlib
from pathlib import Path
import re
import sys
import tarfile
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / 'packaging' / 'macos' / 'driftwire.rb.in'
README = ROOT / 'packaging' / 'macos' / 'homebrew-tap-README.md'
VERSION = re.compile(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)')
ARCHIVE = re.compile(r'driftwire-(?P<version>[0-9.]+)-macos-(?P<arch>universal|arm64)\.tar\.gz')
# Nothing that could end a Ruby string or interpolate in one.
SAFE_URL = re.compile(r'https://[A-Za-z0-9._~:/%+-]+')
GITHUB_REPO = re.compile(r'[A-Za-z0-9-]+/[A-Za-z0-9._-]+')
# What the formula installs; macpack.py's stage_cli puts both in the archive's folder.
INSTALLED = ('driftwire', 'driftwire.1')


def check_url(url, what):
    parts = urllib.parse.urlsplit(url)
    if not SAFE_URL.fullmatch(url) or not parts.hostname or parts.query or parts.fragment or '..' in url:
        raise ValueError(f'Not a usable https URL for {what}: {url!r}')
    return url


def check_archive(path, version, arch):
    """The archive holds one folder with the program and its manual page."""
    top = f'driftwire-{version}-macos-{arch}'
    with tarfile.open(path, 'r:gz') as tar:
        members = {m.name.rstrip('/'): m for m in tar.getmembers()}
    for name, member in members.items():
        parts = name.split('/')
        if parts[0] != top or '..' in parts or name.startswith('/') or not (member.isfile() or member.isdir()):
            raise ValueError(f'{Path(path).name}: {name!r} is not a plain file or folder under {top}/')
    for name in INSTALLED:
        if f'{top}/{name}' not in members or not members[f'{top}/{name}'].isfile():
            raise ValueError(f'{Path(path).name} has no {top}/{name}')
    if not members[f'{top}/driftwire'].mode & 0o100:
        raise ValueError(f'{Path(path).name}: {top}/driftwire is not executable')


def render(version, url, sha256=None, archive=None, homepage='https://github.com/longtimeno-c/mediaviewer'):
    if not VERSION.fullmatch(version or ''):
        raise ValueError(f'Not an x.y.z version: {version!r}')
    check_url(url, 'the archive')
    check_url(homepage, 'the homepage')
    name = urllib.parse.urlsplit(url).path.rsplit('/', 1)[-1]
    m = ARCHIVE.fullmatch(name)
    if not m or m['version'] != version:
        raise ValueError(f'The URL names {name!r}, not driftwire-{version}-macos-universal.tar.gz (or -arm64)')
    if (sha256 is None) == (archive is None):
        raise ValueError('Give --sha256 or --archive, one of them')
    if archive is not None:
        archive = Path(archive)
        if archive.name != name:
            raise ValueError(f'The archive is {archive.name}, and the URL names {name}')
        check_archive(archive, version, m['arch'])
        sha256 = hashlib.sha256(archive.read_bytes()).hexdigest()
    if not re.fullmatch(r'[0-9a-f]{64}', sha256 or ''):
        raise ValueError(f'Not a SHA-256 (64 lower-case hex digits): {sha256!r}')
    arch = '  depends_on arch: :arm64\n' if m['arch'] == 'arm64' else ''
    text = TEMPLATE.read_text(encoding='utf-8')
    for key, value in (('HOMEPAGE', homepage), ('URL', url), ('VERSION', version), ('SHA256', sha256)):
        text = text.replace(f'@{key}@', value)
    text = text.replace('@ARCH_REQUIREMENT@\n', arch)
    left = sorted(set(re.findall(r'@[A-Z][A-Z_]+@', text)))
    if left:
        raise ValueError(f'driftwire.rb.in: nothing to fill {", ".join(left)} with')
    return text


def readme(github_repo):
    if not GITHUB_REPO.fullmatch(github_repo or '') or '..' in github_repo:
        raise ValueError(f'Not a GitHub repository (OWNER/NAME): {github_repo!r}')
    return (README.read_text(encoding='utf-8').replace('@GITHUB_REPO@', github_repo)
            .replace('@OWNER@', github_repo.split('/')[0]))


def write(text, out):
    if out:
        Path(out).parent.mkdir(parents=True, exist_ok=True)
        Path(out).write_text(text, encoding='utf-8')
    else:
        sys.stdout.write(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    commands = parser.add_subparsers(dest='command', required=True)
    r = commands.add_parser('render', help='the formula for one release asset')
    r.add_argument('--version', required=True)
    r.add_argument('--url', required=True, help='the release asset, https://…/driftwire-V-macos-universal.tar.gz')
    r.add_argument('--sha256', help="the asset's SHA-256")
    r.add_argument('--archive', help='the asset itself: hashed, and checked for what the formula installs')
    r.add_argument('--homepage', default='https://github.com/longtimeno-c/mediaviewer')
    r.add_argument('--out', help='where to write it (default: standard output)')
    d = commands.add_parser('readme', help="the tap repository's README.md")
    d.add_argument('--github-repo', required=True)
    d.add_argument('--out')
    args = parser.parse_args(argv)
    if args.command == 'render':
        write(render(args.version, args.url, args.sha256, args.archive, args.homepage), args.out)
    else:
        write(readme(args.github_repo), args.out)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, tarfile.TarError) as error:
        sys.exit(str(error))
