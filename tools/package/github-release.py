#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Preflight and publish a complete two-platform release. No build secrets are logged."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def version_tuple(value):
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", value):
        raise ValueError(f"Expected a strict x.y.z version, got {value!r}")
    return tuple(map(int, value.split('.')))


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
    if mode == 'stable':
        required = ('MV_MANIFEST_SIGNING_KEY', 'MV_MAC_CERT_P12_BASE64', 'MV_MAC_CERT_PASSWORD',
                    'APPLE_ID', 'APPLE_TEAM_ID', 'APPLE_APP_PASSWORD', 'MV_SPARKLE_PRIVATE_KEY')
        missing = [name for name in required if not os.environ.get(name)]
        if missing:
            raise ValueError('Stable release needs repository secrets: ' + ', '.join(missing)
                             + '. Use preview to publish unsigned test installers.')
        azure = ('AZURE_TENANT_ID', 'AZURE_CLIENT_ID', 'AZURE_CLIENT_SECRET',
                 'TRUSTED_SIGNING_ENDPOINT', 'TRUSTED_SIGNING_ACCOUNT', 'TRUSTED_SIGNING_PROFILE')
        configured = [name for name in azure if os.environ.get(name)]
        if configured and len(configured) != len(azure):
            raise ValueError('Incomplete optional Windows code signing. Missing: '
                             + ', '.join(name for name in azure if name not in configured))
        if not configured:
            print('::warning::Windows installers will be unsigned; SmartScreen may warn.')
    tag = f'v{version}'
    if mode == 'preview':
        tag += '.preview.' + os.environ['GITHUB_RUN_ID']
    with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
        output.write(f'version={version}\ntag={tag}\n')
    print(f'{mode}: {version} ({tag})')


def validate_assets(folder, version, mode, repo, tag):
    names = [f'MediaViewer-{version}-Setup.exe', f'MediaViewer-{version}.dmg']
    if mode == 'stable':
        names += [f'MediaViewer-{version}-full.nupkg', 'RELEASES', 'releases.win.json',
                  'assets.win.json', 'mediaviewer-manifest.json', 'mediaviewer-manifest.json.sig',
                  f'MediaViewer-{version}.zip', 'appcast.xml']
    for name in names:
        path = folder / name
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError('Missing or empty release asset: ' + name)
    if mode == 'stable':
        manifest = json.loads((folder / 'mediaviewer-manifest.json').read_text())
        if manifest['version'] != version or manifest['channel'] != 'win':
            raise ValueError('Windows manifest version/channel mismatch')
        if (folder / 'mediaviewer-manifest.json.sig').stat().st_size != 64:
            raise ValueError('Invalid Windows signature length')
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


def publish(folder):
    mode, version, tag, repo, sha = (os.environ[name] for name in
                                   ('MODE', 'VERSION', 'TAG', 'GITHUB_REPOSITORY', 'GITHUB_SHA'))
    if mode not in ('preview', 'stable'):
        raise ValueError('Only preview or stable may publish')
    version_tuple(version)
    assets = validate_assets(folder, version, mode, repo, tag)
    existing = api_optional(f'repos/{repo}/releases/tags/{tag}')
    if existing and (not existing['draft'] or existing['target_commitish'] != sha):
        raise ValueError('Refusing to overwrite a published release or a draft from another commit')
    ref = api_optional(f'repos/{repo}/git/ref/tags/{tag}')
    if ref:
        commit = json.loads(gh('api', f'repos/{repo}/commits/{tag}'))
        if commit['sha'] != sha:
            raise ValueError('Existing tag points at a different commit')
    if mode == 'stable':
        latest = api_optional(f'repos/{repo}/releases/latest')
        if latest and version_tuple(latest['tag_name'].removeprefix('v')) >= version_tuple(version):
            raise ValueError('Bump CMakeLists.txt: stable version must exceed the current latest release')
    checksums = folder / 'SHA256SUMS.txt'
    checksums.write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n'
                                 for p in sorted(assets)), encoding='utf-8')
    assets.append(checksums)
    notes = folder / 'release-notes.md'
    text = ('Windows x64: download the Setup.exe installer.\n\n'
            'macOS 14+ on Apple Silicon: download the .dmg and drag MediaViewer to Applications.\n\n')
    if mode == 'preview':
        text += ('Unsigned test build. Windows SmartScreen may warn; macOS Gatekeeper may block '
                 'the unnotarized app. The Mac preview has no automatic updater; install the '
                 'stable version manually later. This prerelease does not change the stable update feed.\n')
    else:
        text += ('Includes the Windows and macOS update feeds. Windows Authenticode signing is '
                 'optional; if unavailable, SmartScreen may warn on first installation.\n')
    text += f'\nSource commit: {sha}\n'
    notes.write_text(text, encoding='utf-8')
    if not existing:
        gh('release', 'create', tag, '--repo', repo, '--target', sha, '--draft',
           '--title', f'MediaViewer {version}' + (' (unsigned preview)' if mode == 'preview' else ''),
           '--notes-file', str(notes))
    gh('release', 'upload', tag, '--repo', repo, '--clobber', *(str(p) for p in assets))
    uploaded = json.loads(gh('api', f'repos/{repo}/releases/tags/{tag}'))
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
            summary.write(f'Published [{tag}]({url}) with both installers and SHA256SUMS.txt.\n')


if __name__ == '__main__':
    try:
        if sys.argv[1:] == ['prepare']:
            prepare()
        elif len(sys.argv) == 3 and sys.argv[1] == 'publish':
            publish(Path(sys.argv[2]))
        else:
            raise ValueError('usage: github-release.py prepare | publish <assets-directory>')
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
