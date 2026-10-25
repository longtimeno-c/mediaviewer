#!/usr/bin/env bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The owner's one-time setup that lets stable releases publish the driftwire
# command line (docs/DEVELOPMENT.md "Publishing the command line"), in one go:
#
#   1. a signing key for the apt/dnf repository -> secret DRIFTWIRE_REPO_GPG_KEY
#      (made in a throwaway GnuPG home; a backup is left for you to move offline)
#   2. GitHub Pages from GitHub Actions, and a v* tag rule on its environment
#   3. the public repository <owner>/homebrew-tap
#   4. secret HOMEBREW_TAP_TOKEN (you create the token in the browser; GitHub
#      offers no API to mint a fine-grained token)
#   5. variable MV_DRIFTWIRE_REPO=1
#
# Run it on your own machine, signed in with `gh auth login` as an admin of the
# repository. Each step checks first and skips what is already done, so it is
# safe to run again. Windows signing (the Trusted Signing secrets) is optional
# and not done here: RELEASING.md "Optional Windows publisher signing".
#
#   tools/package/setup-driftwire-publishing.sh [--repo owner/name] [--dry-run]
set -euo pipefail

repo="longtimeno-c/mediaviewer"
dry=0
while [ $# -gt 0 ]; do
  case "$1" in
    --repo) repo="$2"; shift 2 ;;
    --dry-run) dry=1; shift ;;
    -h|--help) sed -n '4,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
owner="${repo%%/*}"
tap="$owner/homebrew-tap"

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
run() {
  if [ "$dry" = 1 ]; then printf '  (dry run) %s\n' "$*"; else "$@"; fi
}

for tool in gh gpg; do
  command -v "$tool" >/dev/null || { echo "needs $tool on PATH" >&2; exit 2; }
done
gh auth status >/dev/null 2>&1 || { echo "sign in first: gh auth login" >&2; exit 2; }
if [ "$(gh api "repos/$repo" --jq .permissions.admin 2>/dev/null)" != "true" ]; then
  echo "you need admin rights on $repo" >&2
  exit 2
fi

# ---- 1. the repository signing key
say "1. The apt/dnf repository's signing key"
if gh secret list --repo "$repo" --json name --jq '.[].name' | grep -qx DRIFTWIRE_REPO_GPG_KEY; then
  echo "  DRIFTWIRE_REPO_GPG_KEY is already set: kept (delete it first to make a new key)"
elif [ "$dry" = 1 ]; then
  echo "  (dry run) make an RSA-4096 key in a throwaway GnuPG home, gh secret set DRIFTWIRE_REPO_GPG_KEY,"
  echo "  and leave a backup in ~/driftwire-signing-key-<date>"
else
  backup="$HOME/driftwire-signing-key-$(date +%Y%m%d-%H%M%S)"
  GNUPGHOME="$(mktemp -d)"
  export GNUPGHOME
  trap 'gpgconf --kill gpg-agent >/dev/null 2>&1 || true; rm -rf "$GNUPGHOME"' EXIT
  gpg --batch --passphrase '' --quick-gen-key \
    "Driftwire packages <$owner@users.noreply.github.com>" rsa4096 sign never 2>/dev/null
  fpr="$(gpg --list-secret-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }')"
  echo "  made key $fpr"
  (
    umask 077  # the private key and the revocation certificate: this user only
    mkdir -p "$backup"
    gpg --armor --export-secret-keys "$fpr" > "$backup/driftwire-repo-key.asc"
    gpg --armor --export "$fpr" > "$backup/driftwire-repo-key.pub.asc"
    cp "$GNUPGHOME"/openpgp-revocs.d/*.rev "$backup/" 2>/dev/null || true
  )
  gh secret set DRIFTWIRE_REPO_GPG_KEY --repo "$repo" < "$backup/driftwire-repo-key.asc"
  echo "  backup in $backup: move it somewhere offline (a USB stick, a password manager),"
  echo "  then delete it from this disk. Losing it means a new key and every user re-running the installer."
fi

# ---- 2. GitHub Pages from Actions, and the v* tag rule
say "2. GitHub Pages (Source: GitHub Actions) and a v* tag rule"
if gh api "repos/$repo/pages" >/dev/null 2>&1; then
  run gh api --silent -X PUT "repos/$repo/pages" -f build_type=workflow
  echo "  Pages was on: its source is now GitHub Actions"
else
  run gh api --silent -X POST "repos/$repo/pages" -f build_type=workflow
  echo "  Pages turned on, source GitHub Actions"
fi
# A release may run from its tag, so the github-pages environment must allow
# v* tags as well as the default branch.
default_branch="$(gh api "repos/$repo" --jq .default_branch)"
run gh api --silent -X PUT "repos/$repo/environments/github-pages" --input - <<JSON
{"deployment_branch_policy": {"protected_branches": false, "custom_branch_policies": true}}
JSON
existing="$(gh api "repos/$repo/environments/github-pages/deployment-branch-policies" \
  --jq '.branch_policies[] | "\(.type):\(.name)"' 2>/dev/null || true)"
grep -qxF "branch:$default_branch" <<<"$existing" ||
  run gh api --silent -X POST "repos/$repo/environments/github-pages/deployment-branch-policies" \
    -f name="$default_branch" -f type=branch
grep -qxF 'tag:v*' <<<"$existing" ||
  run gh api --silent -X POST "repos/$repo/environments/github-pages/deployment-branch-policies" \
    -f name='v*' -f type=tag
echo "  github-pages deploys from $default_branch and v* tags"

# ---- 3. the Homebrew tap
say "3. The Homebrew tap, $tap"
if gh repo view "$tap" >/dev/null 2>&1; then
  echo "  already there"
else
  run gh repo create "$tap" --public --add-readme \
    --description "Homebrew formulae for MediaViewer's command line"
fi

# ---- 4. the tap's token
say "4. The tap's token, HOMEBREW_TAP_TOKEN"
if gh secret list --repo "$repo" --json name --jq '.[].name' | grep -qx HOMEBREW_TAP_TOKEN; then
  echo "  already set: kept"
else
  cat <<EOF
  GitHub has no API for this one. In the browser:
    https://github.com/settings/personal-access-tokens/new
  Token name: driftwire homebrew tap   Resource owner: $owner
  Repository access: Only select repositories -> $tap
  Permissions: Repository -> Contents: Read and write (nothing else)
  Pick an expiry and put the date in your calendar: an expired token stops the formula updating.
EOF
  if [ "$dry" = 1 ]; then
    echo "  (dry run) gh secret set HOMEBREW_TAP_TOKEN --repo $repo"
  else
    printf '  Paste the token (input hidden), or press Enter to skip for now: '
    # From the terminal itself, so nothing piped into the script answers for you.
    token=""
    if { : </dev/tty; } 2>/dev/null; then IFS= read -rs token </dev/tty || true; else IFS= read -rs token || true; fi
    echo
    if [ -n "$token" ]; then
      printf '%s' "$token" | gh secret set HOMEBREW_TAP_TOKEN --repo "$repo"
      unset token
      echo "  set"
    else
      echo "  skipped: run this script again once you have it (releases refuse to publish without it)"
      exit 1
    fi
  fi
fi

# ---- 5. the switch
say "5. The switch, MV_DRIFTWIRE_REPO=1"
run gh variable set MV_DRIFTWIRE_REPO --body 1 --repo "$repo"

say "Done."
echo "  The next stable release publishes the apt/dnf repository and the Windows installer to"
echo "  https://$owner.github.io/${repo#*/}/driftwire/ and the formula to $tap."
echo "  Optional: Trusted Signing secrets so driftwire.exe is signed (RELEASING.md)."
