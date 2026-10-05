; =====================================================================
; AuDuo Inno Setup Script
; Dual-Output Bluetooth & Device Audio Synchronizer for Windows
; Built by Aivin Koshy
; =====================================================================

#define MyAppName "AuDuo"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Aivin Koshy"
#define MyAppURL "https://github.com/AivinKoshy/auduo"
#define MyAppExeName "auduo.exe"

[Setup]
AppId={{9F3B56A0-1412-4217-A103-6058097D87E6}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=AuDuo_Setup_v1.0.0
SetupIconFile=..\resources\auduo.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "installvbcable"; Description: "Download and Install VB-Audio Virtual Cable (Recommended)"; GroupDescription: "Virtual Audio Driver:"; Flags: checkedonce

[Files]
Source: "..\bin\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\ROADMAP.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\donate\*"; DestDir: "{app}\docs\donate"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\resources\auduo.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\auduo.ico"
Name: "{group}\Support & Donate"; Filename: "{app}\docs\donate\index.html"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\auduo.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Messages]
BeveledLabel=AuDuo - Dual Audio Mirroring & Auto-Sync

[Code]
// Custom Inno Setup Pascal code for VB-Audio Virtual Cable download & setup
var
  VBCableNoticeLabel: TLabel;

procedure InitializeWizard;
var
  AckPage: TWizardPage;
  Memo: TNewMemo;
begin
  // Create an explicit Acknowledgements page in the wizard
  AckPage := CreateCustomPage(wpLicense, 'Third-Party Acknowledgements & Driver Setup', 'AuDuo relies on virtual audio routing for seamless dual streaming.');

  Memo := TNewMemo.Create(AckPage);
  Memo.Parent := AckPage.Surface;
  Memo.Left := ScaleX(0);
  Memo.Top := ScaleY(0);
  Memo.Width := AckPage.SurfaceWidth;
  Memo.Height := ScaleY(220);
  Memo.ScrollBars := ssVertical;
  Memo.ReadOnly := True;
  Memo.Lines.Add('SPECIAL ACKNOWLEDGEMENT & THANKS:');
  Memo.Lines.Add('----------------------------------------------------');
  Memo.Lines.Add('AuDuo makes use of VB-Audio Virtual Cable (VB-Audio Software).');
  Memo.Lines.Add('We express our sincere gratitude and acknowledgement to Vincent Burel');
  Memo.Lines.Add('and the VB-Audio team (https://vb-audio.com) for their brilliant');
  Memo.Lines.Add('contributions to Windows digital audio routing.');
  Memo.Lines.Add('');
  Memo.Lines.Add('VIRTUAL CABLE DRIVER:');
  Memo.Lines.Add('VB-Audio Cable is Donationware / Freeware for personal use.');
  Memo.Lines.Add('AuDuo does not bundle or sell this proprietary driver.');
  Memo.Lines.Add('If enabled in the following step, the installer will download the official');
  Memo.Lines.Add('driver package directly from download.vb-audio.com and launch its setup.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  TempZip: String;
  TempDir: String;
  CmdDownload: String;
  CmdExtract: String;
begin
  if CurStep = ssPostInstall then
  begin
    if WizardIsTaskSelected('installvbcable') then
    begin
      TempDir := ExpandConstant('{tmp}\vbcable');
      TempZip := TempDir + '\vbcable.zip';
      CreateDir(TempDir);

      WizardForm.StatusLabel.Caption := 'Downloading VB-Audio Virtual Cable from official server...';

      // Download official zip via curl.exe (built-in on Windows 10/11)
      CmdDownload := '/C curl.exe -f -s -S -L "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip" -o "' + TempZip + '"';
      if Exec('cmd.exe', CmdDownload, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
      begin
        WizardForm.StatusLabel.Caption := 'Extracting VB-Audio Virtual Cable...';
        // Extract via tar.exe (built-in on Windows 10/11)
        CmdExtract := '/C tar.exe -xf "' + TempZip + '" -C "' + TempDir + '"';
        if Exec('cmd.exe', CmdExtract, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
        begin
          WizardForm.StatusLabel.Caption := 'Launching VB-Audio Cable Setup...';
          // Run official x64 driver installer (prompts UAC elevation as required by Windows driver installation)
          ShellExec('runas', TempDir + '\VBCABLE_Setup_x64.exe', '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode);
        end;
      end
      else
      begin
        MsgBox('Could not download VB-Audio Cable automatically. Please check your internet connection or install it manually from https://vb-audio.com/Cable/.', mbInformation, MB_OK);
      end;
    end;
  end;
end;
