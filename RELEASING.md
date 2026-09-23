# Releasing MediaViewer

How a build gets to a person, and how the next one reaches them without asking.

Full design: [plan/13](plan/13-updates-and-telemetry.md). Signing procedure:
[tools/package/update-signing.md](tools/package/update-signing.md).

## The model in one paragraph

**The wizard is a one-time download.** Someone installs `MediaViewer-<version>-Setup.exe`
once, and from then on the app updates itself: it checks GitHub on launch and every six
hours, downloads and stages in the background, and shows a quiet *"Update ready —
restart"*. No wizard, no download page, no clicking through an installer again. Updates
never interrupt — not during playback, not during a job. If the new version fails to start
twice, the stub falls back to the previous one.

```
 first install              every update after
 ─────────────              ──────────────────
 GitHub Release             GitHub Release
   MediaViewer-Setup.exe      mediaviewer-manifest.json(.sig)   ← trust root
        ↓                     MediaViewer-<v>-full.nupkg        ← the payload
   wizard runs once           RELEASES / releases.win.json      ← the index
        ↓                              ↓
 %LocalAppData%\MediaViewer    app downloads, verifies, stages
                                       ↓
                               "Update ready — restart"
```

## What a release must contain

The wizard is **not** the update. Upload only the wizard and existing users get nothing —
they stay on the version they have, silently. Every release needs the feed files too.

A local build writes all of them into `dist/`:

| Asset | Needed for | Purpose |
|---|---|---|
| `MediaViewer-<version>-Setup.exe` | first install | the Inno wizard |
| `MediaViewer-<version>-full.nupkg` | updates | the payload the app downloads |
| `RELEASES`, `releases.win.json`, `assets.win.json` | updates | Velopack's index |
| `mediaviewer-manifest.json` | updates | version, min_version, blocklist, hashes |
| `mediaviewer-manifest.json.sig` | updates | detached Ed25519 signature — **without it every client refuses** |

The updater reads `https://github.com/longtimeno-c/mediaviewer/releases/latest/download/`,
so the release carrying these must be the **latest** one on GitHub.

## Cutting a release

### 1. Bump the version

`CMakeLists.txt` is the single source. **The version must increase every release** — the
updater compares against the running version, so a release at the same number is invisible
to everyone who already has it.

```cmake
project(mediaviewer VERSION 0.1.1 LANGUAGES C CXX)
```

Versions are strict `x.y.z`. Not `0.1.1-beta`, not `0.1.1.4`: `ReleaseVersion` parses three
numeric parts and nothing else, and a version it cannot parse makes the updater go **inert**
rather than fail loudly.

### 2. Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### 3. Package and sign

```powershell
.\tools\package\build-release.ps1 -BuildDir build `
  -ManifestKey $env:USERPROFILE\.mediaviewer-release\dev.key `
  -SigningMetadata trusted-signing.json      # optional; see "SmartScreen" below
```

It refuses to continue if the app exceeds plan/09's 250 MB cap, fails plan/11's licence
gate, or contains the Windows App SDK AI / ONNX / DirectML / WebView2 files plan/13 forbids
shipping. It prints `manifest signed` when the key took, and warns loudly when it did not.

### 4. Publish

Everything in `dist\releases\`, plus the wizard:

```powershell
$v = "0.1.1"
gh release create "v$v" --title "MediaViewer $v" `
  (Get-ChildItem dist\releases -File).FullName `
  "dist\MediaViewer-$v-Setup.exe"
```

Existing installs pick it up within six hours, or on their next launch.

## Signing: two separate credentials

They are unrelated, and they fail in different ways.

### Update manifest — Ed25519 (free, already set up)

Proves an update came from you. The public half is pinned in
`src.managed/MediaViewer.Updater/UpdateKeys.cs`; the private half lives at
`%USERPROFILE%\.mediaviewer-release\dev.key` and must never enter git or CI.

> **Back this key up.** There is no in-band key rotation. Rotating means shipping a build
> carrying the new public key, signed under the old one, *first*. Lose the private key and
> every existing install is permanently unreachable — they keep working, they just never
> update again, and the only fix is asking each person to reinstall by hand.

### Authenticode — SmartScreen (paid, not yet set up)

Proves Windows should trust the executable. Without it every early user gets a SmartScreen
block on first run. Use Azure Trusted Signing (plan/13) and pass `-SigningMetadata`.

This is an enrolled identity, not a build step: building locally does not substitute for
it. Updates still work unsigned — only the first-run warning is affected.

## Status

| | |
|---|---|
| Wizard, per-user, no UAC | ✅ built and tested |
| Uninstall via Apps & features | ✅ tested; wizard owns it, not Velopack |
| Bundled runtimes (no prerequisites) | ✅ App SDK + .NET in the payload |
| Update: download, stage, restart | ✅ end-to-end 16/16 locally |
| Update: tamper rejection, rollback | ✅ tested |
| Ed25519 manifest signing | ✅ key generated, public key pinned |
| Authenticode / SmartScreen | ❌ needs Azure Trusted Signing |
| Clean-VM verify | ❌ not run |

## macOS

Same model, different machinery: Sparkle instead of Velopack, a notarized `.dmg` instead of
the wizard, and `appcast.xml` instead of the manifest — reading the same
`/releases/latest/download/`.

**CI builds it.** The `macOS` job in `.github/workflows/release.yml` runs after the Windows
job on every push to `main`: build, test, sign with Developer ID, notarize, staple, then
attach to the same GitHub Release:

| Asset | Purpose |
|---|---|
| `MediaViewer-<version>.dmg` | first install: drag to Applications, GPL shown on mount |
| `MediaViewer-<version>.zip` | what Sparkle installs on every later update |
| `appcast.xml` | signed feed the app reads; **every release must carry its own** |

Every push makes a new "latest" release, and the app reads `latest/download/appcast.xml`,
so a release without the feed would strand Mac users until the next one. The job attaches
all three together, or none.

### One-time setup: repository secrets

The job needs six secrets. Without them it still builds, but the image is ad-hoc signed and
is **not** attached to the release (a tagged release fails outright instead).

| Secret | What | How to get it |
|---|---|---|
| `MV_MAC_CERT_P12_BASE64` | Developer ID Application certificate **and its private key** | Keychain Access → My Certificates → right-click *Developer ID Application: …* → Export → `.p12`, set a password |
| `MV_MAC_CERT_PASSWORD` | that `.p12` password | you chose it |
| `APPLE_ID` | Apple ID that owns the developer account | |
| `APPLE_TEAM_ID` | 10-character Team ID | developer.apple.com → Membership |
| `APPLE_APP_PASSWORD` | app-specific password for notarization | appleid.apple.com → Sign-In and Security |
| `MV_SPARKLE_PRIVATE_KEY` | Sparkle EdDSA private key | `generate_keys -x file` (below) |

```sh
base64 -i DeveloperID.p12 | gh secret set MV_MAC_CERT_P12_BASE64
gh secret set MV_MAC_CERT_PASSWORD
gh secret set APPLE_ID
gh secret set APPLE_TEAM_ID --body 23FQ4A4Q35
gh secret set APPLE_APP_PASSWORD
gh secret set MV_SPARKLE_PRIVATE_KEY < ~/.mediaviewer-release/sparkle_ed25519.key
rm DeveloperID.p12          # the private key must not stay on disk
```

`gh secret set` with no value prompts for it, so nothing lands in shell history. The Sparkle
**public** key and the signing identity name are not secret and live in the workflow's `env:`.

> **Back up the Sparkle private key** (`~/.mediaviewer-release/sparkle_ed25519.key`,
> also in your login keychain). It is the Mac twin of the Windows manifest key: lose it and
> no installed copy can ever accept another update. Rotating it means shipping a build with
> the new public key first, signed under the old one.

The certificate and its private key are the Mac-side identity of the developer account, and a
GitHub secret is readable by anything the workflow runs. Keep workflow changes reviewed, and
do not enable `pull_request_target` or run untrusted fork code with these secrets.

### Building by hand

For a rehearsal, or a build outside CI, see the runbook in the README ("macOS: build, sign,
release, update"): `tools/mac/macpack.py release --skip-notarize` signs and builds the image
locally without contacting Apple.
