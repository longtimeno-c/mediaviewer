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
`ProductionPublicKeyHex` (32 bytes, lowercase hex). The production public key is
already pinned. Use its matching existing private key; do not regenerate it for CI.
An all-zero placeholder would fail closed with `KeyNotConfigured`.

For a new installation of this signing scheme only, the owner:

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

---

# Authenticode: the wizard, the binaries and the bundle (PR 8)

The Ed25519 key above signs the update *manifest*. It says nothing about
whether Windows trusts the executables. That is Authenticode, it is a separate
credential. Without it Windows SmartScreen may warn on first installation.
Authenticode is optional for GitHub publishing and does not replace manifest signing.

The workflow supports Azure Artifact Signing (formerly Trusted Signing). See
[RELEASING.md](../../RELEASING.md) for optional credentials and publication modes.

## What must be signed

| Artefact | Signed by |
|---|---|
| Every `.exe` and `.dll` in the payload | `vpk pack`, from `--azureTrustedSignFile` |
| `MediaViewer-win-Setup.exe` (the Velopack bundle) | `vpk pack`, same flag |
| `Update.exe` | `vpk pack`, same flag |
| `MediaViewer-<version>-Setup.exe` (the Inno wizard) | a separate `signtool` step, after ISCC |
| `mediaviewer-manifest.json` | the Ed25519 key above — *not* Authenticode |

The wizard is signed after ISCC rather than through Inno's `SignTool=`
directive. Two reasons: the wizard wraps the already-signed Velopack bundle, so
it has to be built last anyway, and a release pipeline should hold the signing
credential for one step rather than for the whole compile.

## The seam

`tools/package/build-release.ps1` takes **one** of:

* `-SigningMetadata <path to trusted-signing metadata.json>` — Azure Trusted
  Signing, the intended production path;
* `-SignParams "<signtool arguments>"` — an EV certificate or a local test
  certificate.

Given neither, it prints an UNSIGNED BUILD warning three lines tall and carries
on, because a local packaging change must be testable without a credential. It
never fabricates a certificate and never quietly skips the step: an unsigned
artefact says so on the last line of its own output.

`metadata.json` for Azure Trusted Signing looks like this. It contains no
secret — authentication is the Azure identity of whoever runs the build — so it
may live in the pipeline, but not in this repo, because the endpoint and
account names are deployment details rather than source:

```json
{
  "Endpoint": "https://eus.codesigning.azure.net/",
  "CodeSigningAccountName": "<account>",
  "CertificateProfileName": "<profile>",
  "CorrelationId": "mediaviewer-<version>"
}
```

## Signing the wizard

```powershell
signtool sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 `
  /dlib <Azure.CodeSigning.Dlib.dll> /dmdf <metadata.json> `
  dist\MediaViewer-<version>-Setup.exe
```

Timestamping is not optional. Without `/tr`, every signature expires with the
certificate and installers already in the wild start failing.

## What a human still has to do once

1. Store the existing Ed25519 private key in the release secret store and keep
   an offline backup. The matching public half is already pinned in `UpdateKeys.cs`.
2. Optionally enrol an Azure signing account and certificate profile, and grant
   a release identity signing permission. Store all six Azure credentials from
   the runbook when enabling this path.

The production public key is already configured. Stable CI requires its matching private
key as `MV_MANIFEST_SIGNING_KEY` and verifies the generated feed before publication.
Azure is optional: the update feed can still be trusted without a publisher signature on
executables. Preview publishes test installers without replacing the stable update feed.
See [RELEASING.md](../../RELEASING.md).
