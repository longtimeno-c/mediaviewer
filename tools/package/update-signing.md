# Update manifest signing (PR 8)

The in-app updater trusts nothing from the update channel until the signed
manifest verifies against a public key compiled into the app
([plan/13](../../plan/13-updates-and-telemetry.md), "Signing").

## What is signed

Release assets on `longtimeno-c/mediaviewer`:

| Asset | Content |
|---|---|
| `mediaviewer-manifest.json` | Schema 1: `schema`, `channel` (`win`), `version`, `min_version`, `blocklist`, `released_at`, `packages[] {file, sha256, size, kind: full\|delta}` |
| `mediaviewer-manifest.json.sig` | 64-byte raw Ed25519 signature, detached, over the exact bytes of the JSON file |

The updater rejects, before asking Velopack for anything: a missing or bad
signature, the placeholder key, a malformed manifest, `schema != 1`, a channel
other than `win`, a version below the running one, a version on its own
blocklist, a version this machine already rolled back, and a manifest with no
full package. Every downloaded package must match its entry's size and SHA-256.
A running version on the blocklist (or below `min_version`) is offered the
newest good version as an "Important update"; it is never force-restarted.

## The pinned key

`src.managed/MediaViewer.Updater/UpdateKeys.cs`, constant
`ProductionPublicKeyHex` (32 bytes, lowercase hex). **It ships as an all-zero
placeholder**, which makes every manifest fail with `KeyNotConfigured` — an
unconfigured build fails closed.

Before the first external build, the owner:

1. Generates the release keypair on a trusted machine (not CI):
   `dotnet run --project src.managed/MediaViewer.Updater.Tests -c Release -- keygen <offline-dir>`
   writes `dev.key` (private, hex) and `dev.pub` (public, hex). Any Ed25519
   tool producing a raw 32-byte public key works equally well.
2. Stores the private key offline / in the release signer's secret store. It
   never goes in git, in a CI log, or on a build agent that runs PR code.
3. Pastes the public hex into `ProductionPublicKeyHex` and commits that.
4. Signs each release's manifest over its exact bytes:
   `... -- sign <releases>/mediaviewer-manifest.json <private-key-file>`.

Rotating the key means shipping a build with the new public key signed under
the old one first; there is no in-band key update.

## Development key and the local feed

Tests generate a throwaway keypair in memory on every run; no test key is
committed. The local end-to-end check (`tools/package/e2e-update.ps1`) needs a
**dev build** of the chrome: `-p:MvUpdaterDev=true`. Only that build honours

- `MV_UPDATE_FEED_DIR` — a directory with `releases.win.json`, the `.nupkg`
  files, the manifest and its `.sig`, instead of GitHub;
- `MV_UPDATE_DEV_PUBKEY` — the dev public key instead of the pinned one;
- `MV_UPDATE_CHECK_DELAY_MS` — the first-check delay (default 30 s).

A normal Debug or Release build compiles all three out. The signature check is
the same code path for the local feed as for GitHub, so neither variable is an
unsigned bypass.
