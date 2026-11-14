# Plan — Users and roles on the self-hosted computer, and its LAN SMB share

Owner's ask (#686 §2, 2026-11-14): the drive the PC shares with the phone (`driftwire host`,
[self-hosted.md](self-hosted.md)) is also an ordinary network share at home, with a username and
password, so other computers on the network can open it. Owner's call of the same day: **there is
one admin password**, the owner's passphrase that already exists
([nas-owner-and-guests.md](nas-owner-and-guests.md) §3, `src/wire/owner_pass.*`). Anyone signed in
as admin, in MediaViewer or in the `driftwire` CLI, can create and edit **other users and their
roles**, for the LAN share and the self-hosted computer. SMB stays LAN-only and is never opened on
the router.

#686 §2's other calls were not answered one by one. They are taken as recommended here, and §11
lists them for the owner to change:

- An administrator once for each change to the OS: yes, through one elevated step (§5).
- A new user's role: **can view files** (read-only), unless the admin picks another.
- The share is optional. It is off until the first user is added.

## 1. Short version

- **Roles:** `admin`, `change` (can change files) and `view` (can view files). The default is `view`.
- **Users** (name, role, folders) are kept in the host service's own secret store, beside the owner
  passphrase (`<data>/host/serve`, item `share_users`). **MediaViewer never keeps a password.** An SMB
  password is the OS account's own. It is typed, passed once to the OS through a pipe, and wiped.
- **Admin sign-in:** the owner passphrase. The CLI asks for it with echo off. The desktop uses the
  owner session the card already unlocks.
- **The OS side:** one elevated step, `driftwire host smb-apply`. The person approves it each time
  through sudo, the macOS authorization dialog or UAC. It is never part of the always-running host,
  which still refuses elevation. It makes only **sharing-only** accounts. It never changes an
  account it did not make.
- **LAN only:** `host_ports` refuses to forward 137–139, 445 and 3389, whoever asks.
- Each share is **SMB3 with encryption on that share** (`sharing -E 1` on macOS, `EncryptData` on
  Windows). Guest access is off. SMB1 is never turned on, and on Windows the step refuses to make a
  share while the SMB1 server is on.
- **Ports (D9):** one interface, `host_smb::port`, with one file per OS: `host_smb_win.cpp`,
  `host_smb_mac.cpp`, `host_smb_none.cpp` (Linux: Samba later).

## 2. Roles

| Role | What the person can do | SMB share permission (Windows) | NTFS on the folder | macOS ACL on the folder | A phone paired for them (§9) |
|---|---|---|---|---|---|
| `view` (can view files) | open and copy out | Read | Read & execute (`RX`), inherited | `list,search,readattr,readextattr,readsecurity`, inherited | backup off; the user's folders |
| `change` (can change files) | also add, rename, delete | Change | Modify (`M`), inherited | view's + `add_file,add_subdirectory,delete,delete_child,writeattr,writeextattr` | backup on; the user's folders |
| `admin` | also change permissions on the share's files | Full control | Full control (`F`), inherited | change's + `writesecurity,chown` | no limit |

- The `admin` **role** is about files on the share. It is not MediaViewer's admin sign-in, which is
  the passphrase and nothing else. One admin password: making someone an `admin` user does not let
  them add users unless they also know the passphrase.
- **Folders:** each folder the host shares (`host share PATH --label L`) becomes one SMB share named
  after its label (`\\PC\Photos`). A user sees every share, or only the labels the admin picked. A
  share no user may open is not made.
- POSIX permission bits are never changed. The ACL entries allow access on top of them, and the
  folder stays the owner's.

## 3. Where users are kept

`share_users` in the host service's file store (`<data>/host/serve`, owner-only, the store that
holds `owner_passphrase`). It is JSON, version 1:

```json
{"v":1,"users":[{"name":"anna","role":"view","folders":["Photos"],"created":1763130000,"applied":1763130042}]}
```

- `name`: 1 to 20 characters, lowercase ASCII letters, digits, `.`, `_` and `-`, starting with a
  letter. It is the same on every OS (a Windows local account allows 20 characters, macOS allows
  more). OS names are refused: `admin`, `administrator`, `guest`, `root`, `nobody`, `daemon`,
  `everyone`, `system`, `users`, the account running MediaViewer, and anything starting with `_`.
- `folders`: labels. Absent means every folder.
- `applied`: when the OS last took this user. 0 means it is waiting for the elevated step.
- There are at most 32 users. A store that cannot be read is never overwritten (`grants`' rule).
- There is no password and no hash. Rule 6: no name, path, address or password goes in a log. User
  names appear only on the person's own screen and in `host status`.

## 4. Signing in as admin

**CLI.** `driftwire host users add|role|password|remove` asks (`add` and `role` take `--folders L1,L2|all`)
`Admin passphrase (the owner's): ` with echo off. It checks the passphrase against the verifier:
Argon2id at the stored cost, and the public key compared in constant time. Misses count on the
owner's counter, which `serve` also uses, so the lockout applies here as it does over the wire.
`host users` (the list) and `host status` need no sign-in, because they only read.

This bends call **C3** ("the CLI does not ask for it locally") for users only. The decision-log
row says why: the owner's call asks for sign-in in the CLI too. The check is honest about its
limit. Someone who can edit the host's files can edit `share_users`. But every change to the OS
still needs the elevated step, which an OS administrator approves. So the passphrase guards the
list, and the OS guards the accounts.

**Desktop.** Settings → Phone access → This computer → **Users**. The section shows "Unlock as
admin" first, the card's existing unlock (desk `unlock`, the service's `challenge`/`unlock`, ten
minutes). Once it is unlocked, the desk asks the host's own service over the owner session, which
gets three new `serve_admin` verbs (optional; an older service answers `bad_request`):

| Verb | Needs an unlock | Does |
|---|---|---|
| `users` | no (read) | `{"users":[{name, role, folders, applied}]}` |
| `user_set {name, role, folders?}` | yes | adds the user or changes them |
| `user_remove {name}` | no (it takes access away) | removes the user from the list |

Then the app runs `driftwire host users apply --input-stdin`, the elevated step's front end (§5),
writing one line `NAME<TAB>PASSWORD` for each new user or changed password. The passwords go
through a pipe and are wiped after writing. They are never arguments, never in `state_json` and
never logged.

## 5. The elevated step

`driftwire host smb-apply` is the only command that **must** run elevated. It refuses otherwise.
The person never types it: `host users …` (and `host users apply` for the desktop) builds the
whole request, shows what it will change, and starts it:

- **macOS, at a terminal:** `sudo <driftwire> host smb-apply`. sudo asks on the tty, and the request
  goes over stdin.
- **macOS, from the app:** the authorization dialog (`AuthorizationExecuteWithPrivileges`; the
  request goes over its pipe).
- **Windows:** `ShellExecuteEx` "runas" (UAC) with the name of a named pipe that only this user can
  open (`FILE_FLAG_FIRST_PIPE_INSTANCE`, DACL for this user's SID and the Administrators group, since UAC may
  ask for another account). The parent checks the client's process id against the one it started.

The request is a single JSON object `{shares:[{name, path}], users:[{name, role, shares[],
password?}], remove:[names]}`. The helper checks every field again, because it trusts nothing a
user-writable process sent it. It applies the request and answers
`{ok, users:[{name, ok, why}], shares:[{name, ok, why}]}`. The answer has no password and no
path. The front end then sets each user's `applied` stamp.

Before it starts, the front end says what the step will do, in plain words: "Make the sharing-only
account anna (can view files) and share Photos on this network, encrypted, no guests. Turns on
file sharing if it is off." Then it asks once (`y/N` at the terminal, a button in the app) before
the OS's own prompt.

**What it does on each OS.**

| | macOS (`host_smb_mac.cpp`) | Windows (`host_smb_win.cpp`) |
|---|---|---|
| Account | a local user via Open Directory (`ODNodeCreateRecord`): `UserShell /usr/bin/false`, `NFSHomeDirectory /var/empty`, `IsHidden 1`, a `Comment` marking it as MediaViewer's, a member of the group `mvshare`. The NT hash is turned on (`pwpolicy -u NAME -sethashtypes SMB-NT on`, a name only), the password set through `ODRecordChangePassword`, and the user added to `com.apple.access_smb` | `NetUserAdd` level 1 with a comment marking it as MediaViewer's, `UF_DONT_EXPIRE_PASSWD`, and membership in the local group "MediaViewer share users" (never "Users" or "Administrators"); the deny-local and deny-RDP logon rights (`LsaAddAccountRights`); hidden from the sign-in screen (`Winlogon\SpecialAccounts\UserList`) |
| Ours? | refuses any existing account without our `Comment` and group | refuses any existing account without our comment and group |
| Share | `sharing -a PATH -S NAME -s 001 -g 000 -E 1` (SMB only, no guest, encrypted), or `-e` on a share point that is already ours | `NetShareAdd` level 502 with a share DACL that lists the users alone (no Everyone), remark "MediaViewer"; `SHI1005_FLAGS_ENCRYPT_DATA` (`NetShareSetInfo` 1005) |
| Folder permissions | an inherited ACL entry for each user (`acl_*`) on the folder, and its inherited copy on what is already inside (as the Finder's "Apply to enclosed items"; links are not followed, other volumes not entered); our earlier entries for MediaViewer's users removed first | inherited ACEs for each user (`SetEntriesInAcl`, `SetNamedSecurityInfo`); earlier ACEs for our users removed first |
| SMB1, guest | macOS's server has no SMB1 setting that MediaViewer changes. Guest is off on each share | the step refuses while `LanmanServer\Parameters\SMB1` is not 0, and says how to turn it off. Guest is off: no Everyone or Guest ACE |
| Service | `launchctl enable` + `kickstart system/com.apple.smbd` if file sharing is off (said before the prompt) | the Server service is assumed running; if not, the step says so |
| Address | `smb://<LocalHostName>.local/<share>` | `\\<COMPUTERNAME>\<share>` |

**What the step checks itself** (Opus review, 2026-11-14), because the request comes from a process
anyone logged in as the person can run:

- **The folder** (`check_share_folder`, per OS):
  - It must not be a system folder (`refuse_system_folder`): not `/` or the system drive's root,
    not `/System`, `/Library`, `/private`, `/usr`, `/Applications` or the like, not `C:\Windows`,
    Program Files or ProgramData, and not `/Users`, `/Volumes` or `C:\Users` themselves.
  - It must be the folder itself, with no link, junction or reparse point anywhere in its path
    (`realpath`, or `GetFinalPathNameByHandle`).
  - It must belong to the person who started the step: `SUDO_UID` or the dialog's real uid on
    the Mac, the token's user on Windows. The one exception is the root of a drive that is not
    the system's.
- **The Mac's ACL walk** works by open descriptor (`openat`, `O_NOFOLLOW`, `acl_set_fd_np`).
  It follows no link and enters no other volume, so nothing swapped in during the walk is
  touched.
- **Removals the step was never told about:** every account in MediaViewer's own group that the
  request no longer lists is taken away. That covers a removal made in the app, or one whose
  step was declined.
- **Only MediaViewer's accounts** have their entries stripped from a folder. On Windows a
  name resolves only to a local user of this computer, never a domain or well-known account.
- **Passwords** must be valid UTF-8, and one Windows cannot convert is refused, never set empty.
- **The admin sign-in** counts a miss before it checks, and refuses when the count cannot be
  written, as the service does.
- **On Windows**, a step that ends without connecting no longer leaves the front end waiting.

Removing a user deletes the account only if it is ours. It also removes that user's entries from
each share's permissions. A share no user may open any more is removed. The folder and its files
are never touched (rule 5).

**Linux:** `host_smb_none.cpp` answers "not on this computer yet (Samba comes with the Linux
host)". `host users` still keeps the list, and `host status` says the share is not offered.

## 6. LAN only

`host_ports::forwardable(port)` is false for 137, 138, 139 (NetBIOS), 445 (SMB) and 3389 (RDP).
The keeper marks such a port `refused` with the note "never forwarded (a LAN-only service)" before
it talks to any router. So no caller, now or later, can map them. The tests prove it with a router
that counts every request it sees: it sees none. Setup and `host users` say "This share is for your
home network only. MediaViewer never opens it on your router."

## 7. `host status`

Below the existing lines:

```
LAN share   \\STUDIO\Photos   smb://studio.local/Photos   (encrypted, no guests; never on the router)
users       anna (can view files), ben (can change files), cleo (admin) — waiting: ben
```

These are names and roles only, never a password. The addresses are for the person's own screen,
as the router keeper's are. With no users: `LAN share   off (driftwire host users add NAME)`.

## 8. The desktop: Users

This is in Settings → Phone access → This computer, on both hosts. Built first as `PhoneUsersView.swift`
and `IslandHost.PhoneUsers.cs`; since the Phone access add-on (2026-11-14, `docs/plans/phone-access-addon.md`)
it is the add-on's card, `users_rows` in `src/addons/phone/screens.cpp`, one copy for both hosts, the
same rows and requests:

1. Locked: one line, "Users can open your folders from other computers at home", and **Unlock as
   admin** (the passphrase field the card already has).
2. Unlocked: one row per user, showing name, role (a picker: Can view files / Can change files /
   Admin) and Remove. "Waiting for your approval" shows on any user not applied yet.
3. **Add user**: name, role, password twice (secure fields), then **Add**. That is `user_set`, then
   the elevated step with the password.
4. The share's address with a copy button, and the line "For your home network only".

The link's requests: `users_refresh`, `user_set {name, role, folders?, password?}`,
`user_remove {name}`, `users_apply`. The card's state JSON gains `users:[{name, role, applied}]`,
`users_unlocked` and `share_addresses:[…]`, and never a password.

## 9. Phones, desktops and their role

Every paired device has a role, kept in the grants it already has (`grants.*`). No new store is
needed:

| Role | What it is in the grants | What the service lets it do |
|---|---|---|
| `view` (can view files) | a grant with `backup` off (no end, all folders, unless also timed or scoped) | read only; every write is refused (`not-allowed`) |
| `change` (can change files) | unlimited (the paired list), or a grant with `backup` on | read, and write. Today the service's only write is phone backup, into backup folders |
| `admin` | the owner's session | the owner's requests (the passphrase still gates what adds access) |

- **Caps** carry it to every client: `"role":"view"|"change"|"admin"` (`caps_msg::role`,
  `kCapsRole`). A service from before roles sends none. A client then treats a paired device as
  `change`, because it could write. The desktop client stays read-only whatever the role.
- **A desktop starts as `view`.** It pairs with `serve_pair {code, name, kind:"desktop"}`
  (`serve::kDeviceDesktop`, `client::options::device_kind`, `mobile::service_config::device_kind`).
  Whatever the code gave, its grant has `backup` off. A plain code's desktop is no guest. A phone
  by a plain code is unlimited, as before.
- **Setting it:** `serve_admin grant {key, role}`. `change` adds access, so it needs the unlock;
  `view` does not. From the desktop, the card's per-phone backup switch (`phone_backup`) is the
  same flag. From the CLI: `driftwire serve grant NAME-OR-KEY --role view|change`, alone or with
  `--for`/`--forever`.
- `share_users::phone_limit(user)` maps a LAN user's role to a grant. Pairing a device *for* a
  user, with a grant that follows that user's role changes, is **not built** (§11).

## 10. Tests (`"[host-users]"`)

These are portable and use stand-ins only. They never create a real account or share, never
change this Mac's sharing settings, and use loopback only.

- Names, roles and the store: the round trip, refusals (reserved names, case, length, an
  unreadable store never overwritten), no password ever in the JSON.
- Role mapping: SMB/NTFS/ACL rights for each role, and the phone limit.
- The plan: wanted versus applied, and only the users and shares that changed go in a request.
- The helper's core, `host_smb::apply`, against a recording stand-in port: accounts made only when
  new, an account that is not ours refused, a removed user's account deleted only if ours, a share
  with no users removed, the password wiped, the answer free of passwords and paths.
- The request's JSON: the helper refuses bad names, paths that are not absolute and unknown roles.
- `host_ports`: 445/139/137/138/3389 refused with no router request; 7878 still mapped.
- Wire: `users` reads without an unlock; `user_set` needs an unlock; `user_remove` does not; an
  older-shaped request is refused.
- CLI: `host users` parsing, the passphrase refused as an argument, `--input-stdin` lines, status
  lines with names and roles only.
- The addresses: `\\PC\Photos`, `smb://pc.local/My%20Photos`.

## 11. Owed / for the owner

- **Calls taken as recommended** (owner may change them): an administrator once for each OS
  change; new users start as `view`; the share is off until the first user.
- **Not run on a real OS:** the macOS and Windows ports have never made a real account or share.
  The tests use stand-ins. Real checks for #686's verify: a share opened from another computer with
  its username and password, and not reachable from outside. Details to check then:
  - whether `pwpolicy -sethashtypes SMB-NT on` still turns on the SMB hash on current macOS;
  - whether `launchctl enable` + `kickstart` of `com.apple.smbd` turns File Sharing on as System
    Settings does;
  - whether Windows' `NetUserAdd` puts the account in "Users" (the step takes it out).
- **The CLI's lockout and the service's** share one counter in the store, but they run in separate
  processes, so a miss counted by both at the same moment may be lost. The CLI then lets one extra
  guess through.
- **Windows halves:** the C++ compiles in CI only. The WinUI half is checked with
  `msbuild -t:Compile`, and its words with `dotnet test` (`PhoneWordsTests`), on the Mac.
- **Linking a device to a user** (§9): `host pair --for NAME`, and grants that follow that user's role.
- **Windows firewall:** File and Printer Sharing on the Private profile is the person's own call.
  The step only says when the network is marked Public.
- **Linux:** Samba, with the Linux host.
