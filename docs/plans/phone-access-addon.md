# Plan — The Phone access add-on

Issue #686 §1, owner's calls of 2026-11-14: a new **Phone access add-on** carries the Synology
path, self-hosting and their setup screens, so the base viewer no longer does. It is **offered the
first time someone opens Phone access** (not installed by default), and **phones already paired
keep working**. A Synology stays a supported choice equal to this computer and to anything else
that runs `driftwire serve`.

This reverses the 2026-11-12 call "Phone access is not an add-on" ([phone-in-the-app.md](phone-in-the-app.md)
§2). The row is in [12-decision-log.md](../design/12-decision-log.md). What that plan built stays:
the protocol in `src/wire`, the catalogue's generic half in `src/catalogue`, Local search as the
catalogue's provider, the phone's file mode. Only where the feature runs changes.

## 1. What moves, what stays

| Piece | Today | After |
|---|---|---|
| The desk (claim, codes, devices, revoke, roots, nearby server and advert) | `src/phone/desk.*`, in `mediaviewer_core` | `src/addons/phone/desk.*` (`git mv`) |
| The state machine (folder, library id, epoch, takeover, revoke queue) | `src/phone/link.*` | `src/addons/phone/link.*` (`git mv`) |
| The one link per process, its primary / secondary rule | `src/phone/app.*`, driven by `mv_phone_resume` / `_stop` from both shells | `src/addons/phone/service.*` (`git mv` of `app.*`), driven by the add-on's load and shutdown |
| This computer as the server: finding and running the installed `driftwire`, `host share`, `host start-at-login on`, DuckDNS, reading `host status`'s files | `src/phone/self_host.*` | `src/addons/phone/self_host.*` (`git mv`) |
| Settings → Add-ons → Phone access, the card | `PhoneAccessView.swift`, `IslandHost.Phone.cs`, `Shared/PhoneWords.cs`, `Shared/QrCode.cs`, `Interop/Phone.cs` (two copies of every word) | The add-on's view tree, `src/addons/phone/screens.*` (one copy, drawn by both hosts); the QR is a `surface` the add-on draws (`qr.*`) |
| The Synology package and its signed updates | `tools/package/driftwire_spk.py`, the `driftwire` CLI's NAS update | Unchanged files: the SPK carries the `driftwire` program, which is the CLI's (its release, its updater). The add-on's Synology choice says how to get and install it. Owed: shipping the SPK inside the add-on's package (§7) |
| `driftwire host` and its start at login | The CLI (`src/addons/driftwire/cli/`) | Unchanged: the CLI's. The add-on runs the installed program, as the card did |

**Stays in the base app:**

- The viewer, and nothing of Phone access but its Settings section's heading and the install
  offer (Mac `PhoneAccess.swift`, Windows `IslandHost.PhoneAccess.cs`, as Driftwire's offer).
- `src/wire` (the protocol: Driftwire, the CLI, iOS and this add-on link it) and `src/catalogue`
  (Local search and this add-on link it). Neither is linked by `mediaviewer_core` any more.
- Local search keeps building, signing and delivering the catalogue by itself (`phone_publish`,
  `phone_auto` after a quiet scan, its own retries). It does not need the add-on: a phone keeps
  receiving new catalogues on the NAS with the add-on not installed.

## 2. The contribution points and host-table rows

The card is a **settings** screen (place `settings`, [25 §10](../design/25-open-addons.md#10-screens)),
id `settings`, drawn by the host on the add-on's page, **Settings → Add-ons → Phone access**, like every
other add-on's settings (until 2026-11-15 it was a section of Settings of its own). One engine, as
Driftwire's settings page.

New in the vocabulary, **contribution API 15**: `text_field` `"secure": true`. The host draws a
secure field (`SecureField`, `PasswordBox`); the core never keeps its value in the tree (an event's
value goes to the add-on's worker and nowhere else; a posted tree may only give it `""`), so the
passphrases, the relay key and the DuckDNS token are never in a snapshot, a log or a crash report
(rule 6; secrets never arguments, never kept).

New host-table rows, appended after `thumbnail_make` (struct_size checked, `MV_ADDON_HOST_API`
stays 3): Local search as the catalogue's provider, reached through the host because the AI pack is
not a slot add-on and `addon_request` cannot reach it.

| Row | What it does |
|---|---|
| `catalogue_publish(host, destination_utf8)` | Local search's `phone_publish`. `MV_ERR_NOT_FOUND` while the pack is not loaded |
| `catalogue_cancel(host)` | `phone_cancel` |
| `catalogue_json(host, out, cap, needed)` | `phone_json` (its rows: running, phase, fraction, published, assets, library_id, epoch, gen_dir, roots) |

Both shells set the provider when the pack attaches as the writer and clear it before the pack
goes (`src/addon/catalogue_port.*`, where `phone::set_app_source` was): clearing waits out a call
in flight, so the pack can unload after it.

**C ABI 0.43:** `mv_phone_request`, `mv_phone_state_json`, `mv_phone_resume`, `mv_phone_stop` and
`MV_PHONE_SECONDARY` go; the Mac bridge's `mv_chrome_phone_*` go. Both chromes reach the add-on
through the slots ABI they already use.

## 3. An already-paired phone keeps working

What a paired phone depends on, and why none of it changes:

1. **The NAS or this computer's host** serves the phone. It is `driftwire serve` / `driftwire host`,
   a program of its own, never the app: the phone reads its library there with the app closed, with
   the add-on not installed, and through the move.
2. **The desk's keys and lists** (`phone_desk_key`, `phone_service`, `phone_devices`,
   `phone_revoke_pending`, `phone_grants`) stay in the OS's store under the same service, "MediaViewer
   Phone access". The add-on runs inside the app's process, so the Keychain / Credential Manager
   item is read as before: nothing is copied, renamed or re-encrypted. The NAS keeps seeing the same
   desk key, so the claim holds and the `known` list on the NAS (the phones it lets in) is unchanged.
3. **The publisher key** (`catalogue-signing-key`, same store) is the one Local search signs with
   and phones trust. Untouched.
4. **The library's id and epoch** are in `folder.json` in the feature's folder
   (`~/Library/Application Support/MediaViewer/Phone Access`, `%LocalAppData%\MediaViewer\phone-access`).
   The add-on keeps that folder **where it is** (`service.cpp` finds it the way `io::phone_dir` did),
   not in the add-on's data folder: removing the add-on, even without keeping its data, never makes
   a new library id. The share's `library.json` names the same id; the nearby advert's `lib=` tag is
   derived from it, so a phone on the home network finds this desktop again.
5. **What stops while the add-on is not installed:** the desktop's nearby copy (the phone falls back
   to the NAS), revokes made earlier and not yet told (told when it is installed; the queue is in
   `link.json`), and adding phones. The section says so when it finds a library set up before
   (folder.json is there) and offers the add-on.

**Remove:** Remove keeps everything above. Remove without keeping data deletes only what the host
keeps in the add-on's folder (nothing of the above); the desk's keys go only through the card's own
Start over, which tells the NAS. `mv_addon_forget` therefore forgets nothing (it says why).

## 4. Running

- **Load** (the host's add-on queue, after first pixel; rule 1): the add-on takes an instance lock
  in the feature's folder. The process that holds it is the primary (it runs the link); a later
  window's process is secondary and its card says "Set up in the first MediaViewer window", as
  before. With a library set up (folder.json there), the primary starts the link at once
  (`link::resume`): the nearby copy and owed revokes carry on with Settings never opened.
- **Settings open:** `screen_open` for `settings`. A ticker on the add-on's own thread reads the
  link's state every 0.5 s (memory only), builds the tree and posts it **only when it changed**;
  while a code shows it ticks once a second for the countdown. Nothing ticks while Settings is closed.
- **Events** come on the add-on's slot worker and become the link's requests, as the cards' were.
  Typed secrets live in the screen's state until sent, and are cleared on send, on close and when
  Settings hides (as `forgetSecrets`).
- **Quit / unload:** `shutdown` stops the link (never waits on the NAS: `detach_on_stop`), then clears
  the catalogue source. Before the AI pack unloads, the host clears its provider (waits out a call).

## 5. Both hosts (D9)

| | Mac | Windows |
|---|---|---|
| Section, offer, install | `PhoneAccess.swift` (`PhoneAccessStore`, `PhoneAccessOffer`), channel `mediaviewer-addon-phone-access-macos` | `IslandHost.PhoneAccess.cs`, channel `mediaviewer-addon-phone-access-win-x64` |
| The card | `ScreenView` of (`phone-access`, `settings`) on its page in Settings → Add-ons | `IslandHost.Screens` builder on its page |
| `secure` | `SecureField` | `PasswordBox` |
| Removed | `PhoneAccessView.swift`, `PhoneAccessTests.swift`, `mv_chrome_phone_*` | `IslandHost.Phone.cs`, `Shared/PhoneWords.cs`, `Shared/QrCode.cs`, `Interop/Phone.cs`, their tests |

Linux stays hostable: the add-on is portable C++ (`service.cpp`'s folder has a Linux branch; the
module builds in `cmake/portable`).

## 6. Verify

- `[phone-pairing]`, `[phone-link]`, `[phone-self-host]`, `[phone-relay]` pass unchanged from the new
  place (loopback only).
- New `[phone-addon]`: the module loads through a signed test store, posts a valid settings tree,
  a secure field's value never appears in the tree or the snapshot, a pairing code posts a QR
  surface that decodes (`[phone-qr]` checks the encoder against the old C# matrices), and the
  catalogue rows answer `MV_ERR_NOT_FOUND` with no AI pack.
- The feature's folder and the store's items from before the move are read in place: a library set
  up by the old app opens in the add-on with the same library id.
- The app builds and launches with no Phone access add-on; both present-loop gates hold.

## 7. Owed (not in this slice)

- **The SPK inside the add-on.** Today the person downloads `mediaviewer-nas.spk` from the release.
  Shipping it in the add-on's package (saved from the card's Synology choice) needs the release job
  to build the SPK before packing the add-on, and the SPK's own update channel stays the CLI's.
- **Hooks from the parallel sessions** (users and roles, drives, tray; #686 §§2–4). Their screens
  belong in this add-on's card. They are built on the `driftwire host` CLI and the tray; until they
  merge, the card shows what `host status` writes (the keepers' lines). Owed per session:
  `smb-users`: the LAN share's address and user lines in `state.json` (the card reads them as the
  keepers'); `drives-raid`: a mirror's health line in `state.json`; `host-tray`: nothing from this
  add-on (the tray is the CLI's and the shell's).
- **Installing for a desktop set up before the move.** The owner's call is "offered"; a desktop with a
  library set up before sees the offer with the line from §3.5. Installing it without asking was
  not chosen.

## 8. The `driftwire` program inside the add-on (owner, 2026-11-14: "Yes, ship it inside")

Amends the decision-log row "The app runs an installed `driftwire` for 'This computer'" (the new row is
above it in [12](../design/12-decision-log.md)). Built:

- **Payload.** `host/` in the add-on: on the Mac the command line the release already signed and notarized
  (macpack `cli`), on Windows the signed `driftwire.exe` and the DLLs `driftwire-windows.ps1` stages beside
  it. A local build copies `mv_driftwire_cli` there (cmake/phone.cmake). NOTICE names what the program links:
  libsodium (ISC), BLAKE3 (CC0-1.0), BoringSSL (Apache-2.0, GPLv3-compatible), FTXUI (MIT), and Microsoft's
  Visual C++ runtime on Windows ([11](../design/11-licensing.md)). `build-release.ps1` still refuses the
  program in the app's payload: the add-on is packed apart.
- **Where it lives.** `bundled::install` at the add-on's load (the first window's process, off the
  foreground queue) copies `host/` into `<feature dir>/bin` when its version or size changed, and
  `self_host::find()` takes that copy first.
- **Updates (the owner's question).** A MediaViewer update never touches `bin`: the running host goes on.
  An add-on update copies the new files beside (`*.new`) and renames them in, the program last, never
  writing over a running file. On Windows the running .exe and its DLLs are renamed aside (`*.old-N`, removed
  by a later install). The host's supervisor sees its program change and ends 75 (`cli/host_cmd.h`); the
  login entry starts the new one (Windows: the task's retry, at most 5 minutes). Phones see one reconnect
  (their reads are idempotent), and capability negotiation keeps an older or newer phone app working. Tested
  on loopback: `[phone-addon]` "an add-on update swaps the program under a running host, which ends 75".
- **Which program wins.** The card always runs the add-on's copy. A separately installed command line
  (Homebrew, install.ps1) keeps its own path and its own update channel, so the two never write one binary;
  the login entry points at whichever program last ran `host start-at-login on` (the card's Set up and Start
  move it to the add-on's copy; a terminal's `driftwire host start-at-login on` moves it back).
- **Removal.** New optional export `mv_addon_removed(data_dir, keep_data)` (mediaviewer_addon.h), called on
  every Remove except a reinstall (`mv_addon_remove` keep_data 2). When the login entry runs the add-on's
  copy, it runs `host start-at-login off` (Task Scheduler /End + /Delete, launchctl bootout), which stops the
  host; then `bin` goes. Shared folders, the host's data and pairings stay; a host from a separately
  installed command line is left alone.
- **The Synology** keeps its own signed package feed (`nas-setup.md`), unchanged.

**Merged with the integration branch (2026-11-14):** the Users section (`self-host-users-smb.md` §8) and
the Drives step (`self-host-drives.md` §6), built first as native views on both hosts, are the add-on's
card now (`users_rows`, `drives_rows` in `screens.cpp`), with their rules (`valid_user_name`,
`good_drive_label`, `drive_names_match`) and their tests ported (`[phone-addon]`). The native views, their
models and their C#/Swift tests are gone. A password and the admin unlock are secure fields.

**The server's menu stays the base app's for now.** The tray (`host_tray.*`, `src/abi/host_tray_glue.*`,
`mv_host_tray_start` / `_stop`) is the registry's shell keep, started by the first window's process. Its
model and watcher live in `src/addons/phone/host_tray.*` and the core links `mv_phone` for them alone (the
linker pulls only those objects). It runs whatever `self_host::find()` names, which looks in the add-on's
`bin/` first, so the menu's Pause / Resume / Quit act on the add-on's program. Removing the add-on stops the
host (the login entry goes), so the menu goes with it. Moving it into the add-on needs two things an
add-on keep cannot say yet: the app's login item while the host only starts at login (a "login" flag on
`keep_alive`) and opening Settings → Add-ons → Phone access from the menu (a settings-place screen as the keep's).

Still owed:

- **The server's menu** (#686 §4, session `host-tray`): its `src/phone/host_tray.*` and the shell keep move
  into this add-on once both branches merge (keep_alive + keep_menu with `host_tray::build()`'s rows, and a
  "login" flag on keep_alive for the app's login item). On the merge with its branch, keep its own start and
  stop lines when this branch's phone lines go: Windows `PhoneNative.StartHostTray()` (after the removed
  `PhoneNative.Resume(primary: true)` in IslandHost.Addons.cs) and `mv_host_tray_stop()` in main.cpp (before the
  removed `mv_phone_stop()`); Mac `mv::abi::start_host_tray()` / `stop_host_tray()` in main_mac.mm beside the
  removed `resume_app(true)` / `stop_app()`. Its `src/phone/host_tray.*` and `self_host.cpp`'s include of it move
  to `src/addons/phone` together. Its verbs run `self_host::find()`'s program, which is now the add-on's copy.
- The Linux login entry's escaping (systemd) is not matched by `login_entry_runs`; Linux ships no add-on yet.
