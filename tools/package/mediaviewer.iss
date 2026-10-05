; Copyright (C) 2026 longtimeno-c
; SPDX-License-Identifier: GPL-3.0-or-later
;
; MediaViewer first-install wizard (PR 8, docs/design/13 Part 1 "First install").
;
; This script is the WIZARD ONLY. It does not lay the payload down itself: it
; runs the Velopack bundle silently with --installto, which writes the versioned
; layout docs/design/13 requires (stub MediaViewer.exe, Update.exe, current\, packages\)
; so that every LATER update is a directory swap done by Velopack and NEVER
; re-opens this wizard.
;
; Per-user, no UAC:  PrivilegesRequired=lowest and a {localappdata} default.
; A per-machine install would need elevation for every update, which is the
; whole reason docs/design/13 rejects it. The enterprise MSI is a separate artefact.
;
; Pages, in order, and no more (docs/design/13): Welcome, Licence, Location, Options,
; Progress, Finish. The Ready and Start-Menu-folder pages are disabled to keep
; that list exact.
;
; NOT in this wizard, deliberately:
;   * silently becoming the default app - Windows does not allow it. The wizard
;     registers the associations ([Registry]) and offers, pre-ticked, to open
;     Settings > Default apps on the Finish page. The in-app ask (docs/design/09) stays.
;   * telemetry - the first-run screen inside the app (docs/design/13 Part 3).
;   * "install for all users" - see above.
;   * pre-ticked promotional links or telemetry.
;
; Build it through tools/package/build-release.ps1, which passes every
; /D below. Compiling this file by hand will fail on the first #error.

#ifndef MvVersion
  #error MvVersion is required (pass /DMvVersion=0.1.0)
#endif
#ifndef MvPayloadSetup
  #error MvPayloadSetup is required: the Velopack *-win-Setup.exe to run
#endif
#ifndef MvRepoRoot
  #error MvRepoRoot is required: the repository root
#endif

#define MvAppName "MediaViewer"
#define MvPublisher "MediaViewer contributors"
#define MvRepoUrl "https://github.com/longtimeno-c/mediaviewer"

[Setup]
; A fixed AppId: it is the Apps & features identity across every version.
; Never regenerate it - a new AppId makes an upgrade look like a second app.
AppId={{8C5A1E6E-2B4F-4E4E-9E4E-4D2C7B0A9F31}
AppName={#MvAppName}
AppVersion={#MvVersion}
AppVerName={#MvAppName} {#MvVersion}
VersionInfoVersion={#MvVersion}
AppPublisher={#MvPublisher}
AppPublisherURL={#MvRepoUrl}
AppSupportURL={#MvRepoUrl}/issues
AppUpdatesURL={#MvRepoUrl}/releases

; Per-user, never elevated. PrivilegesRequiredOverridesAllowed is empty on
; purpose: /ALLUSERS on the command line must not turn this into a per-machine
; install behind the updater's back.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=
DefaultDirName={localappdata}\{#MvAppName}
DisableDirPage=no
DefaultGroupName={#MvAppName}
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableWelcomePage=no
AllowNoIcons=no
ChangesAssociations=yes

LicenseFile={#MvRepoRoot}\LICENSE
SetupIconFile={#MvRepoRoot}\assets\icon\mediaviewer.ico
UninstallDisplayIcon={app}\MediaViewer.exe
UninstallDisplayName={#MvAppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#MvRepoRoot}\dist
OutputBaseFilename={#MvAppName}-{#MvVersion}-Setup
; The Velopack bundle inside is already >200 MB of codec DLLs; do not also
; try to show a splash over the top of its own progress.
ShowLanguageDialog=no
CloseApplications=no
; Apps & features size. Inno only counts what it copies itself - here its own
; uninstaller - so without this the entry said ~4 MB for a ~300 MB install.
; build-release.ps1 passes the payload plus the retained full package; add-ons
; are optional downloads and are not counted.
#ifdef MvInstalledBytes
UninstallDisplaySize={#MvInstalledBytes}
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install [name/ver] - a viewer for a real camera dump, photos and video in one folder.%n%nMediaViewer installs just for you, in your own profile. It never asks for administrator rights, and updates install quietly in the background.
FinishedHeadingLabel=MediaViewer is installed

[Tasks]
; The whole Options page. Start Menu on, Desktop off (docs/design/13).
Name: "startmenu"; Description: "Create a Start Menu shortcut"; GroupDescription: "Shortcuts:"
Name: "desktopicon"; Description: "Create a Desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
; `dontcopy`: the bundle is extracted and run by [Code] BEFORE Inno lays
; anything down. Velopack's --installto CLEARS the directory it is given, so
; anything the wizard writes first - including its own unins000.exe - is
; deleted, leaving an Apps & features entry that points at a file which no
; longer exists. Measured on vpk 1.2.0, not assumed.
;
; The licence texts are not installed to {app} either: the payload already
; carries LICENSE, NOTICE and THIRD-PARTY.md into current\, which is where About reads
; them from, and a second copy at {app} would be the one that goes stale.
Source: "{#MvPayloadSetup}"; Flags: dontcopy

[Run]
; Finish page. "Launch" is the primary checkbox; GitHub and Licence are the two
; secondary links. Launch and the default-app setup are pre-ticked. Nothing opens
; a browser unless the user asks for it (docs/design/13: do not auto-open the repo).
Filename: "{app}\MediaViewer.exe"; Description: "Launch {#MvAppName}"; \
  Flags: nowait postinstall skipifsilent
; Opens Settings > Default apps on MediaViewer. Pre-ticked: Windows makes the user
; confirm the choice there, and nothing is taken silently.
Filename: "ms-settings:defaultapps?registeredAppUser=MediaViewer";   Description: "Choose MediaViewer as the default for all supported photos and videos (opens Settings)";   Flags: nowait postinstall skipifsilent shellexec
Filename: "{#MvRepoUrl}"; Description: "Visit the project on GitHub"; \
  Flags: nowait postinstall skipifsilent shellexec unchecked
Filename: "{app}\current\LICENSE"; Description: "Read the licence (GPL-3.0-or-later)"; \
  Flags: nowait postinstall skipifsilent shellexec unchecked
; Deletes the downloaded setup .exe once this wizard has closed (user request,
; 2026-09-25; the Mac twin is the setup sheet's eject-and-Trash box). Pre-ticked;
; unticking keeps the file. The entry itself runs nothing: its BeforeInstall
; records the tick, and DeinitializeSetup starts the delete, because the file is
; locked until Setup has exited. Silent installs never delete it.
Filename: "{cmd}"; Parameters: "/c exit 0"; \
  Description: "Delete the installer ({code:InstallerFileName}) when Setup closes"; \
  Flags: nowait postinstall skipifsilent runhidden; BeforeInstall: NoteDeleteInstaller

[Icons]
; Both point at the root stub, which docs/design/13 makes the stable target: it never
; changes, and it launches the newest version folder that has started. A
; shortcut into current\ or app-1.2.3\ would break on the next update. The icon
; is the stub's own, set by `vpk pack --icon` from assets/icon/mediaviewer.ico,
; so the shortcut, the taskbar and the running window are one mark.
; AppUserModelID (PR 15) is the one the process sets (main.cpp kAppUserModelId):
; without it the stub's shortcut and the process it starts are two taskbar
; entries, and the jump list's recent folders belong to neither.
Name: "{userprograms}\{#MvAppName}"; Filename: "{app}\MediaViewer.exe"; \
  IconFilename: "{app}\MediaViewer.exe"; AppUserModelID: "MediaViewer.Viewer"; Tasks: startmenu
Name: "{userdesktop}\{#MvAppName}"; Filename: "{app}\MediaViewer.exe"; \
  IconFilename: "{app}\MediaViewer.exe"; AppUserModelID: "MediaViewer.Viewer"; Tasks: desktopicon

[Registry]
; File associations (docs/design/09 "Windows integration"). REGISTER, never take: Windows
; will not let an app write UserChoice, so nothing here changes what opens a file
; today. It puts MediaViewer in "Open with", makes it a candidate in Settings >
; Default apps, and the Finish page can open that page. Every key is per-user
; (HKCU) and points at the root stub, which survives updates. All of it is
; removed on uninstall (uninsdeletekey / uninsdeletevalue): docs/design/10 "an update
; that leaves a zombie association is a failed uninstall".
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Image"; ValueType: string; ValueData: "MediaViewer Photo"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Image\DefaultIcon"; ValueType: string; ValueData: "{app}\MediaViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Image\shell\open\command"; ValueType: string; ValueData: """{app}\MediaViewer.exe"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Video"; ValueType: string; ValueData: "MediaViewer Video"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Video\DefaultIcon"; ValueType: string; ValueData: "{app}\MediaViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Video\shell\open\command"; ValueType: string; ValueData: """{app}\MediaViewer.exe"" ""%1"""
; docs/design/25: an add-on package (.mvaddon) is MediaViewer's own type, so the
; extension points at it outright (nothing else opens one), unlike the photo
; and video types, which are only offered. Opening one shows Settings' install
; sheet, never the viewer.
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Addon"; ValueType: string; ValueData: "MediaViewer Add-on"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Addon\DefaultIcon"; ValueType: string; ValueData: "{app}\MediaViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Addon\shell\open\command"; ValueType: string; ValueData: """{app}\MediaViewer.exe"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\.mvaddon"; ValueType: string; ValueData: "MediaViewer.Addon"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.mvaddon\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Addon"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mvaddon"; ValueData: "MediaViewer.Addon"
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "MediaViewer"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "View photos and video from a camera dump."
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "MediaViewer"; ValueData: "Software\MediaViewer\Capabilities"; Flags: uninsdeletevalue
; PR 15: the Explorer thumbnail handler (docs/design/12 2026-09-25). The app copies
; it to {app}\shellext\<version> and writes these keys' values on its first
; start (shell/shellext_install.cpp); the wizard only creates them, so that
; uninstall deletes them. The handler's ShellEx sits under MediaViewer.Image
; and goes with that key.
Root: HKCU; Subkey: "Software\Classes\CLSID\{{6A3F1B52-8C0E-4D7A-9B21-5E4C7D2F9A13}"; ValueType: none; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\AppID\{{3E91A7C2-5B4D-4F18-8C6E-9D2A1B7F4E05}"; ValueType: none; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\.jpg\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpg"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.jpeg\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpeg"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.png\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".png"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.bmp\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".bmp"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.gif\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".gif"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.webp\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webp"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.tif\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tif"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.tiff\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tiff"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.ico\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ico"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.heic\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heic"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.heif\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heif"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.hif\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".hif"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.avif\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avif"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.dng\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dng"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.cr2\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cr2"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.cr3\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cr3"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.nef\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".nef"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.nrw\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".nrw"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.arw\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".arw"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.srf\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".srf"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.sr2\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".sr2"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.orf\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".orf"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.raf\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".raf"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.rw2\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".rw2"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.pef\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pef"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.ptx\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ptx"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.srw\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".srw"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.rwl\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".rwl"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.3fr\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".3fr"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.fff\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".fff"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.iiq\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".iiq"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.mef\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mef"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.mos\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mos"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.raw\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Image"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".raw"; ValueData: "MediaViewer.Image"
Root: HKCU; Subkey: "Software\Classes\.mp4\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Video"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp4"; ValueData: "MediaViewer.Video"
Root: HKCU; Subkey: "Software\Classes\.mov\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Video"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mov"; ValueData: "MediaViewer.Video"
Root: HKCU; Subkey: "Software\Classes\.mkv\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Video"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mkv"; ValueData: "MediaViewer.Video"
Root: HKCU; Subkey: "Software\Classes\.webm\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Video"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webm"; ValueData: "MediaViewer.Video"
Root: HKCU; Subkey: "Software\Classes\.avi\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Video"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avi"; ValueData: "MediaViewer.Video"
Root: HKCU; Subkey: "Software\Classes\.ts\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Video"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ts"; ValueData: "MediaViewer.Video"
Root: HKCU; Subkey: "Software\Classes\.m4v\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Video"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4v"; ValueData: "MediaViewer.Video"
; Audio (docs/plans/audio-and-documents.md §3): "Open with" only. No
; Capabilities\FileAssociations line, so MediaViewer is not even a candidate
; for these in Default apps — it is never made their default (owner, 2026-10-03).
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Audio"; ValueType: string; ValueData: "MediaViewer Audio"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Audio\DefaultIcon"; ValueType: string; ValueData: "{app}\MediaViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Audio\shell\open\command"; ValueType: string; ValueData: """{app}\MediaViewer.exe"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\.mp3\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Audio"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.m4a\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Audio"; ValueData: ""; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.m4p\OpenWithProgids"; ValueType: string; ValueName: "MediaViewer.Audio"; ValueData: ""; Flags: uninsdeletevalue

[UninstallDelete]
; The whole Velopack layout. Inno removes what it installed by itself, and it
; installed NONE of this: the payload, the stub and Update.exe were written by
; the bundle, so without these lines the uninstaller leaves MediaViewer.exe and
; Update.exe behind and the directory cannot be removed. Measured - "dirGone
; False" with only the folders listed.
;
; docs/design/13 asks for the install directory "including leftover app-* folders",
; which is the shape a half-applied update leaves.
;
; Entries are enumerated rather than globbing {app}\* because the user may have
; pointed the Location page at a directory that is not exclusively ours.
Type: filesandordirs; Name: "{app}\packages"
Type: filesandordirs; Name: "{app}\current"
Type: filesandordirs; Name: "{app}\updater"
Type: filesandordirs; Name: "{app}\staging"
Type: filesandordirs; Name: "{app}\app-*"
Type: filesandordirs; Name: "{app}\telemetry"
; PR 15: the versioned thumbnail handler copies. One still held by a
; surrogate is removed when that process ends (it idles out in minutes).
Type: filesandordirs; Name: "{app}\shellext"
; The session copy of the UI font (IslandHost.SessionFontCopy). [Code]
; unregisters it first; Windows holds a registered font open.
Type: filesandordirs; Name: "{app}\fonts"
; Thumbnail cache (io::thumb_cache_dir): always under %LocalAppData%, and
; only ever a cache. Add-ons are removed by [Code] below.
Type: filesandordirs; Name: "{localappdata}\MediaViewer\thumbs"
Type: files; Name: "{app}\MediaViewer.exe"
Type: files; Name: "{app}\Update.exe"
Type: files; Name: "{app}\sq.version"
Type: files; Name: "{app}\*.log"
Type: files; Name: "{app}\settings.ini"
; PR 15: Ctrl+Alt+C's flattened copy (io::clipboard_dir), always under
; %LocalAppData%, wherever the app itself was installed.
Type: filesandordirs; Name: "{localappdata}\MediaViewer\clipboard"
; Only if nothing the user put there remains.
Type: dirifempty; Name: "{app}"

[Code]
{ Velopack's silent install registers its OWN Apps & features entry pointing at
  Update.exe --uninstall. This wizard owns uninstall, so that second entry is
  removed here and again by the host after every update
  (src/shell/update_guard.cpp, remove_velopack_uninstall_entry). Two entries for
  one app is how a half-uninstall happens. }
const
  VelopackUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\MediaViewer';

var
  DeleteInstaller: Boolean;
  KeepDir: String;

function RemoveFontResourceEx(Name: String; Flags: Cardinal; Reserved: Cardinal): Integer;
  external 'RemoveFontResourceExW@gdi32.dll stdcall';

procedure RemoveVelopackUninstallEntry;
begin
  if RegKeyExists(HKEY_CURRENT_USER, VelopackUninstallKey) then
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, VelopackUninstallKey);
end;

(* The chrome registers its UI font session-wide (AddFontResourceEx, fl=0).
   A viewer that never reached Detach - a crash, a kill, the TerminateProcess
   exit under a running add-on - leaves it registered until sign-out, and
   Windows holds the file open the whole time. Until this fix that file was in
   current\, so --installto could not clear the directory and exited 1, and
   every in-app update failed on the rename of current\. Drop every
   registration of every .ttf in Dir (each AddFontResourceEx is counted). A
   running viewer is unaffected: XAML loads its own copy by URI. *)
procedure ReleaseSessionFonts(Dir: String);
var
  Find: TFindRec;
  I: Integer;
begin
  if FindFirst(AddBackslash(Dir) + '*.ttf', Find) then
  try
    repeat
      I := 0;
      while (I < 64) and (RemoveFontResourceEx(AddBackslash(Dir) + Find.Name, 0, 0) <> 0) do
        I := I + 1;
      if I > 0 then
        Log(Format('Released %d session registration(s) of %s', [I, Find.Name]));
    until not FindNext(Find);
  finally
    FindClose(Find);
  end;
end;

(* What --installto would wipe that is not the app: the add-ons and their
   models (up to 3 GB, docs/design/17), the thumbnail cache, settings, crash reports.
   These live in %LocalAppData%\MediaViewer, which is also the default install
   directory, so a reinstall over the top deleted them - the "install local
   search" offer came back after every reinstall. updater\ is deliberately not
   kept: its trial and rollback state belong to the build being replaced. *)
function KeptNames: TArrayOfString;
begin
  SetArrayLength(Result, 8);
  Result[0] := 'addons';
  Result[1] := 'thumbs';
  Result[2] := 'metadata-snapshots';
  Result[3] := 'Crashes';
  Result[4] := 'clipboard';
  Result[5] := 'telemetry';
  Result[6] := 'settings.ini';
  Result[7] := 'import-hint.dismissed';
end;

(* Same-volume renames into a sibling directory: instant, whatever the size.
   A rename that fails means something holds the file - in practice a running
   MediaViewer - so stop before --installto rather than let it wipe the rest. *)
procedure SetUserDataAside;
var
  App, Name: String;
  Names: TArrayOfString;
  I, N: Integer;
begin
  App := ExpandConstant('{app}');
  KeepDir := '';
  if not DirExists(App) then
    Exit;
  N := 0;
  repeat
    N := N + 1;
    KeepDir := App + '.keep' + IntToStr(N);
  until not FileOrDirExists(KeepDir);
  Names := KeptNames;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    Name := Names[I];
    if not FileOrDirExists(App + '\' + Name) then
      Continue;
    if not DirExists(KeepDir) then
      if not CreateDir(KeepDir) then
        RaiseException('Could not prepare ' + KeepDir + '.');
    if not RenameFile(App + '\' + Name, KeepDir + '\' + Name) then
      RaiseException('MediaViewer seems to be running (' + Name + ' is in use). ' +
                     'Close MediaViewer and run Setup again. Nothing has been changed.');
  end;
end;

procedure PutUserDataBack;
var
  App, Name: String;
  Names: TArrayOfString;
  I: Integer;
begin
  if (KeepDir = '') or not DirExists(KeepDir) then
    Exit;
  App := ExpandConstant('{app}');
  ForceDirectories(App);
  Names := KeptNames;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    Name := Names[I];
    if not FileOrDirExists(KeepDir + '\' + Name) then
      Continue;
    if FileOrDirExists(App + '\' + Name) then
      Log('Not putting back ' + Name + ': the new install has one; it stays in ' + KeepDir)
    else if not RenameFile(KeepDir + '\' + Name, App + '\' + Name) then
      Log('Could not put back ' + Name + '; it stays in ' + KeepDir);
  end;
  RemoveDir(KeepDir);  { only if everything went back }
end;

(* docs/design/17 and docs/design/18: uninstalling the app removes the add-ons, with a
   separate choice about the search index. Each add-on's data\ (the Local
   search index, Import's history) goes only when the user says so; a silent
   uninstall keeps it. *)
procedure RemoveAddons;
var
  Addons, Dir: String;
  Find, Inner: TFindRec;
  DeleteData: Boolean;
begin
  Addons := ExpandConstant('{localappdata}\MediaViewer\addons');
  if not DirExists(Addons) then
    Exit;
  DeleteData := False;
  if not UninstallSilent then
    DeleteData := MsgBox('Also delete your Local search index and Import history?' + #13#10#13#10 +
                         'Keep them if you will reinstall MediaViewer; the add-ons themselves ' +
                         'are removed either way.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  if FindFirst(Addons + '\*', Find) then
  try
    repeat
      if (Find.Name = '.') or (Find.Name = '..') or
         (Find.Attributes and FILE_ATTRIBUTE_DIRECTORY = 0) then
        Continue;
      Dir := Addons + '\' + Find.Name;
      if (Find.Name = '.staging') or DeleteData then
      begin
        DelTree(Dir, True, True, True);
        Continue;
      end;
      if FindFirst(Dir + '\*', Inner) then
      try
        repeat
          if (Inner.Name <> '.') and (Inner.Name <> '..') and (CompareText(Inner.Name, 'data') <> 0) then
          begin
            if Inner.Attributes and FILE_ATTRIBUTE_DIRECTORY <> 0 then
              DelTree(Dir + '\' + Inner.Name, True, True, True)
            else
              DeleteFile(Dir + '\' + Inner.Name);
          end;
        until not FindNext(Inner);
      finally
        FindClose(Inner);
      end;
      RemoveDir(Dir);
    until not FindNext(Find);
  finally
    FindClose(Find);
  end;
  RemoveDir(Addons);
end;

(* Lay the Velopack tree down FIRST, in ssInstall, which runs before Inno
   copies its own files, creates the shortcuts, or writes unins000.exe.
   --installto CLEARS its target directory, so this has to be the first thing
   that touches the install directory - otherwise the wizard deletes its own
   uninstaller and leaves an Apps & features entry pointing at nothing.
   --silent shows no UI of its own, so the progress page stays in front.

   A Pascal brace comment is not used here on purpose: an Inno constant in
   braces inside one would close the comment early. *)
procedure InstallPayload;
var
  Bundle: String;
  ResultCode: Integer;
begin
  Bundle := ExpandConstant('{tmp}\{#ExtractFileName(MvPayloadSetup)}');
  ExtractTemporaryFile('{#ExtractFileName(MvPayloadSetup)}');
  (* Installing over an existing copy: free what --installto has to delete,
     and move what it must not delete out of its way. *)
  ReleaseSessionFonts(ExpandConstant('{app}\current'));
  ReleaseSessionFonts(ExpandConstant('{app}\fonts'));
  SetUserDataAside;
  try
    if not Exec(Bundle, '--silent --installto "' + ExpandConstant('{app}') + '"',
                '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      RaiseException('Could not start the MediaViewer payload installer.');
    if ResultCode <> 0 then
      RaiseException('The MediaViewer payload installer failed with code '
                     + IntToStr(ResultCode) + '. If MediaViewer is open, close it and run Setup again.');
  finally
    PutUserDataBack;
  end;
end;

(* The Finish page's "Delete the installer" box (see [Run]). *)
function InstallerFileName(Param: String): String;
begin
  Result := ExtractFileName(ExpandConstant('{srcexe}'));
end;

procedure NoteDeleteInstaller;
begin
  DeleteInstaller := True;
end;

(* Setup's own .exe stays locked until this process and its launcher have
   exited, so a hidden cmd retries for up to ~20 s and stops as soon as the
   file is gone. Only runs after the box was ticked on a finished install. *)
procedure DeinitializeSetup;
var
  Installer: String;
  ResultCode: Integer;
begin
  if not DeleteInstaller then
    Exit;
  Installer := ExpandConstant('{srcexe}');
  Exec(ExpandConstant('{cmd}'),
       '/c for /l %i in (1,1,10) do (ping -n 3 127.0.0.1 >nul & del /f /q "' + Installer +
       '" 2>nul & if not exist "' + Installer + '" exit /b 0)',
       '', SW_HIDE, ewNoWait, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  { ssInstall fires before any file is copied. }
  if CurStep = ssInstall then
    InstallPayload;
  if CurStep = ssPostInstall then
    RemoveVelopackUninstallEntry;
end;

(* Uninstall.

   Update.exe --uninstall is deliberately NOT called. It would remove the
   registry key this wizard already owns and the shortcuts Velopack was told
   not to create (--shortcuts None), so it has nothing left to do here - but it
   detaches a cleanup process that races Inno's own directory removal. Measured:
   with it, the uninstaller logs "Failed to delete directory (145)" and exits 1
   while Velopack finishes the job a second later. The tree comes out either
   way, but an uninstaller that reports failure on success is one users retry.

   So: [UninstallDelete] takes the versioned layout and anything a failed or
   partial update left, and this hook removes the duplicate registry entry if
   an update put it back since install.

   The ProgId, OpenWithProgids and RegisteredApplications keys are [Registry]
   entries with uninsdeletekey / uninsdeletevalue, so Inno removes them with
   the tree - docs/design/10: "an update that leaves a zombie association is a failed
   uninstall". *)
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    RemoveVelopackUninstallEntry;
    (* Before [UninstallDelete]: a registered font cannot be deleted. *)
    ReleaseSessionFonts(ExpandConstant('{app}\current'));
    ReleaseSessionFonts(ExpandConstant('{app}\fonts'));
    RemoveAddons;
  end;
end;
