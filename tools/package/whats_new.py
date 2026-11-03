#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""The short "What's new" an update carries (#620, docs/design/13 "What's new").

At most three lines and 400 bytes of printable text, no links. Windows signs it
into the update manifest's `notes`; the Mac embeds it in the signed appcast as
the item's plain-text description. Neither app makes a request for it.

The text is the release's own words when the run was given them (release.yml's
`whats_new` input), else the subjects of the commits since the previous release
(a pull request's merge commit gives its title).

    whats_new.py [--text-file F] [--repo DIR] [--version X.Y.Z]   print it

The rule here is the one MediaViewer.Updater's ManifestNotes applies before it
trusts a manifest: keep the two identical.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

MAX_BYTES = 400
MAX_LINES = 3
# One line of a tooltip; longer subjects end in an ellipsis.
MAX_LINE_CHARS = 120
ELLIPSIS = '\u2026'
LINK_MARKERS = ('://', 'www.', 'mailto:')


def forbidden(ch):
    """Characters a notes line may not hold: controls, and invisible ones that
    could reorder or hide text. A fixed list, so every runtime agrees."""
    c = ord(ch)
    return (c < 0x20 or c == 0x7F or 0x80 <= c <= 0x9F or c == 0x061C
            or 0x200B <= c <= 0x200F or 0x2028 <= c <= 0x202E or 0x2060 <= c <= 0x2069
            or c == 0xFEFF or 0xD800 <= c <= 0xDFFF)


def valid(text):
    """The manifest rule: 1-3 non-empty lines joined by \\n, <= 400 UTF-8 bytes,
    no forbidden character, no leading or trailing space on a line, no link."""
    if not isinstance(text, str) or not text:
        return False
    try:
        size = len(text.encode('utf-8'))
    except UnicodeEncodeError:  # a lone surrogate
        return False
    if size > MAX_BYTES:
        return False
    lines = text.split('\n')
    if len(lines) > MAX_LINES:
        return False
    for line in lines:
        if not line or line[0] == ' ' or line[-1] == ' ':
            return False
        if any(forbidden(ch) for ch in line):
            return False
    lowered = text.lower()
    return not any(marker in lowered for marker in LINK_MARKERS)


def clean_line(text):
    """One line made safe: link-like words dropped, forbidden characters and
    runs of space become one space, trimmed."""
    words = []
    for word in re.split(r'\s+', text):
        lowered = word.lower()
        if any(marker in lowered for marker in LINK_MARKERS):
            continue
        words.append(''.join(' ' if forbidden(ch) else ch for ch in word))
    return re.sub(r' +', ' ', ' '.join(words)).strip()


def _shorten(line, chars):
    if len(line) <= chars:
        return line
    return line[:max(1, chars - 1)].rstrip() + ELLIPSIS


def fit(items):
    """Up to three lines from the items, most important first. More than three:
    two of them and "and N more changes"."""
    items = [line for line in (clean_line(i) for i in items) if line]
    if not items:
        return ''
    if len(items) > MAX_LINES:
        rest = len(items) - (MAX_LINES - 1)
        items = items[:MAX_LINES - 1] + [f'and {rest} more changes']
    lines = [_shorten(line, MAX_LINE_CHARS) for line in items]
    # Over the byte bound (long non-ASCII lines): trim the longest until it fits.
    while len('\n'.join(lines).encode('utf-8')) > MAX_BYTES:
        i = max(range(len(lines)), key=lambda k: len(lines[k].encode('utf-8')))
        lines[i] = _shorten(lines[i], len(lines[i]) - 4)
    text = '\n'.join(lines)
    assert valid(text), text
    return text


def from_text(text):
    """The release's own words: its non-empty lines, in order."""
    return fit(line for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n'))


# Commits that are about the release, not what changed in it.
NOISE = re.compile(r'^(merge (branch|remote-tracking branch)\b|updated? version\b|bump (the )?version\b)', re.I)
PR_MERGE = re.compile(r'^Merge pull request #\d+ from \S+$')


def commit_items(log):
    """git log records (subject, body), newest first, as lines: a pull request's
    merge gives its title, the first line of the body."""
    items = []
    for subject, body in log:
        subject = subject.strip()
        if PR_MERGE.match(subject):
            title = next((line.strip() for line in body.splitlines() if line.strip()), '')
            if title:
                items.append(title)
            continue
        if subject and not NOISE.match(subject):
            items.append(subject)
    return items


def _git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, text=True,
                          capture_output=True).stdout


def _version(tag):
    m = re.fullmatch(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:\.preview\.[0-9]+)?', tag)
    return tuple(map(int, m.groups())) if m else None


def previous_release_tag(repo, version=None):
    """The highest release tag in HEAD's history, other than this release's own."""
    current = _version('v' + version) if version else None
    tags = [(v, t) for t in _git(repo, 'tag', '--merged', 'HEAD', '--list', 'v*').split()
            if (v := _version(t)) and v != current]
    return max(tags)[1] if tags else None


def from_git(repo, version=None, limit=50):
    since = previous_release_tag(repo, version)
    rng = f'{since}..HEAD' if since else 'HEAD'
    out = _git(repo, 'log', '--first-parent', f'--max-count={limit}', '--format=%s%x00%b%x1e', rng)
    log = []
    for record in out.split('\x1e'):
        record = record.strip('\n')
        if record:
            subject, _, body = record.partition('\x00')
            log.append((subject, body))
    return fit(commit_items(log))


def whats_new(text=None, repo='.', version=None):
    """The notes for a release: its own words if given, else its commits. Never
    fails a release: no history gives an empty string (an update without notes)."""
    if text and text.strip():
        return from_text(text)
    try:
        return from_git(repo, version)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"::warning::What's new: no commit history ({type(error).__name__})", file=sys.stderr)
        return ''


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--text-file', help="the release's own words (else the commit subjects)")
    p.add_argument('--repo', default='.')
    p.add_argument('--version', help="this release's x.y.z, so its own tag is never the previous one")
    args = p.parse_args(argv)
    text = Path(args.text_file).read_text(encoding='utf-8') if args.text_file else None
    sys.stdout.write(whats_new(text, args.repo, args.version))


if __name__ == '__main__':
    main(sys.argv[1:])
