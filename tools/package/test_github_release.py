# SPDX-License-Identifier: GPL-2.0-or-later
"""Release safety checks, using temporary assets and a mocked GitHub CLI."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
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
        # Add-ons stay on the stable feed (plan/18): a preview carries none.
        for platform in release.ADDON_PLATFORMS:
            for name in release.addon_asset_names(platform):
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
        os.environ.update(MODE='stable', TAG='v0.1.1', MV_RELEASE_ADDONS='1')
        for name in ('MediaViewer-0.1.1-full.nupkg', 'RELEASES', 'releases.win.json',
                     'assets.win.json', 'MediaViewer-0.1.1.zip'):
            (self.folder / name).write_bytes(b'payload')
        manifest = {'version': '0.1.1', 'channel': 'win', 'packages': [
            {'file': 'MediaViewer-0.1.1-full.nupkg', 'size': 7,
             'sha256': hashlib.sha256(b'payload').hexdigest()}]}
        (self.folder / 'mediaviewer-manifest.json').write_text(json.dumps(manifest))
        (self.folder / 'mediaviewer-manifest.json.sig').write_bytes(b'x' * 64)
        for platform in release.ADDON_PLATFORMS:
            base = f'mediaviewer-addon-import-{platform}'
            (self.folder / (base + '.zip')).write_bytes(b'addon')
            (self.folder / (base + '.json')).write_text(json.dumps({
                'id': 'import', 'platform': platform, 'version': '0.1.1',
                'archive': {'path': base + '.zip', 'size': 5, 'sha256': hashlib.sha256(b'addon').hexdigest()}}))
            (self.folder / (base + '.json.sig')).write_bytes(b's' * 64)
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

    def test_addons_are_not_required_until_the_workflow_packs_them(self):
        self.stable_assets()
        os.environ.pop('MV_RELEASE_ADDONS')
        for platform in release.ADDON_PLATFORMS:
            for name in release.addon_asset_names(platform):
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

    def test_stable_uploads_the_addon(self):
        self.stable_assets()
        names = [p.name for p in release.validate_assets(self.folder, '0.1.1', 'stable', 'owner/repo', 'v0.1.1')]
        for platform in release.ADDON_PLATFORMS:
            for name in release.addon_asset_names(platform):
                self.assertIn(name, names)

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
