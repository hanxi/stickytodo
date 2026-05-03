; setup.iss — Inno Setup script for the StickyTodo Windows client.
;
; Produces a single self-contained .exe installer that:
;   1. Lays down stickytodo.exe + optional README / LICENSE into
;      %LocalAppData%\Programs\StickyTodo (default; per-user install so no
;      UAC prompt) or into %ProgramFiles%\StickyTodo when the user explicitly
;      elevates.
;   2. Creates Start Menu shortcut and (optionally) a Desktop shortcut.
;   3. Registers a standard Uninstall entry in Add/Remove Programs.
;
; Build-time inputs (passed via iscc `/D` flags — see scripts/package-win-client.sh):
;   AppVersion        = semver, e.g. "1.2.3" or "dev"
;   ArtifactDir       = absolute path to the CMake Release build output that
;                       contains stickytodo.exe (expected layout:
;                       <ArtifactDir>\stickytodo.exe)
;   RepoRoot          = absolute path to the repo root (used to locate
;                       README.md / LICENSE / assets/branding/*.ico)
;   OutputDir         = absolute path where the .exe installer is written
;   OutputBaseName    = desired output filename without `.exe` extension,
;                       typically stickytodo-setup-<AppVersion>
;
; Fallback defaults are provided so running iscc on this file standalone
; (without wrapper script) still produces *something* buildable for local
; smoke-test iteration — CI always passes all five `/D` flags explicitly.

#ifndef AppVersion
  #define AppVersion "dev"
#endif
#ifndef ArtifactDir
  ; Relative to the .iss file when iscc is launched from the repo root —
  ; matches the conventional out-of-source CMake layout used by
  ; CMakePresets.json (`build/release/`).
  #define ArtifactDir "..\client\win\build\release"
#endif
#ifndef RepoRoot
  #define RepoRoot ".."
#endif
#ifndef OutputDir
  #define OutputDir "..\dist\win-client"
#endif
#ifndef OutputBaseName
  #define OutputBaseName "stickytodo-setup-" + AppVersion
#endif

[Setup]
; AppId is what Add/Remove Programs and the uninstaller use as the stable
; identity — must NEVER change across versions, otherwise upgrades won't
; replace the old entry. Mirrors the macOS Bundle ID pattern (com.hanxi.stickytodo)
; wrapped in Inno Setup's GUID form. Generated once and frozen.
AppId={{4B5B6C2E-9E7B-4F3D-A8C5-0D6A1B2C3D4E}}
AppName=StickyTodo
AppVersion={#AppVersion}
AppPublisher=hanxi
AppPublisherURL=https://github.com/hanxi/stickytodo
AppSupportURL=https://github.com/hanxi/stickytodo/issues
AppUpdatesURL=https://github.com/hanxi/stickytodo/releases
DefaultDirName={autopf}\StickyTodo
DefaultGroupName=StickyTodo
; `auto*` constants resolve based on PrivilegesRequiredOverridesAllowed below:
; when user picks "current user only" → {userpf}\StickyTodo under %LocalAppData%\Programs
; when user picks "all users" → {commonpf}\StickyTodo under %ProgramFiles%.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline

; Cosmetics / runtime
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBaseName}
; UninstallDisplayName is what shows in "Apps & features". Keep it clean —
; never embed the version here (same discipline as the macOS .app bundle
; name guard documented in AGENTS.md §5.3).
UninstallDisplayName=StickyTodo
UninstallDisplayIcon={app}\stickytodo.exe
; Close the app before uninstall if it's running — otherwise removing the
; exe fails silently and leaves an orphaned tray icon until next reboot.
CloseApplications=yes
RestartApplications=no
; Require Windows 10 20H1 (build 19041) or later. This matches the
; Direct2D 1.1 + Segoe UI Variable + modern DWrite features we rely on.
; Windows 8/8.1 and early Windows 10 builds would still "install" but the
; binary would crash on first render, so block them up front.
MinVersion=10.0.19041

; No license dialog shipped with installer by default, but if LICENSE exists
; at the repo root, include it so corporate IT review paths are trivial.
; LicenseFile is picked up conditionally via [Files] below.

[Languages]
; Offer English + Simplified Chinese because the app UI already ships
; strings in both (matches macOS's Localizable strings and Web's i18n keys).
Name: "english";    MessagesFile: "compiler:Default.isl"
Name: "simplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
; Optional Desktop shortcut — off by default on per-user installs to keep
; the desktop uncluttered, matches modern-Windows installer UX (VS Code /
; Zed / Signal all default-off).
Name: "desktopicon";   Description: "{cm:CreateDesktopIcon}";   GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Core executable. `ignoreversion` lets Inno replace the file even if its
; PE version header hasn't advanced — useful during `-dev` iteration.
Source: "{#ArtifactDir}\stickytodo.exe"; DestDir: "{app}"; Flags: ignoreversion

; Optional documentation — shipped only if present in the build context.
; `skipifsourcedoesntexist` keeps this script runnable in minimal builds
; (where README.md / LICENSE might not be copied next to the exe).
Source: "{#RepoRoot}\README.md"; DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#RepoRoot}\LICENSE";   DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\StickyTodo";            Filename: "{app}\stickytodo.exe"
Name: "{group}\Uninstall StickyTodo";  Filename: "{uninstallexe}"
Name: "{autodesktop}\StickyTodo";      Filename: "{app}\stickytodo.exe"; Tasks: desktopicon

[Run]
; "Launch StickyTodo" checkbox on the final wizard page. `nowait` so the
; installer exits immediately; `postinstall` hides it behind an opt-in
; checkbox instead of auto-launching; `skipifsilent` skips on `/silent`
; installs (which CI / MDM deployments always use).
Filename: "{app}\stickytodo.exe"; Description: "{cm:LaunchProgram,StickyTodo}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Clean up anything the app writes next to its install dir (currently
; nothing — user data lives in HKCU\Software\stickytodo for preferences
; and in %AppData% for any future local cache, neither of which we
; delete, to preserve user settings across reinstalls — same contract
; as macOS's Preferences survival).
Type: filesandordirs; Name: "{app}\*.tmp"
