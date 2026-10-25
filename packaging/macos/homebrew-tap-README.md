# @OWNER@/homebrew-tap

Homebrew formulae for [MediaViewer](https://github.com/@GITHUB_REPO@)'s command line.

## driftwire

Send files to a computer nearby, or anywhere with a code, from a terminal. It sends to and
receives from MediaViewer's Driftwire and another driftwire, and needs no MediaViewer.

```sh
brew install @OWNER@/tap/driftwire
```

`brew upgrade` keeps it up to date and `brew uninstall driftwire` removes it. macOS 14 or later.
The program is signed with its publisher's Developer ID and notarized by Apple.

## How this repository changes

`Formula/driftwire.rb` is written by the release workflow of
[@GITHUB_REPO@](https://github.com/@GITHUB_REPO@) for each stable release (its
`driftwire-homebrew` job): it points at that release's `driftwire-<version>-macos-universal.tar.gz`
and its SHA-256. An edit here is replaced by the next release. Questions and bugs go to
https://github.com/@GITHUB_REPO@/issues.
