#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Preflight and publish a complete release: both installers, and the driftwire command line's
macOS and Windows archives once it is published (MV_RELEASE_CLI). No build secrets are logged."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import xml.etree.ElementTree as ET
import zipfile


def version_tuple(value):
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value):
        raise ValueError(f"Expected a strict x.y.z version, got {value!r}")
    return tuple(map(int, value.split('.')))


# Stable tags are v<version>. Previews are too; tags from before previews became
# signed updates carry a .preview.<run-id> suffix, which only the tag ever had.
def tag_version(tag):
    match = re.fullmatch(r'v([0-9.]+?)(?:\.preview\.[0-9]+)?', tag or '')
    try:
        return version_tuple(match[1]) if match else None
    except ValueError:
        return None


def project_version():
    match = re.search(r'project\(\s*mediaviewer\s+VERSION\s+(\S+)', Path('CMakeLists.txt').read_text())
    if not match:
        raise ValueError('No project version in CMakeLists.txt')
    return match[1]


def prepare():
    mode = os.environ['MODE']
    if mode not in ('artifacts', 'preview', 'stable'):
        raise ValueError('Unknown release mode')
    version = project_version()
    current = version_tuple(version)
    if version_tuple(os.environ['MIN_VERSION']) > current:
        raise ValueError('min_version cannot exceed the release version')
    for blocked in filter(None, os.environ.get('BLOCKLIST', '').split(',')):
        version_tuple(blocked)
        if blocked == version:
            raise ValueError('Cannot blocklist the release being published')
    if os.environ.get('GITHUB_REF_TYPE') == 'tag' and os.environ['GITHUB_REF_NAME'] != f'v{version}':
        raise ValueError('Selected tag must match CMakeLists.txt: v' + version)
    # A preview is a signed update too (Settings > Update channel), so it needs
    # every credential stable does.
    if mode in ('preview', 'stable'):
        required = ('MV_MANIFEST_SIGNING_KEY', 'MV_MAC_CERT_P12_BASE64', 'MV_MAC_CERT_PASSWORD',
                    'APPLE_ID', 'APPLE_TEAM_ID', 'APPLE_APP_PASSWORD', 'MV_SPARKLE_PRIVATE_KEY')
        missing = [name for name in required if not os.environ.get(name)]
        if missing:
            raise ValueError(f'A {mode} release needs repository secrets: ' + ', '.join(missing)
                             + '. Use artifacts to build unsigned test installers.')
        azure = ('AZURE_TENANT_ID', 'AZURE_CLIENT_ID', 'AZURE_CLIENT_SECRET',
                 'TRUSTED_SIGNING_ENDPOINT', 'TRUSTED_SIGNING_ACCOUNT', 'TRUSTED_SIGNING_PROFILE')
        configured = [name for name in azure if os.environ.get(name)]
        if configured and len(configured) != len(azure):
            raise ValueError('Incomplete optional Windows code signing. Missing: '
                             + ', '.join(name for name in azure if name not in configured))
        if not configured:
            print('::warning::Windows installers will be unsigned; SmartScreen may warn.')
    # MV_DRIFTWIRE_REPO=1 publishes the command line from a stable release: after publish,
    # driftwire-repo signs the package repository with DRIFTWIRE_REPO_GPG_KEY and
    # driftwire-homebrew pushes the formula with HOMEBREW_TAP_TOKEN. Without either, that job
    # would fail after the release is out. release.yml says only whether each secret is set
    # (DRIFTWIRE_REPO_KEY_SET, HOMEBREW_TAP_TOKEN_SET), never what it holds.
    if mode == 'stable' and os.environ.get('MV_DRIFTWIRE_REPO') == '1':
        unset = [secret for secret, flag in (('DRIFTWIRE_REPO_GPG_KEY', 'DRIFTWIRE_REPO_KEY_SET'),
                                             ('HOMEBREW_TAP_TOKEN', 'HOMEBREW_TAP_TOKEN_SET'))
                 if os.environ.get(flag) != '1']
        if unset:
            one = len(unset) == 1
            raise ValueError(f'MV_DRIFTWIRE_REPO is 1 but the secret{"" if one else "s"} ' + ' and '.join(unset)
                             + f' {"is" if one else "are"} not set: add {"it" if one else "them"} (docs/DEVELOPMENT.md, '
                             '"Publishing the command line") or set MV_DRIFTWIRE_REPO to 0.')
    tag = f'v{version}'
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write(f'version={version}\ntag={tag}\n')
    print(f'{mode}: {version} ({tag})')


# The optional add-ons, one signed package per add-on and platform. The apps
# fetch them from releases/latest. MV_RELEASE_ADDONS=1 requires Import
# (docs/design/18) and Driftwire (docs/design/28, code only, like Import): a
# stable release without either is refused. MV_RELEASE_AI=1 requires the Local
# search pack and its pieces (docs/design/17). Places (docs/design/27) is
# preferred and not required: a complete pack is published, and a release with
# none still publishes. Publish uploads only what is listed here: an add-on
# packed into the upload folder but not listed never reaches the release.
# NVIDIA acceleration (ai-cuda) is a Windows-only piece: ONNX Runtime's CUDA
# build (MIT); the CUDA runtime and cuDNN stay user-supplied (log 2026-10-03).
ADDON_PLATFORMS = ('win-x64', 'macos')
FIRST_PARTY_ADDONS = ('import', 'driftwire')
PREFERRED_ADDONS = ('places', 'enhance')
AI_ADDONS = ('ai', 'ai-audio', 'ai-docs', 'ai-faces', 'ai-cuda')
WINDOWS_ONLY_ADDONS = ('ai-cuda',)


# The driftwire command line's Linux packages (docs/design/28 "From a
# terminal", cmake/driftwire-cli): a .deb, an .rpm and a tarball for each
# architecture, (Debian name, RPM and tarball name), built and tested by
# release.yml's linux-cli job on every run. They reach people through the
# package repository (tools/package/driftwire_repo.py, release.yml's
# driftwire-repo job), which is how they are updated (docs/design/12,
# 2026-10-24), and are attached to the release only with
# MV_RELEASE_LINUX_CLI=1, which release.yml leaves off: a .deb from the
# release page would install without the source that updates it. With it
# on, every published release, preview or stable, needs all six.
LINUX_CLI_ARCHES = (('amd64', 'x86_64'), ('arm64', 'aarch64'))


# The command line's macOS and Windows archives (docs/design/28 "Installing",
# docs/design/12 2026-10-24): driftwire-<v>-macos-universal.tar.gz, which the
# Homebrew tap's formula installs from the release page (-arm64 when the
# release's Intel leg did not build), and driftwire-<v>-windows-x64.zip,
# which the package repository's site serves to the PowerShell one-liner.
# MV_RELEASE_CLI=1 (release.yml sets it from the repository variable
# MV_DRIFTWIRE_REPO, the one switch that publishes the command line) requires
# both on a stable release; previews, and any release with it off, ignore
# them, so a release of the installers alone stays whole.
CLI_MAC_LABELS = ('universal', 'arm64')


def cli_published():
    return os.environ.get('MV_RELEASE_CLI') == '1'


def cli_asset_names(folder, version):
    """The Windows zip, and the one macOS archive there is (universal when there is none)."""
    mac = [f'driftwire-{version}-macos-{label}.tar.gz' for label in CLI_MAC_LABELS]
    have = [name for name in mac if (folder / name).is_file()]
    if len(have) > 1:
        raise ValueError("Two macOS archives of the command line: " + ', '.join(have))
    return [f'driftwire-{version}-windows-x64.zip', have[0] if have else mac[0]]


def validate_cli(folder, names):
    """One folder named as the archive, holding the program (and on macOS its manual page)."""
    for name in names:
        path = folder / name
        if name.endswith('.zip'):
            top, wanted = name[:-len('.zip')], ('driftwire.exe',)
            try:
                with zipfile.ZipFile(path) as z:
                    members = {n.rstrip('/'): not n.endswith('/') for n in z.namelist()}
            except zipfile.BadZipFile:
                raise ValueError(f'Not a zip: {name}') from None
        else:
            top, wanted = name[:-len('.tar.gz')], ('driftwire', 'driftwire.1')
            try:
                with tarfile.open(path, 'r:gz') as tar:
                    members = {m.name.rstrip('/'): m.isfile() for m in tar.getmembers()}
            except tarfile.TarError:
                raise ValueError(f'Not a gzipped tar: {name}') from None
        for member in members:
            parts = member.split('/')
            if parts[0] != top or '..' in parts or member.startswith('/'):
                raise ValueError(f'{name} holds {member!r}, outside {top}/')
        for want in wanted:
            if not members.get(f'{top}/{want}'):
                raise ValueError(f'{name} has no {top}/{want}')


def linux_cli_published():
    return os.environ.get('MV_RELEASE_LINUX_CLI') == '1'


def linux_cli_asset_names(version):
    names = []
    for deb_arch, rpm_arch in LINUX_CLI_ARCHES:
        names += [f'driftwire_{version}_{deb_arch}.deb', f'driftwire-{version}-1.{rpm_arch}.rpm',
                  f'driftwire-{version}-linux-{rpm_arch}.tar.gz']
    return names


def addon_platforms(addon):
    return ('win-x64',) if addon in WINDOWS_ONLY_ADDONS else ADDON_PLATFORMS


def required_addons(mode):
    if mode != 'stable':
        return []
    ids = []
    if os.environ.get('MV_RELEASE_ADDONS') == '1':
        ids += FIRST_PARTY_ADDONS
    if os.environ.get('MV_RELEASE_AI') == '1':
        ids += AI_ADDONS
    return ids


def preferred_asset_names(folder, version, mode):
    """Places and anything else we ship when the pack is there.

    A platform with no files is skipped. Some of its files, or a pack that
    does not match its manifest, refuses the release: a half pack must not
    be published, and a missing pack must not block it.
    """
    if mode != 'stable':
        return []
    names = []
    for addon in PREFERRED_ADDONS:
        for platform in addon_platforms(addon):
            assets = addon_asset_names(platform, addon)
            have = [name for name in assets
                    if (folder / name).is_file() and (folder / name).stat().st_size > 0]
            if not have:
                continue
            if len(have) != len(assets):
                missing = [name for name in assets if name not in have]
                raise ValueError(f'Incomplete {addon} add-on: missing ' + ', '.join(missing))
            validate_addon(folder, version, platform, addon)
            names += assets
    return names


def note_preferred(mode, asset_names):
    if mode != 'stable':
        return
    packed = set(asset_names)
    for addon in PREFERRED_ADDONS:
        present = [platform for platform in addon_platforms(addon)
                   if addon_asset_names(platform, addon)[0] in packed]
        missing = [platform for platform in addon_platforms(addon) if platform not in present]
        if not present:
            print(f'::warning::{addon} add-on was not packed; this release will not carry it.')
        elif missing:
            print(f'::warning::{addon} add-on was not packed for ' + ', '.join(missing) + '.')


def addons_required(mode):
    return bool(required_addons(mode))


def addon_asset_names(platform, addon='import'):
    base = f'mediaviewer-addon-{addon}-{platform}'
    return [base + '.zip', base + '.json', base + '.json.sig']


def validate_addon(folder, version, platform, addon='import'):
    """Shape only; the signature is the app's to check (addon-pack.py packed
    it with --require-pinned-key)."""
    zip_name, manifest_name, sig_name = addon_asset_names(platform, addon)
    manifest = json.loads((folder / manifest_name).read_text())
    if manifest.get('id') != addon or manifest.get('platform') != platform:
        raise ValueError(f'Add-on manifest id/platform mismatch: {manifest_name}')
    if manifest.get('version') != version:
        raise ValueError(f'Add-on version {manifest.get("version")} is not the release version: {manifest_name}')
    if (folder / sig_name).stat().st_size != 64:
        raise ValueError(f'Invalid add-on signature length: {sig_name}')
    archive = manifest.get('archive') or {}
    path = folder / zip_name
    if archive.get('path') != zip_name or path.stat().st_size != archive.get('size') or \
            hashlib.sha256(path.read_bytes()).hexdigest() != archive.get('sha256'):
        raise ValueError(f'Add-on archive does not match its signed manifest: {zip_name}')


def validate_assets(folder, version, mode, repo, tag):
    names = [f'MediaViewer-{version}-Setup.exe', f'MediaViewer-{version}.dmg']
    if mode in ('preview', 'stable'):
        names += [f'MediaViewer-{version}-full.nupkg', 'RELEASES', 'releases.win.json',
                  'assets.win.json', 'mediaviewer-manifest.json', 'mediaviewer-manifest.json.sig',
                  f'MediaViewer-{version}.zip', 'appcast.xml']
        for addon in required_addons(mode):
            for platform in addon_platforms(addon):
                names += addon_asset_names(platform, addon)
        names += preferred_asset_names(folder, version, mode)
    cli = cli_asset_names(folder, version) if mode == 'stable' and cli_published() else []
    names += cli
    if linux_cli_published():
        names += linux_cli_asset_names(version)
    for name in names:
        path = folder / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError('Missing or empty release asset: ' + name)
    validate_cli(folder, cli)
    if mode in ('preview', 'stable'):
        manifest = json.loads((folder / 'mediaviewer-manifest.json').read_text())
        if manifest['version'] != version or manifest['channel'] != 'win':
            raise ValueError('Windows manifest version/channel mismatch')
        if (folder / 'mediaviewer-manifest.json.sig').stat().st_size != 64:
            raise ValueError('Invalid Windows signature length')
        for addon in required_addons(mode):
            for platform in addon_platforms(addon):
                validate_addon(folder, version, platform, addon)
        for package in manifest['packages']:
            name = package['file']
            if Path(name).name != name or '/' in name or '\\' in name:
                raise ValueError('Package filename must be a basename')
            path = folder / name
            if path.stat().st_size != package['size'] or hashlib.sha256(path.read_bytes()).hexdigest() != package['sha256']:
                raise ValueError('Package does not match signed manifest: ' + name)
            if name not in names:
                names.append(name)
        feed = (folder / 'appcast.xml').read_text()
        enclosures = ET.fromstring(feed).findall('./channel/item/enclosure')
        expected = f'https://github.com/{repo}/releases/download/{tag}/MediaViewer-{version}.zip'
        sparkle = '{http://www.andymatuschak.org/xml-namespaces/sparkle}'
        if 'sparkle-signatures' not in feed or not any(
                e.get('url') == expected and e.get(sparkle + 'edSignature')
                and e.get('length') == str((folder / f'MediaViewer-{version}.zip').stat().st_size)
                for e in enclosures):
            raise ValueError('Mac appcast is unsigned or does not reference this release archive')
    return [folder / name for name in names]


def gh(*args):
    return subprocess.run(['gh', *args], check=True, text=True, capture_output=True).stdout


def api_optional(endpoint):
    try:
        return json.loads(gh('api', endpoint))
    except subprocess.CalledProcessError as error:
        if 'HTTP 404' in error.stderr:
            return None
        raise


def release_by_tag(repo, tag):
    # The /releases/tags endpoint can return 404 for an unpublished draft.
    # Authenticated release listings include drafts; paginate to support retries
    # even when the desired draft is not on the first page.
    pages = json.loads(gh('api', '--paginate', '--slurp', f'repos/{repo}/releases?per_page=100'))
    return next((release for page in pages for release in page if release['tag_name'] == tag), None)


def highest_release_version(repo):
    """The highest version any published release (stable or preview) has used."""
    pages = json.loads(gh('api', '--paginate', '--slurp', f'repos/{repo}/releases?per_page=100'))
    versions = [tag_version(r['tag_name']) for page in pages for r in page if not r['draft']]
    return max(filter(None, versions), default=None)


def cli_notes(version, repo, base, names):
    """The command line's three ways in. The site is this repository's GitHub Pages at its usual
    address (release.yml's driftwire-repo deploys it); the tap is the owner's homebrew-tap."""
    owner, name = repo.split('/', 1)
    site = f'https://{owner.lower()}.github.io/{name}/driftwire'
    windows, mac = names
    return ('- **driftwire**, the command line that sends files to MediaViewer or another driftwire nearby '
            '(it needs no MediaViewer): on Linux `curl -fsSL ' + site + '/install.sh | sh` (or apt and dnf, '
            f'[{site}/]({site}/)), on macOS `brew install {owner}/tap/driftwire`, on Windows '
            f'`irm {site}/install.ps1 | iex`. The [macOS archive]({base}/{mac}) and the '
            f'[Windows zip]({base}/{windows}) are below too.\n')


def release_notes(version, mode, repo, tag, sha, cli=None):
    base = f'https://github.com/{repo}/releases/download/{tag}'
    text = ('## Downloads\n\n'
            f'- **[Download for Windows (x64)]({base}/MediaViewer-{version}-Setup.exe)** '
            '- run the installer.\n'
            f'- **[Download for Mac (Apple Silicon and Intel, macOS 14+)]({base}/MediaViewer-{version}.dmg)** '
            '- open the disk image and drag MediaViewer to Applications.\n')
    if linux_cli_published():
        text += ('- **driftwire for Linux**, the command line that sends files to and from MediaViewer\'s '
                 f'Driftwire: x86_64 [.deb]({base}/driftwire_{version}_amd64.deb), '
                 f'[.rpm]({base}/driftwire-{version}-1.x86_64.rpm) or '
                 f'[tarball]({base}/driftwire-{version}-linux-x86_64.tar.gz); '
                 f'arm64 [.deb]({base}/driftwire_{version}_arm64.deb), '
                 f'[.rpm]({base}/driftwire-{version}-1.aarch64.rpm) or '
                 f'[tarball]({base}/driftwire-{version}-linux-aarch64.tar.gz) '
                 '- install the package with your package manager (glibc 2.35 or newer).\n')
    if mode == 'stable' and cli_published():
        text += cli_notes(version, repo, base, cli or [f'driftwire-{version}-windows-x64.zip',
                                                      f'driftwire-{version}-macos-universal.tar.gz'])
    text += '\n'
    if mode == 'preview':
        text += ('Preview build: signed like a stable release, but not marked Latest. Apps with '
                 '**Settings > Update channel** set to Preview update to it automatically; the '
                 'stable channel does not see it.\n\n'
                 'Windows Authenticode signing is optional; if unavailable, SmartScreen '
                 'may warn on first installation.\n')
    else:
        text += ('You only need the installer for your platform. Other assets below support '
                 'automatic updates, download verification, and source-code access.\n\n'
                 'Windows Authenticode signing is optional; if unavailable, SmartScreen '
                 'may warn on first installation.\n')
    return text + f'\nSource commit: {sha}\n'


def publish(folder):
    mode, version, tag, repo, sha = (os.environ[name] for name in
                                   ('MODE', 'VERSION', 'TAG', 'GITHUB_REPOSITORY', 'GITHUB_SHA'))
    if mode not in ('preview', 'stable'):
        raise ValueError('Only preview or stable may publish')
    version_tuple(version)
    assets = validate_assets(folder, version, mode, repo, tag)
    note_preferred(mode, [p.name for p in assets])
    existing = release_by_tag(repo, tag)
    if existing and (not existing['draft'] or existing['target_commitish'] != sha):
        raise ValueError('Refusing to overwrite a published release or a draft from another commit')
    ref = api_optional(f'repos/{repo}/git/ref/tags/{tag}')
    if ref:
        commit = json.loads(gh('api', f'repos/{repo}/commits/{tag}'))
        if commit['sha'] != sha:
            raise ValueError('Existing tag points at a different commit')
    # Previews and stable releases share one version sequence: a preview uses up
    # its version, so the preview channel always sees a stable release that
    # follows it as newer.
    highest = highest_release_version(repo)
    if highest and highest >= version_tuple(version):
        raise ValueError('Bump CMakeLists.txt: the version must exceed every published release, '
                         'previews included')
    checksums = folder / 'SHA256SUMS.txt'
    checksums.write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n'
                                 for p in sorted(assets)), encoding='utf-8')
    assets.append(checksums)
    notes = folder / 'release-notes.md'
    cli = cli_asset_names(folder, version) if mode == 'stable' and cli_published() else None
    notes.write_text(release_notes(version, mode, repo, tag, sha, cli), encoding='utf-8')
    if not existing:
        gh('release', 'create', tag, '--repo', repo, '--target', sha, '--draft',
           '--title', f'MediaViewer {version}' + (' (preview)' if mode == 'preview' else ''),
           '--notes-file', str(notes))
    gh('release', 'upload', tag, '--repo', repo, '--clobber', *(str(p) for p in assets))
    uploaded = release_by_tag(repo, tag)
    if not uploaded or not uploaded['draft'] or uploaded['target_commitish'] != sha:
        raise ValueError('Expected draft from this source commit; refusing publication')
    actual = {a['name']: a['size'] for a in uploaded['assets']}
    if actual != {p.name: p.stat().st_size for p in assets}:
        raise ValueError('Draft assets differ from the validated set; leaving draft unpublished')
    gh('release', 'edit', tag, '--repo', repo, '--draft=false',
       '--prerelease=' + str(mode == 'preview').lower(),
       '--latest=' + str(mode == 'stable').lower(), '--notes-file', str(notes))
    url = f'https://github.com/{repo}/releases/tag/{tag}'
    print(url)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a', encoding='utf-8') as summary:
            linux = ', the driftwire Linux packages' if linux_cli_published() else ''
            cli_text = ", driftwire's macOS and Windows archives" if mode == 'stable' and cli_published() else ''
            summary.write(f'Published [{tag}]({url}) with both installers{cli_text}{linux} and SHA256SUMS.txt.\n')


if __name__ == '__main__':
    try:
        if sys.argv[1:] == ['prepare']:
            prepare()
        elif len(sys.argv) == 3 and sys.argv[1] == 'publish':
            publish(Path(sys.argv[2]))
        else:
            raise ValueError('usage: github-release.py prepare | publish <assets-directory>')
    except subprocess.CalledProcessError as error:
        sys.exit(f'{error}\n{(error.stderr or "").strip()}')
    except (ValueError, KeyError, OSError) as error:
        sys.exit(str(error))
