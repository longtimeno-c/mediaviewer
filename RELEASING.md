# Releasing MediaViewer

Downloads belong on [GitHub Releases](https://github.com/longtimeno-c/mediaviewer/releases):
a Windows x64 installer wizard (`Setup.exe`) and a macOS 14+ universal (Apple Silicon and
Intel) disk image (`.dmg`, drag to Applications).

**You can publish test installers without buying signing certificates.** Use `preview`.
For a stable Mac release, configure Developer ID, Apple notarization and Sparkle signing.
Windows Authenticode is optional; the Windows update-manifest key is required for stable updates.

## Publish from GitHub Actions

[Release](.github/workflows/release.yml) is manual. Pushes and tags do not start packaging
or publishing. [CI](.github/workflows/ci.yml) handles ordinary push/PR validation.
The workflow reads the version already committed in `CMakeLists.txt`; it does not
bump the version, commit changes, or push them for you.

1. For each stable release, increase `project(mediaviewer VERSION x.y.z ...)` in
   `CMakeLists.txt`. Both platforms use that exact version, including the Mac build number.
   App versions must be strictly numeric (`0.1.2`, not `0.1.2-beta`).
2. Commit and push that version change with the desired source to `main`; let CI pass first.
3. Open **Actions → Release → Run workflow**, select `main`, and choose:

| Mode | Result | Credentials |
|---|---|---|
| `artifacts` (default) | Unsigned installers in Actions artifacts, retained 30 days | None |
| `preview` | Both unsigned installers on GitHub Releases as a **Pre-release** | None |
| `stable` | Both installers and signed update feeds, marked **Latest** | Windows manifest key + all six Mac secrets; Azure optional |

After the updated workflow is on `main`, the equivalent commands are:

```sh
gh workflow run release.yml --ref main -f mode=preview
# After configuring the stable credentials:
gh workflow run release.yml --ref main -f mode=stable
gh run list --workflow release.yml --limit 5
gh release list
```

Leave `min_version=0.0.0` and `blocklist` empty normally. These are Windows update
withdrawal controls. A blocklist is comma-separated strict versions without spaces;
it must not contain the release being published.

Required secrets are checked before the expensive builds. Windows and Mac build
independently; a final job validates both asset sets, uploads into a draft, checks the
uploaded names and sizes, and only then publishes. A failed platform leaves the previous
Latest release intact. Only the publishing job has release-write permission.

Stable tags are `v<version>`; previews use `v<version>.preview.<run-id>` (only the Git tag
has a suffix). Tags point at the exact built commit. Selecting an existing tag requires
`v<version>` to match CMake. Published releases are never overwritten. Stable versions
must increase beyond the current latest release; no run-number version stamping occurs.

## Unsigned previews

Windows SmartScreen may warn. The Mac image is ad-hoc signed and unnotarized, so
Gatekeeper may block it. A preview is for deliberate testing, not normal signed distribution.
The **Mac preview has no automatic updater**; install a stable build manually later.
The Windows preview only accepts correctly signed, higher-version updates from the stable feed.

Previews carry installers and checksums, without update feeds. They do not replace
Latest or its `/latest/download/` URLs. **Do not manually promote a preview to Latest.**

## Stable signing setup

These credentials serve separate purposes:

| Credential | Purpose | Required for stable? |
|---|---|---|
| Windows Ed25519 manifest key | Trust and hashes of Windows updates | Yes; free |
| Apple Developer ID + notarization | Mac app identity and Gatekeeper assessment | Yes for this workflow |
| Sparkle EdDSA key | Trust of Mac updates and feed | Yes; free |
| Windows Authenticode / Azure Artifact Signing (formerly Trusted Signing) | Publisher identity of Windows executables | Optional |

Code signing does not guarantee that SmartScreen never warns; reputation and policy matter too.

### Windows update key

The production public key is already pinned in
[`UpdateKeys.cs`](src.managed/MediaViewer.Updater/UpdateKeys.cs). Use its **existing matching
private key**; do not generate another for CI. On the Windows release machine:

```powershell
Get-Content -Raw "$env:USERPROFILE\.mediaviewer-release\dev.key" |
  gh secret set MV_MANIFEST_SIGNING_KEY --repo longtimeno-c/mediaviewer
```

This uploads the key without printing it. Keep an offline backup. Private keys must never
enter git or logs. An Actions repository secret is supported for this manual workflow;
do not expose it to PR code. The Windows job verifies the resulting manifest with the
pinned public key and checks package hashes before uploading. A wrong key fails the build.

There is no built-in Windows key rotation: ship a transition build signed by the old key
before changing the pinned key. Losing the private key can strand existing installations.

### macOS secrets

Add these under **Settings → Secrets and variables → Actions → Repository secrets**:

| Secret | Value |
|---|---|
| `MV_MAC_CERT_P12_BASE64` | Base64 of exported **Developer ID Application** certificate **and private key** (`.p12`) |
| `MV_MAC_CERT_PASSWORD` | Nonempty password used when exporting the `.p12` |
| `APPLE_ID` | Apple account used for notarization |
| `APPLE_TEAM_ID` | Apple Developer Team ID |
| `APPLE_APP_PASSWORD` | App-specific password for notarization |
| `MV_SPARKLE_PRIVATE_KEY` | Existing Sparkle private key, exported with `generate_keys -x <file>` |

Export from **Keychain Access → My Certificates**, including the private key. A `.cer`
alone is insufficient. The certificate must match `MV_MAC_SIGN_IDENTITY` in the workflow
(currently Oakforge Studios LTD, team `23FQ4A4Q35`). The Sparkle key must match the
workflow's `MV_SPARKLE_PUBLIC_ED_KEY`.

On the Mac:

```sh
base64 -i DeveloperID.p12 | gh secret set MV_MAC_CERT_P12_BASE64 --repo longtimeno-c/mediaviewer
gh secret set MV_MAC_CERT_PASSWORD --repo longtimeno-c/mediaviewer
gh secret set APPLE_ID --repo longtimeno-c/mediaviewer
gh secret set APPLE_TEAM_ID --repo longtimeno-c/mediaviewer
gh secret set APPLE_APP_PASSWORD --repo longtimeno-c/mediaviewer
gh secret set MV_SPARKLE_PRIVATE_KEY --repo longtimeno-c/mediaviewer < ~/.mediaviewer-release/sparkle_ed25519.key
```

Commands without input values prompt privately. Back up the identity and Sparkle key;
remove temporary unprotected exports after setup. CI imports a temporary runner keychain
and removes its signing material afterwards. Sparkle appcast generation uses a private
temporary key file (`--sparkle-key-file`), avoiding interactive keychain access on the
runner; the key is still checked against the pinned public key before building.
Follow Sparkle's key-rotation procedure
before replacing its pinned public key.

### Optional Windows publisher signing

Configure all six, or leave all unset: `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`,
`AZURE_CLIENT_SECRET`, `TRUSTED_SIGNING_ENDPOINT`, `TRUSTED_SIGNING_ACCOUNT`,
`TRUSTED_SIGNING_PROFILE`. They identify an enrolled Azure signing account, certificate
profile and identity with signing permission. Partial configuration fails preflight.

Velopack signs its payload and setup bundle; the workflow separately signs the final
Inno wizard with a timestamp. Signing errors fail the run. Without Azure, stable Windows
publishing still works with a signed update manifest; Windows may warn at installation.

## Stable release assets

| Asset | Purpose |
|---|---|
| `MediaViewer-<version>-Setup.exe` | Windows first-install wizard |
| `MediaViewer-<version>-full.nupkg` | Windows update payload |
| `RELEASES`, `releases.win.json`, `assets.win.json` | Velopack feed/index |
| `mediaviewer-manifest.json` and `mediaviewer-manifest.json.sig` | Signed Windows manifest |
| `MediaViewer-<version>.dmg` | Mac first install |
| `MediaViewer-<version>.zip` | Stapled Mac app archive for Sparkle |
| `appcast.xml` | Signed Sparkle feed |
| `SHA256SUMS.txt` | Download checksums |

Add-ons (Milestone G, [plan/18](plan/18-import.md)) publish beside the app on the same release,
one set per platform, signed with the **same** Ed25519 key as the update manifest:

| Asset | Purpose |
|---|---|
| `mediaviewer-addon-import-<platform>.json` and `.json.sig` | Signed add-on manifest (every file's SHA-256, size, licence; host API range; the archive) |
| `mediaviewer-addon-import-<platform>.zip` | The add-on's files and nothing else |

`<platform>` is `win-x64` or `macos` (one universal add-on, like the app).

> **Owner step, once:** the workflow half of this ships as `tools/package/release-addon.patch`
> (the change was made from a session without the GitHub `workflow` scope). Apply it with
> `git apply tools/package/release-addon.patch`, commit `.github/workflows/release.yml`, and delete
> the patch. Until then releases carry no add-on and Settings says Import is not published yet.

With the patch applied, a **stable** run of the
release workflow builds, signs and packs both, and `publish` refuses a stable release without them
(the apps fetch the add-on from `releases/latest`, so a release that lacks it would leave Settings
with nothing to install). The add-on's version is the release version (`-DMV_IMPORT_VERSION`), so
each release's add-on is newer than the last. Preview and artifacts runs do not pack add-ons.

- **Windows:** `mv_import.dll` and `MediaViewer.Import.Chrome.dll` are Authenticode-signed when
  Trusted Signing is configured, then `addon-pack.py pack --require-pinned-key` signs the manifest
  with `MV_MANIFEST_SIGNING_KEY` and fails if that key is not the one the app pins.
- **macOS:** each architecture's `build/addons/import` is joined with `lipo_merge.py`, then
  `tools/mac/macpack.py addon` signs `libmv_import.dylib` and `Import.bundle` with the app's
  Developer ID (same Team ID: the app loads them under library validation) and notarizes them,
  and `addon-pack.py pack --platform macos --require-pinned-key` packs it with `ditto`, so the
  signatures survive.

To pack by hand (the key file holds the private key, hex; never commit it or echo it into a log):

```
python3 -m pip install -r tools/package/requirements.txt
python3 tools/package/addon-pack.py pack --platform win-x64 --src build/addons/import/Release \
    --version 1.0.0 --key <key-file> --out dist/ --require-pinned-key
python3 tools/package/addon-pack.py verify dist/mediaviewer-addon-import-win-x64.json \
    --public-key 0451bfecfb6a26d9058fb09cfa7a9305dcf1cea7c87221e9185b0038b1cc908c
python3 tools/mac/macpack.py addon --dir build/addons/import --identity "Developer ID Application: …" \
    --notary-profile <profile>      # macOS, before packing with --platform macos
```

Settings → Add-ons reads the signed manifest when it opens and offers **Install** only when it
verifies for that app; otherwise it says Import is not published yet, or that it needs a newer
MediaViewer.

Both updaters use this repository's `/releases/latest/download/`. Keep both feeds on every
stable release: installers alone do not update existing users. Smoke-test an install on
each platform and an upgrade from the previous stable before announcing a release.

## Recovery and local builds

- No downloads on Releases: `artifacts` only saves Actions artifacts. Use `preview` or
  `stable` and inspect the final publishing job.
- Missing secrets: use `preview` now, or configure the named secrets for `stable`.
- Mac signing failed: inspect Apple's submission log; check the private key, identity,
  Team ID and app-specific password.
- Mac stopped at `generate_appcast`: app and DMG notarization may already have passed.
  In CI, use `--sparkle-key-file` to avoid a keychain permission prompt. Appcast generation
  times out after five minutes; the complete disk-image step is limited to 30 minutes.
- Windows `release.key` missing: create it with explicit PowerShell parameters,
  `Set-Content -LiteralPath $key -Value $env:MV_MANIFEST_SIGNING_KEY -Encoding ascii -NoNewline`.
  Do not use positional arguments here. Packaging checks that the file exists up front.
- `<arch>-osx-dynamic prefix not found`: the dynamic codec install is missing, not
  an Apple credential problem. The workflow installs the manifest below before CMake.
- Build/upload failed: rerun failed jobs while the successful jobs' artifacts still exist.
  An interrupted upload leaves a draft the same source commit can resume. Review/remove
  unexpected extra draft assets before retrying. Published assets are never clobbered.
- Publish returned 404 after uploading: GitHub's release-by-tag endpoint may not expose
  an unpublished draft. The publisher uses the authenticated, paginated release list
  to locate drafts, verifies their source commit and complete asset set, then publishes.
  A completed draft can be recovered without rebuilding; keep its original source commit.
- After changing source or workflow files, push the fix and start a **new** workflow
  run. Re-running an old run still builds its original commit.
- Version already published: bump CMake and release again; do not reuse stable tags.
- Signing key lost: restore its backup; a new key will not make old installs trust it.

Local Windows packaging (install Velopack CLI 1.2.0 and Inno Setup 6 first):

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\tools\package\build-release.ps1 -BuildDir build `
  -ManifestKey "$env:USERPROFILE\.mediaviewer-release\dev.key" -MinVersion 0.0.0
dotnet run --project src.managed/MediaViewer.Updater.Tests -c Release -- verify-release dist/releases
```

Optional `-SigningMetadata` signs the Velopack payload; local builds need a separate
wizard-signing step ([update-signing.md](tools/package/update-signing.md)). See the
[README Mac runbook](README.md#runbook-build-sign-release-update-macos) for local Mac builds.

### macOS dependencies (local and Actions)

No local Mac is needed for the Actions build. The Mac release is one universal app:
`macos-build` builds it natively twice (arm64 on `macos-14`, x86_64 on `macos-15-intel`, each
with its own unit tests), then `macos` runs `tools/mac/lipo_merge.py` to join the two `.app`
trees, signs and notarizes once, and produces the `.dmg`, the Sparkle `.zip` and the signed
appcast. Each build uses two pinned vcpkg manifests with separate install trees:

- Root `vcpkg.json`: static permissive libraries, installed by CMake as `arm64-osx` /
  `x64-osx`.
- [`tools/mac/dependencies/vcpkg.json`](tools/mac/dependencies/vcpkg.json): dynamic
  FFmpeg, libheif/libde265, LibRaw and Exiv2, installed as `arm64-osx-dynamic` /
  `x64-osx-dynamic` before CMake.

On a Mac with Xcode and vcpkg, run from the repository root (Apple Silicon shown; on an Intel
Mac use `x64-osx` and `x64-osx-dynamic`):

```sh
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic \
  --x-manifest-root="$PWD/tools/mac/dependencies" \
  --x-install-root="$PWD/build/vcpkg_dynamic"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DMV_VCPKG_DYNAMIC_PREFIX="$PWD/build/vcpkg_dynamic/arm64-osx-dynamic"
cmake --build build --config Release
cmake --build build --config Release --target mediaviewer_app
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Keep both manifests' baselines and the workflow's `VCPKG_COMMIT` aligned. Do not install
both manifests into the same directory: manifest mode reconciles the install tree and
can remove packages needed by the other manifest. Actions caches both directories.
Dependency caches are saved immediately after configuration succeeds, so a later
packaging or signing failure does not force another full dependency build.

For manual uploads, attach both complete platform asset sets to a draft before publishing Latest.

References: [GitHub CLI](https://cli.github.com/manual/gh_release_create),
[Apple notarization](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution),
[Sparkle signing](https://sparkle-project.org/documentation/),
[SmartScreen](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/).
