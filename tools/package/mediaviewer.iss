; SPDX-License-Identifier: GPL-2.0-or-later
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
;   * "make MediaViewer the default photo viewer" - PR 15, in-app, after the
;     first successful still open.
;   * telemetry - the first-run screen inside the app (plan/13 Part 3).
;   * "install for all users" - see above.
;   * anything pre-ticked beyond the Start Menu shortcut.
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
; carries LICENSE and THIRD-PARTY.md into current\, which is where About reads
; them from, and a second copy at {app} would be the one that goes stale.
Source: "{#MvPayloadSetup}"; Flags: dontcopy

[Run]
; Finish page. "Launch" is the primary checkbox; GitHub and Licence are the two
; secondary links. Nothing here is pre-ticked except Launch, and nothing opens
; a browser unless the user asks for it (plan/13: do not auto-open the repo).
Filename: "{app}\MediaViewer.exe"; Description: "Launch {#MvAppName}"; \
  Flags: nowait postinstall skipifsilent
Filename: "{#MvRepoUrl}"; Description: "Visit the project on GitHub"; \
  Flags: nowait postinstall skipifsilent shellexec unchecked
Filename: "{app}\current\LICENSE"; Description: "Read the licence (GPL-2.0-or-later)"; \
  Flags: nowait postinstall skipifsilent shellexec unchecked

[Icons]
; Both point at the root stub, which plan/13 makes the stable target: it never
; changes, and it launches the newest version folder that has started. A
; shortcut into current\ or app-1.2.3\ would break on the next update. The icon
; is the stub's own, set by `vpk pack --icon` from assets/icon/mediaviewer.ico,
; so the shortcut, the taskbar and the running window are one mark.
Name: "{userprograms}\{#MvAppName}"; Filename: "{app}\MediaViewer.exe"; \
  IconFilename: "{app}\MediaViewer.exe"; Tasks: startmenu
Name: "{userdesktop}\{#MvAppName}"; Filename: "{app}\MediaViewer.exe"; \
  IconFilename: "{app}\MediaViewer.exe"; Tasks: desktopicon

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
Type: files; Name: "{app}\MediaViewer.exe"
Type: files; Name: "{app}\Update.exe"
Type: files; Name: "{app}\sq.version"
Type: files; Name: "{app}\*.log"
Type: files; Name: "{app}\settings.ini"
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

   When PR 15 adds ProgId and handler registrations, they are removed HERE,
   before the tree goes - plan/10: "an update that leaves a zombie association
   is a failed uninstall". *)
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveVelopackUninstallEntry;
end;
