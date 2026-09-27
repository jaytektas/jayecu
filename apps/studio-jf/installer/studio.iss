; jayecu Studio — Windows installer (Inno Setup 6, free: https://jrsoftware.org/isinfo.php)
;
;   iscc /DAppVersion=0.1.0 installer\studio.iss
;   -> installer\Output\jayecu-studio-0.1.0-setup.exe
;
; The file name matters: the studio's update check picks the release file ending "-setup.exe"
; (src/app/UpdateCheck.h), and the version must be the one in CMakeLists.txt's project(... VERSION).
;
; SMOOTH UPGRADES come from three things here:
;   AppId                — NEVER change it. It is how a new installer knows it is upgrading this studio
;                          rather than installing a second one beside it.
;   PrivilegesRequired   — lowest: installs for the current user (%LOCALAPPDATA%\Programs), so an update
;                          asks for no administrator password.
;   [Run] without skipifsilent — the studio updates itself by running this installer with /SILENT, and
;                          this entry starts the new studio when it finishes.
;
; THE LOG. The studio writes genesis.log in its working folder, and has no console window to show it in,
; so every way of starting it (shortcuts, the post-install launch) starts it in {app}: the log is then
; always beside studio.exe.
;
; THE USB DRIVER for the ECU's bootloader (WinUSB on 0483:df11) is what lets the studio write firmware.
; It needs an administrator, so it is the one step that asks: wdi-simple (third_party/libwdi-win) run
; through "runas". Never during a silent install — an update must not put a prompt in front of anyone,
; and the studio asks for it itself the first time it writes firmware if it is missing.

#ifndef AppVersion
  #error Pass the version: iscc /DAppVersion=x.y.z installer\studio.iss
#endif

#define AppName  "jayecu Studio"
#define BuildDir "..\build-win"
; The same arguments src/comms/DfuUsbWindows.cpp passes when the studio installs the driver itself.
#define DriverArgs '--vid 0x0483 --pid 0xDF11 --type 0 --name ""jayecu ECU bootloader"" --inf jayecu_dfu.inf --manufacturer ""jayecu"" --dest ""{localappdata}\jayecu\usb-driver"" --progressbar --timeout 60000'

[Setup]
AppId={{65A19615-4563-4B98-A28A-5DE42E66BDD9}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Jason Roughley
AppPublisherURL=https://github.com/jaytektas/jayecu
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
SetupIconFile=..\assets\studio.ico
UninstallDisplayIcon={app}\studio.exe
OutputBaseFilename=jayecu-studio-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\..\..\LICENSE

[Tasks]
Name: "usbdriver"; Description: "Install the USB driver for updating ECU firmware (WinUSB, needs administrator)"; GroupDescription: "USB driver:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Everything the studio needs beside its .exe — the same list CMakeLists.txt copies into the build.
Source: "{#BuildDir}\studio.exe";         DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\studio.style";       DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\studio.common.style"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\jaytek-logo.png";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\jaytek-landing.png"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\UbuntuSans.ttf";     DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\UbuntuSans-LICENCE.txt"; DestDir: "{app}"; Flags: ignoreversion
; The licence the studio and the firmware kit are conveyed under, the firmware's additional permission,
; and what third-party code both contain. The GPL asks that every recipient gets a copy.
Source: "..\..\..\LICENSE";           DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\..\LICENSE.exception"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\..\THIRD-PARTY.md";    DestDir: "{app}"; Flags: ignoreversion
; The WinUSB driver installer for the ECU's bootloader (third_party/libwdi-win), and its licence.
Source: "{#BuildDir}\driver\wdi-simple.exe"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "..\third_party\libwdi-win\COPYING-LGPL.txt"; DestDir: "{app}\driver"; Flags: ignoreversion
; The shipped sensor calibration presets, beside the executable (Apply Preset lists them).
Source: "..\calibrations\*";   DestDir: "{app}\calibrations"; Flags: ignoreversion

; The user manual (make manual), beside the executable: Help ▸ User Manual opens {app}\manual\index.html.
Source: "..\..\..\manual\site\*";   DestDir: "{app}\manual"; Flags: ignoreversion recursesubdirs createallsubdirs
; The ECU firmware kits, one per board: what the studio offers as an update and installs for recovery,
; each with the descriptor and dashboard drawn for it. Older firmware's are fetched from their releases
; when an ECU running one connects. EVERY board's kit ships: recovery must be able to put firmware on a
; virgin board of any type. Passed in by `make studio-installer` as /DKitsDir (the folder `make ship-kits` fills).
#ifdef KitsDir
Source: "{#KitsDir}\*"; DestDir: "{app}\firmware"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
; The CAN device templates (shared/can_templates, made by codegen).
Source: "..\..\..\shared\can_templates\*.json"; DestDir: "{app}\can_templates"; Flags: ignoreversion

[InstallDelete]
; What shipped is replaced, not added to: last version's kit and templates go before this version's arrive.
Type: filesandordirs; Name: "{app}\firmware"
Type: filesandordirs; Name: "{app}\can_templates"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\studio.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\Install the jayecu USB driver"; Filename: "{app}\driver\wdi-simple.exe"; \
    Parameters: "{#DriverArgs}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\studio.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\driver\wdi-simple.exe"; Parameters: "{#DriverArgs}"; \
    Verb: runas; Flags: shellexec waituntilterminated; Tasks: usbdriver; Check: not WizardSilent; \
    StatusMsg: "Installing the USB driver for the ECU's bootloader..."
Filename: "{app}\studio.exe"; WorkingDir: "{app}"; Description: "Start {#AppName}"; Flags: nowait postinstall
