#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for homebrew_formula.py (docs/design/28 "Installing"): the formula for a release asset,
checked with `ruby -c` and evaluated against a stand-in for Homebrew's Formula class (each DSL
call recorded), the URL and checksum against an archive packed as macpack.py packs it, the
arm64-only variant, what it installs against what the archive holds, and the refusals. Homebrew
itself (brew audit, brew install) needs a Mac and is not run. The Ruby checks skip without ruby.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import homebrew_formula as formula  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'mac'))
import macpack  # noqa: E402

RUBY = shutil.which('ruby')
URL = 'https://github.com/longtimeno-c/mediaviewer/releases/download/v1.2.3/driftwire-1.2.3-macos-universal.tar.gz'
SHA = 'ab' * 32

# Homebrew's DSL, recorded: every class-level call with its arguments, as JSON.
STAND_IN = r'''
require "json"
class Formula
  def self.method_missing(name, *args, **kwargs, &block)
    (@calls ||= []) << [name.to_s, args.map(&:to_s), kwargs.to_h { |k, v| [k.to_s, v.to_s] }, !block.nil?]
    nil
  end
  def self.respond_to_missing?(*) = true
  def self.calls = @calls
  # Kernel#test would answer this one before method_missing.
  def self.test(*args, &block) = method_missing(:test, *args, &block)
end
load ARGV[0]
methods = Driftwire.instance_methods(false).map(&:to_s).sort
puts JSON.generate({ "calls" => Driftwire.calls, "methods" => methods,
                     "caveats" => Driftwire.allocate.caveats })
'''


class HomebrewFormulaTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='dwbrew-'))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def archive(self, version='1.2.3', label='universal'):
        binary = self.tmp / 'driftwire'
        binary.write_bytes(b'\xca\xfe\xba\xbe program')
        top = macpack.stage_cli(binary, version, label, self.tmp / 'stage')
        out = self.tmp / f'driftwire-{version}-macos-{label}.tar.gz'
        macpack.pack_cli_archive(top, out, mtime=1700000000)
        return out

    def evaluate(self, text):
        rb = self.tmp / 'driftwire.rb'
        rb.write_text(text)
        subprocess.run([RUBY, '-c', str(rb)], check=True, capture_output=True)
        stand_in = self.tmp / 'stand_in.rb'
        stand_in.write_text(STAND_IN)
        out = subprocess.run([RUBY, str(stand_in), str(rb)], check=True, capture_output=True, text=True).stdout
        return json.loads(out)

    def test_the_formula(self):
        text = formula.render('1.2.3', URL, sha256=SHA)
        self.assertNotIn('@', text.replace('"#{', ''))
        self.assertIn(f'  url "{URL}"\n', text)
        self.assertIn(f'  sha256 "{SHA}"\n', text)
        self.assertNotIn('depends_on arch', text)
        self.assertIn('  license "GPL-3.0-or-later"\n\n  depends_on macos: :sonoma\n\n  def install\n', text)

    @unittest.skipIf(not RUBY, 'needs ruby')
    def test_ruby_reads_it_as_homebrew_would(self):
        result = self.evaluate(formula.render('1.2.3', URL, sha256=SHA))
        calls = {name: (args, kwargs, block) for name, args, kwargs, block in result['calls']}
        self.assertEqual(calls['desc'][0], ['Send files to a computer nearby, or anywhere with a code, from a terminal'])
        self.assertEqual(calls['homepage'][0], ['https://github.com/longtimeno-c/mediaviewer'])
        self.assertEqual(calls['url'][0], [URL])
        self.assertEqual(calls['version'][0], ['1.2.3'])
        self.assertEqual(calls['sha256'][0], [SHA])
        self.assertEqual(calls['license'][0], ['GPL-3.0-or-later'])
        self.assertEqual(calls['depends_on'][1], {'macos': 'sonoma'})
        self.assertTrue(calls['test'][2])
        self.assertEqual(result['methods'], ['caveats', 'install'])
        # brew audit's rules for a description: no article first, no full stop, not the name.
        desc = calls['desc'][0][0]
        self.assertLessEqual(len(desc), 80)
        self.assertFalse(desc.startswith(('A ', 'An ')) or desc.endswith('.') or 'driftwire' in desc.lower())
        self.assertIn('firewall', result['caveats'])
        self.assertIn('Driftwire open on the other computer', result['caveats'])

    @unittest.skipIf(not RUBY, 'needs ruby')
    def test_an_arm64_archive_needs_an_arm64_mac(self):
        url = URL.replace('universal', 'arm64')
        result = self.evaluate(formula.render('1.2.3', url, sha256=SHA))
        depends = [kwargs for name, _, kwargs, _ in result['calls'] if name == 'depends_on']
        self.assertEqual(depends, [{'arch': 'arm64'}, {'macos': 'sonoma'}])

    def test_the_checksum_and_contents_of_the_archive(self):
        archive = self.archive()
        text = formula.render('1.2.3', URL, archive=archive)
        self.assertIn(f'  sha256 "{hashlib.sha256(archive.read_bytes()).hexdigest()}"\n', text)
        # What the formula installs is what macpack.py packs.
        for name in formula.INSTALLED:
            self.assertIn(f'install "{name}"', text)
        with self.assertRaisesRegex(ValueError, 'the URL names'):
            formula.render('1.2.3', URL.replace('universal', 'arm64'), archive=archive)

    def test_an_archive_without_the_manual_page_is_refused(self):
        import tarfile
        archive = self.tmp / 'driftwire-1.2.3-macos-universal.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            program = self.tmp / 'driftwire'
            program.write_bytes(b'x')
            program.chmod(0o755)
            tar.add(program, 'driftwire-1.2.3-macos-universal/driftwire')
        with self.assertRaisesRegex(ValueError, 'has no driftwire-1.2.3-macos-universal/driftwire.1'):
            formula.render('1.2.3', URL, archive=archive)

    def test_refusals(self):
        for version, url, sha, pattern in (
                ('1.2', URL, SHA, 'x.y.z'),
                ('1.2.4', URL, SHA, 'not driftwire-1.2.4'),
                ('1.2.3', URL.replace('https', 'http'), SHA, 'https URL'),
                ('1.2.3', URL + '?x=1', SHA, 'https URL'),
                ('1.2.3', URL.replace('releases', 're"leases'), SHA, 'https URL'),
                ('1.2.3', URL.replace('releases', '#{x}'), SHA, 'https URL'),
                ('1.2.3', URL.replace('universal', 'x86_64'), SHA, 'not driftwire-1.2.3'),
                ('1.2.3', URL, 'AB' * 32, 'SHA-256'),
                ('1.2.3', URL, 'ab' * 31, 'SHA-256')):
            with self.assertRaisesRegex(ValueError, pattern):
                formula.render(version, url, sha256=sha)
        with self.assertRaisesRegex(ValueError, 'one of them'):
            formula.render('1.2.3', URL)

    def test_the_tap_readme(self):
        text = formula.readme('someone/fork')
        self.assertIn('# someone/homebrew-tap', text)
        self.assertIn('brew install someone/tap/driftwire', text)
        self.assertIn('https://github.com/someone/fork/issues', text)
        self.assertNotIn('@', text)
        with self.assertRaisesRegex(ValueError, 'OWNER/NAME'):
            formula.readme('../x')

    def test_command_line_writes_the_file(self):
        archive = self.archive()
        out = self.tmp / 'tap/Formula/driftwire.rb'
        subprocess.run([sys.executable, str(Path(formula.__file__)), 'render', '--version', '1.2.3', '--url', URL,
                        '--archive', str(archive), '--out', str(out)], check=True)
        self.assertEqual(out.read_text(), formula.render('1.2.3', URL, archive=archive))
        bad = subprocess.run([sys.executable, str(Path(formula.__file__)), 'render', '--version', '1.2.3', '--url',
                              URL, '--sha256', 'nope'], capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn('Not a SHA-256', bad.stderr)


if __name__ == '__main__':
    unittest.main()
