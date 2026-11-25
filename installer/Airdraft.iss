; Airdraft setup (Inno Setup 6). Built by scripts\build-installer.ps1 from build\stage.
; One exe installs everything: the client and service (self-contained, no .NET
; needed), the user-mode radio, awdld + Wintun, the AirDrop engine with its own
; Python, the register-proxy driver, TAP-Windows, firewall rules and the service.
#define AppVersion "0.1.6"

[Setup]
AppId={{F4973E16-2FF2-4C71-B6F0-2AE0B9A888EA}
AppName=Airdraft Developer Preview
AppVersion={#AppVersion}
AppPublisher=Airdraft
DefaultDirName={autopf}\Airdraft
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputBaseFilename=Airdraft-{#AppVersion}-Developer-Preview-Setup
OutputDir=..\build
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
SetupIconFile=../app/assets/Airdraft.ico
UninstallDisplayIcon={app}\Airdraft.exe
CloseApplications=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
en.Installing=Installing the driver, TAP adapter, firewall rules and the Airdraft radio service...
en.Failed=Airdraft's system parts did not install:%n%n%1%n%nFull log: %2
en.StartNow=Start Airdraft
en.Desktop=Create a desktop shortcut

[Tasks]
Name: "desktop"; Description: "{cm:Desktop}"; Flags: unchecked

[Files]
Source: "..\build\stage\*"; DestDir: "{tmp}\Airdraft-stage"; Flags: recursesubdirs ignoreversion deleteafterinstall

[Icons]
; The AppUserModelID lets Windows show Airdraft's notifications under its own name.
Name: "{autoprograms}\Airdraft"; Filename: "{app}\Airdraft.exe"; AppUserModelID: "Airdraft"
Name: "{autodesktop}\Airdraft"; Filename: "{app}\Airdraft.exe"; AppUserModelID: "Airdraft"; Tasks: desktop
; One-click rollback: gives the Wi-Fi card back to Intel's driver (restore.ps1).
Name: "{autoprograms}\Airdraft - restore Windows Wi-Fi"; Filename: "{app}\restore.cmd"; IconFilename: "{sys}\netshell.dll"; IconIndex: 0
; Diagnostic report for bug reports: logs and system details, redacted (report.ps1).
Name: "{autoprograms}\Airdraft - diagnostic report"; Filename: "{app}\report.cmd"; IconFilename: "{sys}\imageres.dll"; IconIndex: 76

[Run]
; Start the tray in the signed-in user's session.
Filename: "{app}\Airdraft.exe"; Description: "{cm:StartNow}"; Flags: postinstall nowait skipifsilent runasoriginaluser
; An update started from Airdraft runs silently: start the new version again.
Filename: "{app}\Airdraft.exe"; Parameters: "--background"; Flags: nowait runasoriginaluser; Check: WizardSilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\uninstall.ps1"" -Target ""{app}"""; \
  Flags: runhidden waituntilterminated; RunOnceId: "Airdraft-uninstall"

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Err: AnsiString;
  Log: String;
begin
  if CurStep <> ssPostInstall then Exit;
  WizardForm.StatusLabel.Caption := CustomMessage('Installing');
  Log := ExpandConstant('{commonappdata}\Airdraft\install.log');
  if not Exec('powershell.exe',
      '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{tmp}\Airdraft-stage\install.ps1') +
      '" -Target "' + ExpandConstant('{app}') + '"',
      '', SW_HIDE, ewWaitUntilTerminated, Code) then
    Code := -1;
  if Code <> 0 then
  begin
    if not LoadStringFromFile(ExpandConstant('{commonappdata}\Airdraft\install-error.txt'), Err) then
      Err := 'install.ps1 exit code ' + IntToStr(Code);
    RaiseException(FmtMessage(CustomMessage('Failed'), [String(Err), Log]));
  end;
end;
