; Copyright (C) 2026 longtimeno-c
; SPDX-License-Identifier: GPL-3.0-or-later
;
; MediaViewer first-install wizard (PR 8, plan/13 Part 1 "First install").
;
; This script is the WIZARD ONLY. It does not lay the payload down itself: it
; runs the Velopack bundle silently with --installto, which writes the versioned
; layout plan/13 requires (stub MediaViewer.exe, Update.exe, current\, packages\)
; so that every LATER update is a directory swap done by Velopack and NEVER
; re-opens this wizard.
;
; Per-user, no UAC:  PrivilegesRequired=lowest and a {localappdata} default.
; A per-machine install would need elevation for every update, which is the
; whole reason plan/13 rejects it. The enterprise MSI is a separate artefact.
;
; Pages, in order, and no more (plan/13): Welcome, Licence, Location, Options,
; Progress, Finish. The Ready and Start-Menu-folder pages are disabled to keep
; that list exact.
;
; NOT in this wizard, deliberately:
;   * silently becoming the default app - Windows does not allow it. The wizard
;     registers the associations ([Registry]) and offers, pre-ticked, to open
;     Settings > Default apps on the Finish page. The in-app ask (plan/09) stays.
;   * telemetry - the first-run screen inside the app (plan/13 Part 3).
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

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
WelcomeLabel2=This will install [name/ver] - a viewer for a real camera dump, photos and video in one folder.%n%nMediaViewer installs just for you, in your own profile. It never asks for administrator rights, and updates install quietly in the background.
FinishedHeadingLabel=MediaViewer is installed

[Tasks]
; The whole Options page. Start Menu on, Desktop off (plan/13).
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
; a browser unless the user asks for it (plan/13: do not auto-open the repo).
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
; Both point at the root stub, which plan/13 makes the stable target: it never
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
; File associations (plan/09 "Windows integration"). REGISTER, never take: Windows
; will not let an app write UserChoice, so nothing here changes what opens a file
; today. It puts MediaViewer in "Open with", makes it a candidate in Settings >
; Default apps, and the Finish page can open that page. Every key is per-user
; (HKCU) and points at the root stub, which survives updates. All of it is
; removed on uninstall (uninsdeletekey / uninsdeletevalue): plan/10 "an update
; that leaves a zombie association is a failed uninstall".
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Image"; ValueType: string; ValueData: "MediaViewer Photo"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Image\DefaultIcon"; ValueType: string; ValueData: "{app}\MediaViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Image\shell\open\command"; ValueType: string; ValueData: """{app}\MediaViewer.exe"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Video"; ValueType: string; ValueData: "MediaViewer Video"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Video\DefaultIcon"; ValueType: string; ValueData: "{app}\MediaViewer.exe,0"
Root: HKCU; Subkey: "Software\Classes\MediaViewer.Video\shell\open\command"; ValueType: string; ValueData: """{app}\MediaViewer.exe"" ""%1"""
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "MediaViewer"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\MediaViewer\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "View photos and video from a camera dump."
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "MediaViewer"; ValueData: "Software\MediaViewer\Capabilities"; Flags: uninsdeletevalue
; PR 15: the Explorer thumbnail handler (plan/12 2026-09-25). The app copies
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

[UninstallDelete]
; The whole Velopack layout. Inno removes what it installed by itself, and it
; installed NONE of this: the payload, the stub and Update.exe were written by
; the bundle, so without these lines the uninstaller leaves MediaViewer.exe and
; Update.exe behind and the directory cannot be removed. Measured - "dirGone
; False" with only the folders listed.
;
; plan/13 asks for the install directory "including leftover app-* folders",
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

procedure RemoveVelopackUninstallEntry;
begin
  if RegKeyExists(HKEY_CURRENT_USER, VelopackUninstallKey) then
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, VelopackUninstallKey);
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
  if not Exec(Bundle, '--silent --installto "' + ExpandConstant('{app}') + '"',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    RaiseException('Could not start the MediaViewer payload installer.');
  if ResultCode <> 0 then
    RaiseException('The MediaViewer payload installer failed with code '
                   + IntToStr(ResultCode) + '.');
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
   the tree - plan/10: "an update that leaves a zombie association is a failed
   uninstall". *)
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveVelopackUninstallEntry;
end;
