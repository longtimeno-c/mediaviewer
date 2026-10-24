# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release safety checks, using temporary assets and a mocked GitHub CLI."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('release', Path(__file__).with_name('github-release.py'))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class DraftLookupTests(unittest.TestCase):
    def test_finds_draft_on_later_page_without_published_tag_endpoint(self):
        draft = {'id': 42, 'tag_name': 'v0.1.1', 'draft': True}
        pages = [[{'tag_name': 'v0.1.0', 'draft': False}], [draft]]
        with patch.object(release, 'gh', return_value=json.dumps(pages)) as cli:
            self.assertEqual(release.release_by_tag('owner/repo', 'v0.1.1'), draft)
        cli.assert_called_once_with('api', '--paginate', '--slurp', 'repos/owner/repo/releases?per_page=100')

    def test_missing_release_returns_none(self):
        with patch.object(release, 'gh', return_value='[[]]'):
            self.assertIsNone(release.release_by_tag('owner/repo', 'v0.1.1'))

    def test_highest_release_version_counts_previews_and_skips_drafts(self):
        pages = [[{'tag_name': 'v0.1.3', 'draft': False},
                  {'tag_name': 'v0.1.4.preview.99', 'draft': False},
                  {'tag_name': 'v0.1.9', 'draft': True}]]
        with patch.object(release, 'gh', return_value=json.dumps(pages)):
            self.assertEqual(release.highest_release_version('owner/repo'), (0, 1, 4))

    def test_authentication_failure_is_not_treated_as_missing_release(self):
        with patch.object(release, 'gh', side_effect=subprocess.CalledProcessError(1, 'gh', stderr='HTTP 403')):
            with self.assertRaises(subprocess.CalledProcessError):
                release.release_by_tag('owner/repo', 'v0.1.1')


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.env = {'MODE': 'artifacts', 'VERSION': '0.1.1', 'TAG': 'v0.1.1',
                    'GITHUB_REPOSITORY': 'owner/repo', 'GITHUB_SHA': 'abc123',
                    'GITHUB_RUN_ID': '1', 'MIN_VERSION': '0.0.0', 'BLOCKLIST': '',
                    'GITHUB_OUTPUT': str(self.folder / 'output')}
        self.addCleanup(patch.stopall)
        patch.dict(os.environ, self.env, clear=True).start()
        patch.object(release, 'project_version', return_value='0.1.1').start()
        self.lookup = patch.object(release, 'release_by_tag', return_value=None).start()
        self.highest = patch.object(release, 'highest_release_version', return_value=None).start()
        for name in ('MediaViewer-0.1.1-Setup.exe', 'MediaViewer-0.1.1.dmg'):
            (self.folder / name).write_bytes(b'test installer')

    def test_artifacts_preflight_needs_no_secrets(self):
        release.prepare()
        self.assertIn('tag=v0.1.1', (self.folder / 'output').read_text())

    def test_signed_preflight_fails_before_build_without_credentials(self):
        for mode in ('preview', 'stable'):
            os.environ['MODE'] = mode
            with self.assertRaisesRegex(ValueError, 'MV_MANIFEST_SIGNING_KEY'):
                release.prepare()

    def test_preview_tag_has_no_suffix(self):
        os.environ['MODE'] = 'preview'
        for name in ('MV_MANIFEST_SIGNING_KEY', 'MV_MAC_CERT_P12_BASE64', 'MV_MAC_CERT_PASSWORD',
                     'APPLE_ID', 'APPLE_TEAM_ID', 'APPLE_APP_PASSWORD', 'MV_SPARKLE_PRIVATE_KEY'):
            os.environ[name] = 'test'
        release.prepare()
        self.assertIn('tag=v0.1.1\n', (self.folder / 'output').read_text())

    def test_tag_version_reads_stable_and_legacy_preview_tags(self):
        self.assertEqual(release.tag_version('v0.1.4'), (0, 1, 4))
        self.assertEqual(release.tag_version('v0.1.4.preview.36219649049'), (0, 1, 4))
        self.assertIsNone(release.tag_version('nightly'))

    def test_partial_azure_configuration_fails(self):
        os.environ['MODE'] = 'stable'
        for name in ('MV_MANIFEST_SIGNING_KEY', 'MV_MAC_CERT_P12_BASE64', 'MV_MAC_CERT_PASSWORD',
                     'APPLE_ID', 'APPLE_TEAM_ID', 'APPLE_APP_PASSWORD', 'MV_SPARKLE_PRIVATE_KEY',
                     'AZURE_TENANT_ID'):
            os.environ[name] = 'test'
        with self.assertRaisesRegex(ValueError, 'Incomplete optional Windows'):
            release.prepare()

    def test_package_repository_needs_its_key_on_stable(self):
        for name in ('MV_MANIFEST_SIGNING_KEY', 'MV_MAC_CERT_P12_BASE64', 'MV_MAC_CERT_PASSWORD',
                     'APPLE_ID', 'APPLE_TEAM_ID', 'APPLE_APP_PASSWORD', 'MV_SPARKLE_PRIVATE_KEY'):
            os.environ[name] = 'test'
        os.environ['MV_DRIFTWIRE_REPO'] = '1'
        os.environ['MODE'] = 'stable'
        with self.assertRaisesRegex(ValueError, 'DRIFTWIRE_REPO_GPG_KEY is not set'):
            release.prepare()
        os.environ['MODE'] = 'preview'   # previews leave the repository alone
        release.prepare()
        os.environ['MODE'] = 'stable'
        os.environ['DRIFTWIRE_REPO_GPG_KEY'] = 'test'   # the key itself is not what prepare reads
        with self.assertRaisesRegex(ValueError, 'DRIFTWIRE_REPO_GPG_KEY is not set'):
            release.prepare()
        del os.environ['DRIFTWIRE_REPO_GPG_KEY']
        os.environ['DRIFTWIRE_REPO_KEY_SET'] = '1'
        release.prepare()
        del os.environ['DRIFTWIRE_REPO_KEY_SET']
        os.environ['MV_DRIFTWIRE_REPO'] = '0'
        release.prepare()

    def test_preflight_is_told_whether_the_repository_key_is_set_not_the_key(self):
        # release.yml's prepare step: a flag from the secret, never the secret
        # itself; only driftwire-repo's signing step gets the key.
        workflow = (Path(__file__).parents[2] / '.github/workflows/release.yml').read_text()
        prepare_job = workflow.split('\n  prepare:\n', 1)[1].split('\n  windows:\n', 1)[0]
        self.assertIn("DRIFTWIRE_REPO_KEY_SET: ${{ secrets.DRIFTWIRE_REPO_GPG_KEY != '' && '1' || '' }}",
                      prepare_job)
        self.assertNotIn('DRIFTWIRE_REPO_GPG_KEY: ', prepare_job)
        self.assertEqual(workflow.count('DRIFTWIRE_REPO_GPG_KEY: ${{ secrets.DRIFTWIRE_REPO_GPG_KEY }}'), 1)

    def test_release_cannot_blocklist_itself(self):
        os.environ['BLOCKLIST'] = '0.1.1'
        with self.assertRaisesRegex(ValueError, 'Cannot blocklist'):
            release.prepare()

    def test_missing_platform_never_contacts_github(self):
        self.stable_assets()
        (self.folder / 'MediaViewer-0.1.1.dmg').unlink()
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'Missing or empty'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_published_release_is_never_overwritten(self):
        self.stable_assets()
        self.lookup.return_value = {'draft': False}
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'Refusing to overwrite'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_upload_failure_leaves_draft_unpublished(self):
        self.stable_assets()
        calls = []
        def cli(*args):
            calls.append(args)
            if args[:2] == ('release', 'upload'):
                raise subprocess.CalledProcessError(1, 'gh', stderr='upload failed')
            return ''
        with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh', side_effect=cli):
            with self.assertRaises(subprocess.CalledProcessError):
                release.publish(self.folder)
        self.assertEqual([c[:2] for c in calls], [('release', 'create'), ('release', 'upload')])
        self.assertIn('--draft', calls[0])

    def test_preview_publishes_signed_feeds_without_latest(self):
        self.stable_assets()
        os.environ['MODE'] = 'preview'
        # Add-ons stay on the stable feed (docs/design/18): a preview carries none.
        for addon in release.FIRST_PARTY_ADDONS:
            for platform in release.ADDON_PLATFORMS:
                for name in release.addon_asset_names(platform, addon):
                    (self.folder / name).unlink()
        calls = []
        self.lookup.side_effect = lambda *_: self.uploaded_draft() if calls else None
        def cli(*args):
            calls.append(args)
            return ''
        with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh', side_effect=cli):
            release.publish(self.folder)
        self.assertEqual([c[:2] for c in calls if c[0] == 'release'],
                         [('release', 'create'), ('release', 'upload'), ('release', 'edit')])
        self.assertIn('--latest=false', calls[-1])
        self.assertIn('--prerelease=true', calls[-1])
        uploaded = [c for c in calls if c[:2] == ('release', 'upload')][0]
        for name in ('mediaviewer-manifest.json', 'appcast.xml', 'releases.win.json'):
            self.assertTrue(any(a.endswith(name) for a in uploaded), name)
        self.assertFalse(any('mediaviewer-addon-' in a for a in uploaded))

    def test_preview_cannot_publish_installers_without_feeds(self):
        os.environ['MODE'] = 'preview'
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'full.nupkg'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_stable_cannot_publish_installers_without_feeds(self):
        os.environ['MODE'] = 'stable'
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'full.nupkg'):
                release.publish(self.folder)
            cli.assert_not_called()

    def stable_assets(self):
        os.environ.update(MODE='stable', TAG='v0.1.1', MV_RELEASE_ADDONS='1', MV_RELEASE_LINUX_CLI='1')
        for name in ('MediaViewer-0.1.1-full.nupkg', 'RELEASES', 'releases.win.json',
                     'assets.win.json', 'MediaViewer-0.1.1.zip'):
            (self.folder / name).write_bytes(b'payload')
        manifest = {'version': '0.1.1', 'channel': 'win', 'packages': [
            {'file': 'MediaViewer-0.1.1-full.nupkg', 'size': 7,
             'sha256': hashlib.sha256(b'payload').hexdigest()}]}
        (self.folder / 'mediaviewer-manifest.json').write_text(json.dumps(manifest))
        (self.folder / 'mediaviewer-manifest.json.sig').write_bytes(b'x' * 64)
        for addon in release.FIRST_PARTY_ADDONS:
            self.write_addon(addon)
        for name in release.linux_cli_asset_names('0.1.1'):
            (self.folder / name).write_bytes(b'package')
        (self.folder / 'appcast.xml').write_text('''<!-- sparkle-signatures: fixture -->
<rss xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle"><channel><item>
<enclosure url="https://github.com/owner/repo/releases/download/v0.1.1/MediaViewer-0.1.1.zip"
sparkle:edSignature="fixture" length="7" /></item></channel></rss>''')

    def test_stable_rejects_tampered_package_before_contacting_github(self):
        self.stable_assets()
        (self.folder / 'MediaViewer-0.1.1-full.nupkg').write_bytes(b'changed')
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'does not match'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_stable_needs_the_import_addon_for_both_platforms(self):
        self.stable_assets()
        (self.folder / 'mediaviewer-addon-import-macos.zip').unlink()
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'mediaviewer-addon-import-macos.zip'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_stable_needs_the_driftwire_addon_for_both_platforms(self):
        # docs/design/28: packed beside Import, and published with it: a stable
        # release that lost it on the way is refused, not shipped without it.
        self.stable_assets()
        (self.folder / 'mediaviewer-addon-driftwire-win-x64.json').unlink()
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'mediaviewer-addon-driftwire-win-x64.json'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_every_release_needs_each_driftwire_linux_package(self):
        # docs/design/28 "From a terminal": the linux-cli job's .deb, .rpm and
        # tarball for x86_64 and arm64; once they are published, a release that
        # lost one is refused, preview or stable, before GitHub hears of it.
        self.stable_assets()
        for mode in ('preview', 'stable'):
            os.environ['MODE'] = mode
            for name in release.linux_cli_asset_names('0.1.1'):
                path = self.folder / name
                path.unlink()
                with patch.object(release, 'gh') as cli:
                    with self.assertRaisesRegex(ValueError, 'Missing or empty release asset: ' + re.escape(name)):
                        release.publish(self.folder)
                    cli.assert_not_called()
                path.write_bytes(b'')
                with self.assertRaisesRegex(ValueError, 'Missing or empty'):
                    release.validate_assets(self.folder, '0.1.1', mode, 'owner/repo', 'v0.1.1')
                path.write_bytes(b'package')

    def test_preview_and_stable_upload_the_driftwire_linux_packages(self):
        self.stable_assets()
        for mode in ('preview', 'stable'):
            names = [p.name for p in release.validate_assets(self.folder, '0.1.1', mode, 'owner/repo', 'v0.1.1')]
            for name in release.linux_cli_asset_names('0.1.1'):
                self.assertIn(name, names)
        self.assertEqual(len(release.linux_cli_asset_names('0.1.1')), 6)

    def test_linux_package_names_are_the_ones_cpack_writes(self):
        # cmake/driftwire-cli/CMakeLists.txt names the files; the release must
        # ask for exactly those, or a release would wait for files never made.
        text = (Path(__file__).resolve().parents[2] / 'cmake' / 'driftwire-cli' / 'CMakeLists.txt').read_text()
        def setting(name):
            match = re.search(r'set\(' + name + r' "([^"]+)"\)', text)
            self.assertIsNotNone(match, name)
            return match[1]
        arches = re.findall(r'set\(_deb_arch (\w+)\)\s+set\(_rpm_arch (\w+)\)', text)
        self.assertEqual(tuple(arches), release.LINUX_CLI_ARCHES)
        made = []
        for deb_arch, rpm_arch in arches:
            for name, suffix in (('CPACK_DEBIAN_FILE_NAME', ''), ('CPACK_RPM_FILE_NAME', ''),
                                 ('CPACK_PACKAGE_FILE_NAME', '.tar.gz')):
                made.append(setting(name).replace('${PROJECT_VERSION}', '0.1.1')
                            .replace('${_deb_arch}', deb_arch).replace('${_rpm_arch}', rpm_arch) + suffix)
        self.assertEqual(made, release.linux_cli_asset_names('0.1.1'))

    def test_linux_packages_wait_for_the_owner(self):
        # A build that leaves this machine needs an updater first (CLAUDE.md),
        # and the Linux channel is the owner's call (docs/design/12,
        # 2026-10-23): until MV_RELEASE_LINUX_CLI=1 the packages are neither
        # required nor uploaded, even when the run made them, nor in the notes.
        self.stable_assets()
        os.environ.pop('MV_RELEASE_LINUX_CLI')
        for mode in ('preview', 'stable'):
            names = [p.name for p in release.validate_assets(self.folder, '0.1.1', mode, 'owner/repo', 'v0.1.1')]
            for name in release.linux_cli_asset_names('0.1.1'):
                self.assertNotIn(name, names)
            notes = release.release_notes('0.1.1', mode, 'owner/repo', 'v0.1.1', 'abc123')
            self.assertNotIn('driftwire', notes)
            self.assertIn('MediaViewer-0.1.1.dmg', notes)
        for name in release.linux_cli_asset_names('0.1.1'):
            (self.folder / name).unlink()
        release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')

    def test_release_notes_link_every_driftwire_linux_package(self):
        os.environ['MV_RELEASE_LINUX_CLI'] = '1'
        for mode in ('preview', 'stable'):
            notes = release.release_notes('0.1.1', mode, 'owner/repo', 'v0.1.1', 'abc123')
            for name in release.linux_cli_asset_names('0.1.1'):
                self.assertIn(f'(https://github.com/owner/repo/releases/download/v0.1.1/{name})', notes)

    def test_addons_are_not_required_until_the_workflow_packs_them(self):
        self.stable_assets()
        os.environ.pop('MV_RELEASE_ADDONS')
        for addon in release.FIRST_PARTY_ADDONS:
            for platform in release.ADDON_PLATFORMS:
                for name in release.addon_asset_names(platform, addon):
                    (self.folder / name).unlink()
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')]
        self.assertFalse(any(n.startswith('mediaviewer-addon-') for n in names))

    def test_stable_rejects_addon_archive_that_does_not_match_its_manifest(self):
        self.stable_assets()
        (self.folder / 'mediaviewer-addon-import-win-x64.zip').write_bytes(b'other')
        with self.assertRaisesRegex(ValueError, 'Add-on archive does not match'):
            release.publish(self.folder)

    def test_stable_rejects_addon_from_another_version(self):
        self.stable_assets()
        m = self.folder / 'mediaviewer-addon-import-macos.json'
        m.write_text(m.read_text().replace('"0.1.1"', '"0.1.0"'))
        with self.assertRaisesRegex(ValueError, 'not the release version'):
            release.publish(self.folder)

    def test_stable_publishes_when_places_was_not_packed(self):
        self.stable_assets()
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')]
        self.assertFalse(any('places' in name for name in names))

    def test_stable_includes_places_when_it_was_packed(self):
        self.stable_assets()
        self.write_addon('places')
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')]
        for platform in release.ADDON_PLATFORMS:
            for name in release.addon_asset_names(platform, 'places'):
                self.assertIn(name, names)

    def test_stable_includes_places_for_the_platform_that_packed_it(self):
        self.stable_assets()
        self.write_addon('places')
        for name in release.addon_asset_names('win-x64', 'places'):
            (self.folder / name).unlink()
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')]
        self.assertIn('mediaviewer-addon-places-macos.zip', names)
        self.assertNotIn('mediaviewer-addon-places-win-x64.zip', names)

    def test_stable_rejects_a_partial_places_pack(self):
        self.stable_assets()
        (self.folder / 'mediaviewer-addon-places-win-x64.zip').write_bytes(b'addon')
        with self.assertRaisesRegex(ValueError, 'Incomplete places'):
            release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')

    def test_stable_rejects_a_places_pack_from_another_version(self):
        self.stable_assets()
        self.write_addon('places')
        m = self.folder / 'mediaviewer-addon-places-macos.json'
        m.write_text(m.read_text().replace('"0.1.1"', '"0.1.0"'))
        with self.assertRaisesRegex(ValueError, 'not the release version'):
            release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')

    def test_preview_does_not_publish_places_even_when_packed(self):
        self.stable_assets()
        self.write_addon('places')
        os.environ['MODE'] = 'preview'
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'preview', 'owner/repo', 'v0.1.1')]
        self.assertFalse(any('places' in name or name.startswith('mediaviewer-addon-') for name in names))

    def test_stable_uploads_the_addon(self):
        self.stable_assets()
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')]
        for addon in release.FIRST_PARTY_ADDONS:
            for platform in release.ADDON_PLATFORMS:
                for name in release.addon_asset_names(platform, addon):
                    self.assertIn(name, names)

    def write_addon(self, addon, payload=b'addon'):
        for platform in release.ADDON_PLATFORMS:
            base = f'mediaviewer-addon-{addon}-{platform}'
            (self.folder / (base + '.zip')).write_bytes(payload)
            (self.folder / (base + '.json')).write_text(json.dumps({
                'id': addon, 'platform': platform, 'version': '0.1.1',
                'archive': {'path': base + '.zip', 'size': len(payload),
                            'sha256': hashlib.sha256(payload).hexdigest()}}))
            (self.folder / (base + '.json.sig')).write_bytes(b's' * 64)

    def ai_assets(self, addons=release.AI_ADDONS):
        os.environ['MV_RELEASE_AI'] = '1'
        for addon in addons:
            for platform in release.addon_platforms(addon):
                base = f'mediaviewer-addon-{addon}-{platform}'
                (self.folder / (base + '.zip')).write_bytes(b'models')
                (self.folder / (base + '.json')).write_text(json.dumps({
                    'id': addon, 'platform': platform, 'version': '0.1.1',
                    'archive': {'path': base + '.zip', 'size': 6,
                                'sha256': hashlib.sha256(b'models').hexdigest()}}))
                (self.folder / (base + '.json.sig')).write_bytes(b's' * 64)

    def test_stable_uploads_the_ai_pack_and_its_pieces(self):
        self.stable_assets()
        self.ai_assets()
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')]
        for addon in release.AI_ADDONS:
            for platform in release.addon_platforms(addon):
                for name in release.addon_asset_names(platform, addon):
                    self.assertIn(name, names)
        # NVIDIA acceleration is a Windows piece: the Mac never carries one.
        self.assertIn('mediaviewer-addon-ai-cuda-win-x64.zip', names)
        self.assertNotIn('mediaviewer-addon-ai-cuda-macos.zip', names)

    def test_stable_needs_the_nvidia_piece_on_windows(self):
        self.stable_assets()
        self.ai_assets(addons=('ai', 'ai-audio', 'ai-docs', 'ai-faces'))
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'mediaviewer-addon-ai-cuda-win-x64'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_stable_needs_every_ai_piece_once_the_workflow_packs_them(self):
        self.stable_assets()
        self.ai_assets(addons=('ai', 'ai-faces', 'ai-cuda'))
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'mediaviewer-addon-ai-audio-'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_ai_piece_manifest_must_name_its_own_id(self):
        self.stable_assets()
        self.ai_assets()
        m = self.folder / 'mediaviewer-addon-ai-faces-win-x64.json'
        m.write_text(m.read_text().replace('"ai-faces"', '"ai"'))
        with self.assertRaisesRegex(ValueError, 'id/platform mismatch'):
            release.publish(self.folder)

    def test_previews_carry_no_ai_pack(self):
        os.environ['MV_RELEASE_AI'] = '1'
        self.assertEqual(release.required_addons('preview'), [])

    def test_stable_rejects_appcast_pointing_to_another_release(self):
        self.stable_assets()
        feed = self.folder / 'appcast.xml'
        feed.write_text(feed.read_text().replace('/v0.1.1/', '/v0.1.0/'))
        with self.assertRaisesRegex(ValueError, 'does not reference'):
            release.publish(self.folder)

    def test_release_must_exceed_every_published_version_previews_included(self):
        self.stable_assets()
        for mode in ('preview', 'stable'):
            os.environ['MODE'] = mode
            for highest in ((0, 1, 1), (0, 1, 2)):
                self.highest.return_value = highest
                with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh') as cli:
                    with self.assertRaisesRegex(ValueError, 'must exceed'):
                        release.publish(self.folder)
                    cli.assert_not_called()


    def test_stable_publishes_complete_feed_as_latest(self):
        self.stable_assets()
        calls = []
        self.lookup.side_effect = lambda *_: self.uploaded_draft() if calls else None
        def cli(*args):
            calls.append(args)
            if args[0] == 'api':
                assets = [p for p in self.folder.iterdir() if p.name != 'release-notes.md']
                return json.dumps({'assets': [{'name': p.name, 'size': p.stat().st_size} for p in assets]})
            return ''
        with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh', side_effect=cli):
            release.publish(self.folder)
        self.assertIn('--latest=true', calls[-1])
        self.assertIn('--prerelease=false', calls[-1])


    def uploaded_draft(self):
        return {'draft': True, 'target_commitish': 'abc123', 'assets': [
            {'name': p.name, 'size': p.stat().st_size} for p in self.folder.iterdir()
            if p.name != 'release-notes.md']}

    def test_retry_resumes_same_commit_draft_without_creating_another_release(self):
        self.stable_assets()
        self.lookup.side_effect = lambda *_: self.uploaded_draft()
        with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh') as cli:
            release.publish(self.folder)
        commands = [call.args[:2] for call in cli.call_args_list]
        self.assertEqual(commands, [('release', 'upload'), ('release', 'edit')])

    def test_draft_from_different_commit_is_never_overwritten(self):
        self.stable_assets()
        self.lookup.return_value = {'draft': True, 'target_commitish': 'different'}
        with patch.object(release, 'gh') as cli, self.assertRaisesRegex(ValueError, 'another commit'):
            release.publish(self.folder)
        cli.assert_not_called()

    def test_extra_uploaded_asset_leaves_draft_unpublished(self):
        self.stable_assets()
        def lookup(*_):
            result = self.uploaded_draft()
            result['assets'].append({'name': 'unexpected.exe', 'size': 100})
            return result
        self.lookup.side_effect = lookup
        with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'leaving draft unpublished'):
                release.publish(self.folder)
        self.assertNotIn(('release', 'edit'), [call.args[:2] for call in cli.call_args_list])


if __name__ == '__main__':
    unittest.main()
