# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""#620: the update's short "What's new" (whats_new.py), against a scratch git history."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import whats_new  # noqa: E402


class RuleTests(unittest.TestCase):
    """The rule MediaViewer.Updater's ManifestNotes applies (Program.cs tests the same cases)."""

    def test_accepts_short_printable_text(self):
        self.assertTrue(whats_new.valid('Faster RAW previews'))
        self.assertTrue(whats_new.valid('One\nTwo\nThree'))
        self.assertTrue(whats_new.valid('Café — HEIC thumbnails, “quoted” and 日本語'))
        self.assertTrue(whats_new.valid('x' * 400))

    def test_rejects_what_the_manifest_rejects(self):
        for bad in ('', 'x' * 401, 'é' * 201, 'a\nb\nc\nd', 'a\n\nb', ' lead', 'trail ', 'a\r\nb',
                    'tab\there', 'bell\x07', 'del\x7f', 'c1\x85', 'rtl\u202eevil', 'iso\u2066x',
                    'zero\u200bwidth', 'bom\ufeff', 'line\u2028sep', 'https://example.com',
                    'see WWW.example.com', 'mailto:me', 'ftp://x', 'lone \ud800 surrogate'):
            self.assertFalse(whats_new.valid(bad), repr(bad))
        self.assertFalse(whats_new.valid(None))


class FitTests(unittest.TestCase):
    def test_three_items_are_three_lines(self):
        self.assertEqual(whats_new.fit(['a', 'b', 'c']), 'a\nb\nc')

    def test_more_than_three_says_how_many_more(self):
        self.assertEqual(whats_new.fit(['a', 'b', 'c', 'd', 'e']), 'a\nb\nand 3 more changes')

    def test_links_and_controls_are_dropped(self):
        self.assertEqual(whats_new.fit(['Fix see https://x.example/y now', 'tab\there\u202e']),
                         'Fix see now\ntab here')

    def test_long_lines_are_shortened_and_bytes_bounded(self):
        text = whats_new.fit(['é' * 300, 'ü' * 300, '日' * 300])
        self.assertTrue(whats_new.valid(text))
        self.assertLessEqual(len(text.encode('utf-8')), 400)
        self.assertTrue(all(line.endswith('…') for line in text.split('\n')))

    def test_nothing_left_is_no_notes(self):
        self.assertEqual(whats_new.fit(['https://only.a/link', '   ']), '')

    def test_release_text_keeps_its_lines(self):
        self.assertEqual(whats_new.whats_new('Faster pans\r\n\r\nHEIC fix\n'), 'Faster pans\nHEIC fix')


class GitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.repo = Path(self.temp.name)
        env = dict(os.environ, GIT_AUTHOR_NAME='t', GIT_AUTHOR_EMAIL='t@example.invalid',
                   GIT_COMMITTER_NAME='t', GIT_COMMITTER_EMAIL='t@example.invalid')
        self.git = lambda *a: subprocess.run(['git', '-C', str(self.repo), *a], check=True,
                                             capture_output=True, text=True, env=env).stdout
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'commit.gpgsign', 'false')
        self.git('config', 'tag.gpgsign', 'false')

    def commit(self, message):
        self.git('commit', '-q', '--allow-empty', '-m', message)

    def test_commits_since_the_previous_release(self):
        self.commit('Old change')
        self.git('tag', 'v0.1.0')
        self.commit('canvas: smoother pans')
        # A pull request's merge: its title is the body's first line.
        self.git('checkout', '-q', '-b', 'feature')
        self.commit('wip')
        self.git('checkout', '-q', 'main')
        self.git('merge', '-q', '--no-ff', 'feature', '-m',
                 'Merge pull request #12 from owner/feature\n\nHEIC thumbnails load sooner')
        self.commit('updated version')
        self.assertEqual(whats_new.from_git(self.repo, '0.1.1'),
                         'HEIC thumbnails load sooner\ncanvas: smoother pans')

    def test_own_tag_is_not_the_previous_release(self):
        self.commit('First')
        self.git('tag', 'v0.1.0')
        self.commit('Second')
        self.git('tag', 'v0.1.1')  # a re-run of this release after the tag exists
        self.assertEqual(whats_new.from_git(self.repo, '0.1.1'), 'Second')

    def test_no_history_is_no_notes(self):
        self.assertEqual(whats_new.whats_new(None, self.repo / 'missing', '0.1.1'), '')


if __name__ == '__main__':
    unittest.main()
