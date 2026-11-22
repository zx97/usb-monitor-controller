; SPDX-License-Identifier: GPL-3.0-or-later
;
; Inno Setup script for USB-monitor-controller (Windows 10/11, x64).
;
;   iscc /DAppVersion=0.1.0 packaging\windows\usb-monitor-controller.iss
;
; Expects dist-win\ (produced by tools/build-windows.sh) next to the repository
; root. Per-user installation: no administrator rights, no service, everything
; under %LOCALAPPDATA%\Programs.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

#define AppName "USB Monitor Controller"
#define AppExe "lcd-monitor.exe"
#define SourceDir "..\..\dist-win"

[Setup]
AppId={{8F3C2A64-5B1E-4C7A-9D2E-6A0B1F4C8E31}}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=USB-monitor-controller
; Explicit per-user path: {autopf} is redirected to SysWOW64 when the (32-bit)
; setup runs under the SYSTEM profile, which would split the uninstaller from
; the application. {localappdata} is stable for every user.
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
SetupIconFile=app.ico
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist-win
OutputBaseFilename=usb-monitor-controller-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Per-user by default; the user can still choose "install for all users".
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
CloseApplications=yes
RestartApplications=no
Uninstallable=yes
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "Start USB Monitor Controller at login (tray icon)"; GroupDescription: "Startup:"
Name: "autostart\now"; Description: "... and start it right now"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
; The setup itself is written into dist-win/, exclude it from its own payload.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*-setup.exe"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "--tray"; WorkingDir: "{app}"
Name: "{group}\Control panel"; Filename: "{app}\{#AppExe}"; Parameters: "--window"; WorkingDir: "{app}"
Name: "{group}\Theme editor"; Filename: "{app}\lcd-theme-editor.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "--tray"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userstartup}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "--tray"; WorkingDir: "{app}"; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Parameters: "--tray"; Description: "Start {#AppName} now"; Flags: nowait postinstall skipifsilent; Tasks: autostart\now

[UninstallRun]
; Stop a running instance before removing the files.
Filename: "{sys}\taskkill.exe"; Parameters: "/IM {#AppExe} /F"; Flags: runhidden; RunOnceId: "StopMonitor"

[UninstallDelete]
; Remove the configuration written by the tray (kept outside {app}).
Type: filesandordirs; Name: "{userappdata}\usb-monitor-controller"
