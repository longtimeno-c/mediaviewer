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

    def test_authentication_failure_is_not_treated_as_missing_release(self):
        with patch.object(release, 'gh', side_effect=subprocess.CalledProcessError(1, 'gh', stderr='HTTP 403')):
            with self.assertRaises(subprocess.CalledProcessError):
                release.release_by_tag('owner/repo', 'v0.1.1')


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.env = {'MODE': 'preview', 'VERSION': '0.1.1', 'TAG': 'v0.1.1.preview.1',
                    'GITHUB_REPOSITORY': 'owner/repo', 'GITHUB_SHA': 'abc123',
                    'GITHUB_RUN_ID': '1', 'MIN_VERSION': '0.0.0', 'BLOCKLIST': '',
                    'GITHUB_OUTPUT': str(self.folder / 'output')}
        self.addCleanup(patch.stopall)
        patch.dict(os.environ, self.env, clear=True).start()
        patch.object(release, 'project_version', return_value='0.1.1').start()
        self.lookup = patch.object(release, 'release_by_tag', return_value=None).start()
        for name in ('MediaViewer-0.1.1-Setup.exe', 'MediaViewer-0.1.1.dmg'):
            (self.folder / name).write_bytes(b'test installer')

    def test_preview_preflight_needs_no_secrets(self):
        release.prepare()
        self.assertIn('tag=v0.1.1.preview.1', (self.folder / 'output').read_text())

    def test_stable_preflight_fails_before_build_without_credentials(self):
        os.environ['MODE'] = 'stable'
        with self.assertRaisesRegex(ValueError, 'MV_MANIFEST_SIGNING_KEY'):
            release.prepare()

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
        (self.folder / 'MediaViewer-0.1.1.dmg').unlink()
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'Missing or empty'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_published_release_is_never_overwritten(self):
        self.lookup.return_value = {'draft': False}
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'Refusing to overwrite'):
                release.publish(self.folder)
            cli.assert_not_called()

    def test_upload_failure_leaves_draft_unpublished(self):
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

    def test_preview_publishes_only_after_upload_verification_without_latest(self):
        calls = []
        self.lookup.side_effect = lambda *_: self.uploaded_draft() if calls else None
        def cli(*args):
            calls.append(args)
            if args[0] == 'api':
                assets = [p for p in self.folder.iterdir() if p.suffix in ('.exe', '.dmg') or p.name == 'SHA256SUMS.txt']
                return json.dumps({'assets': [{'name': p.name, 'size': p.stat().st_size} for p in assets]})
            return ''
        with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh', side_effect=cli):
            release.publish(self.folder)
        self.assertEqual([c[:2] for c in calls if c[0] == 'release'],
                         [('release', 'create'), ('release', 'upload'), ('release', 'edit')])
        self.assertIn('--latest=false', calls[-1])
        self.assertIn('--prerelease=true', calls[-1])

    def test_stable_cannot_publish_installers_without_feeds(self):
        os.environ['MODE'] = 'stable'
        with patch.object(release, 'gh') as cli:
            with self.assertRaisesRegex(ValueError, 'full.nupkg'):
                release.publish(self.folder)
            cli.assert_not_called()

    def stable_assets(self):
        os.environ.update(MODE='stable', TAG='v0.1.1')
        for name in ('MediaViewer-0.1.1-full.nupkg', 'RELEASES', 'releases.win.json',
                     'assets.win.json', 'MediaViewer-0.1.1.zip'):
            (self.folder / name).write_bytes(b'payload')
        manifest = {'version': '0.1.1', 'channel': 'win', 'packages': [
            {'file': 'MediaViewer-0.1.1-full.nupkg', 'size': 7,
             'sha256': hashlib.sha256(b'payload').hexdigest()}]}
        (self.folder / 'mediaviewer-manifest.json').write_text(json.dumps(manifest))
        (self.folder / 'mediaviewer-manifest.json.sig').write_bytes(b'x' * 64)
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

    def test_stable_rejects_appcast_pointing_to_another_release(self):
        self.stable_assets()
        feed = self.folder / 'appcast.xml'
        feed.write_text(feed.read_text().replace('/v0.1.1/', '/v0.1.0/'))
        with self.assertRaisesRegex(ValueError, 'does not reference'):
            release.publish(self.folder)

    def test_stable_cannot_move_latest_backwards(self):
        self.stable_assets()
        with patch.object(release, 'api_optional', side_effect=[None, {'tag_name': 'v0.1.2'}]), patch.object(release, 'gh') as cli:
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
        self.lookup.side_effect = lambda *_: self.uploaded_draft()
        with patch.object(release, 'api_optional', return_value=None), patch.object(release, 'gh') as cli:
            release.publish(self.folder)
        commands = [call.args[:2] for call in cli.call_args_list]
        self.assertEqual(commands, [('release', 'upload'), ('release', 'edit')])

    def test_draft_from_different_commit_is_never_overwritten(self):
        self.lookup.return_value = {'draft': True, 'target_commitish': 'different'}
        with patch.object(release, 'gh') as cli, self.assertRaisesRegex(ValueError, 'another commit'):
            release.publish(self.folder)
        cli.assert_not_called()

    def test_extra_uploaded_asset_leaves_draft_unpublished(self):
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
