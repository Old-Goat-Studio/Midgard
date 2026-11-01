; SPDX-License-Identifier: Apache-2.0
; Inno Setup script for the Midgard editor. Build it with `python tools/package.py --installer`,
; which first makes the PyInstaller folder in build\dist\Midgard and the icon build\midgard.ico,
; then runs:
;   ISCC /DAppVersion=0.1.0 /DAppBuild=3 midgard.iss
; The installer lands in build\installer\.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef AppBuild
  #define AppBuild "0"
#endif
#define AppName "Midgard"
#define AppExe "Midgard.exe"

[Setup]
AppId={{4A7F205E-6F63-47A3-A197-49E6B1B42458}
AppName={#AppName}
AppVersion={#AppVersion}.{#AppBuild}
AppVerName={#AppName} {#AppVersion} (build {#AppBuild})
AppPublisher=Old Goat Studio
VersionInfoVersion={#AppVersion}.{#AppBuild}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=build\installer
OutputBaseFilename=Midgard-Setup-{#AppVersion}.{#AppBuild}
SetupIconFile=build\midgard.ico
UninstallDisplayIcon={app}\{#AppExe}
LicenseFile=LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; One folder, so the Qt libraries stay separate files that users can replace (LGPL).
Source: "build\dist\Midgard\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; A new build replaces the old one completely (no stale libraries from an earlier build).
Type: filesandordirs; Name: "{app}\_internal"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

; Projects, settings and layouts are the user's own and are left in place on uninstall.
