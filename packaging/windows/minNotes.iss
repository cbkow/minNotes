; Inno Setup script for minNotes (Windows installer). Mirrors ufb's installer.
;
; PREREQ: build the app first — `cmake --build build` runs windeployqt + copies
; the vendored DLLs (WinSparkle/FFmpeg/KF6) into the build dir as a POST_BUILD
; step, so the build dir is a self-contained runtime tree.
;
; BUILD THE INSTALLER:
;   ISCC.exe packaging\windows\minNotes.iss
; Override the version / source dir from the command line if needed:
;   ISCC.exe /DMyAppVersion=0.1.2 /DReleaseDir=..\..\build packaging\windows\minNotes.iss
;
; OUTPUT: packaging\windows\minNotes-<ver>-x64.exe (unsigned; Authenticate-sign
; on macOS as a separate step).

#define MyAppName "minNotes"
#define MyAppPublisher "cbkow"
#define MyAppURL "https://minnotes.app"
#define MyAppExeName "minNotes.exe"

; Version: default matches the CMake project() version. Keep in sync (or pass
; /DMyAppVersion to ISCC from the build script, which reads it from CMake).
#ifndef MyAppVersion
  #define MyAppVersion "0.1.3"
#endif

; The self-contained runtime tree (exe + windeployqt output + vendored DLLs).
; Relative to THIS .iss file (packaging\windows\) → repo-root\build.
#ifndef ReleaseDir
  #define ReleaseDir "..\..\build"
#endif
#define IconsDir "."

[Setup]
; AppId is the stable identity across versions/upgrades — NEVER change it (and it
; must be unique vs the sister apps). Generated fresh for minNotes.
AppId={{0E0CE369-FD8B-49AE-8FF9-81B4A2B2AA7A}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppCopyright=Copyright (C) 2026 cbkow

DefaultDirName={autopf}\minNotes
DefaultGroupName=minNotes
DisableProgramGroupPage=yes

OutputDir=.
OutputBaseFilename=minNotes-{#MyAppVersion}-x64
Compression=lzma2/max
SolidCompression=yes

SetupIconFile={#IconsDir}\minNotes.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; admin install → per-machine (HKLM); needed to write the file/URI associations
; under HKLM\Software\Classes for all users. HKA below resolves to HKLM here.
PrivilegesRequired=admin
MinVersion=10.0.17763
; Close a running minNotes before overwriting its binaries (the updater relaunch
; also goes through here).
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main executable.
Source: "{#ReleaseDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; All runtime DLLs flat next to the exe: Qt6*.dll + WinSparkle.dll + the FFmpeg
; av*/sw*.dll + KF6SyntaxHighlighting.dll (CMake POST_BUILD staged them all here).
Source: "{#ReleaseDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Qt plugin + QML module directories (windeployqt output). The Fusion Quick
; Controls style (selected at runtime on Windows) ships under qml\QtQuick\Controls.
Source: "{#ReleaseDir}\qml\*";                 DestDir: "{app}\qml";                 Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\platforms\*";           DestDir: "{app}\platforms";           Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\imageformats\*";        DestDir: "{app}\imageformats";        Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\iconengines\*";         DestDir: "{app}\iconengines";         Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\networkinformation\*";  DestDir: "{app}\networkinformation";  Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\generic\*";             DestDir: "{app}\generic";             Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\sqldrivers\*";          DestDir: "{app}\sqldrivers";          Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\tls\*";                 DestDir: "{app}\tls";                 Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\styles\*";              DestDir: "{app}\styles";              Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#ReleaseDir}\qmltooling\*";          DestDir: "{app}\qmltooling";          Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
; App icon (used by the file/URI association DefaultIcon entries below).
Source: "{#IconsDir}\minNotes.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\minNotes";                       Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,minNotes}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\minNotes";                 Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; ---- .mndb document association (minNotes.Document progid) -----------
; HKA → HKLM\Software\Classes (admin/per-machine install). Double-click a .mndb
; (or `minNotes.exe file.mndb`) → main.cpp resolveAndOpen() opens it via argv.
Root: HKA; Subkey: "Software\Classes\.mndb"; ValueType: string; ValueName: ""; ValueData: "minNotes.Document"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\minNotes.Document"; ValueType: string; ValueName: ""; ValueData: "minNotes Document"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\minNotes.Document\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\minNotes.ico,0"
Root: HKA; Subkey: "Software\Classes\minNotes.Document\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

; ---- minnotes:// URI scheme (deep links) ----------------------------
; minnotes:///abs/path/doc.mndb → resolveAndOpen() opens that document.
Root: HKA; Subkey: "Software\Classes\minnotes"; ValueType: string; ValueName: ""; ValueData: "URL:minNotes Protocol"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\minnotes"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\minnotes\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\minNotes.ico,0"
Root: HKA; Subkey: "Software\Classes\minnotes\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,minNotes}"; Flags: nowait postinstall skipifsilent
