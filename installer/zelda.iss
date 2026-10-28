; Installer for the CD-i static recompilation.  Built by package.ps1, which
; passes the values below with /D (version from game.json, staged files).
;
; The game is portable: it keeps saves, quicksaves, settings and game.ini in
; its own folder.  So it is installed per user, into a folder the user can
; write to (no administrator rights), and every file it creates stays there.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppTitle
  #define AppTitle "Zelda's Adventure"
#endif
; the title inside Pascal string literals ([Code]): quotes doubled
#define AppTitlePas StringChange(AppTitle, "'", "''")
#ifndef ExeName
  #define ExeName "zelda"
#endif
#ifndef SourceDir
  #error SourceDir (the staged package folder) must be given with /DSourceDir=...
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F1E2C1A-5B7D-4C1E-9A43-3D2B6C0E7A11}
AppName={#AppTitle} (CD-i recompilation)
AppVersion={#AppVersion}
AppVerName={#AppTitle} (CD-i recompilation) {#AppVersion}
DefaultDirName={localappdata}\Programs\{#AppTitle} (CD-i)
DefaultGroupName={#AppTitle} (CD-i)
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename={#ExeName}-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#ExeName}.exe
; the player's data in {app}\saves stays when the game is uninstalled

[Tasks]
Name: "desktopicon"; Description: "Create desktop shortcuts"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "game.ini"
; a fresh game.ini only if there is none (an update or reinstall keeps the player's)
Source: "{#SourceDir}\game.ini"; DestDir: "{app}"; Flags: onlyifdoesntexist uninsneveruninstall

[INI]
Filename: "{app}\game.ini"; Section: "game"; Key: "rom"; String: "{code:RomPath}"; Check: RomChosen

[Icons]
Name: "{group}\{#AppTitle}"; Filename: "{app}\{#ExeName}.exe"; WorkingDir: "{app}"
Name: "{group}\{#AppTitle} HD (Cartoon HD)"; Filename: "{app}\{#ExeName}_hd.exe"; WorkingDir: "{app}"
Name: "{group}\Game folder (saves, settings)"; Filename: "{app}"
Name: "{group}\Uninstall {#AppTitle}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppTitle}"; Filename: "{app}\{#ExeName}.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\{#AppTitle} HD"; Filename: "{app}\{#ExeName}_hd.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#ExeName}.exe"; Description: "Start {#AppTitle} now"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\update"

[Code]
var
  RomPage: TInputFileWizardPage;

procedure InitializeWizard;
begin
  RomPage := CreateInputFilePage(wpSelectDir,
    'Game disc image',
    'Where is your copy of the {#AppTitlePas} disc?',
    'The game runs from your own CD-i disc image: a raw image in .cue/.bin form ' +
    '(2352-byte sectors), for example "{#AppTitlePas} (Europe).cue". It is not included ' +
    'and is not copied: the game reads it from where it is.' + #13#10#13#10 +
    'You can leave this empty and choose the file when the game first starts.');
  RomPage.Add('Disc image (.cue or .bin):', 'CD-i disc image (*.cue;*.bin)|*.cue;*.bin|All files|*.*', '.cue');
  { unattended installs: setup.exe /VERYSILENT /ROM="D:\images\game.cue" }
  RomPage.Values[0] := ExpandConstant('{param:ROM|}');
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  { reinstall/update: offer the disc image chosen before }
  if (CurPageID = RomPage.ID) and (RomPage.Values[0] = '') then
    RomPage.Values[0] := GetIniString('game', 'rom', '', AddBackslash(WizardDirValue) + 'game.ini');
end;

function RomPath(Param: String): String;
begin
  Result := RomPage.Values[0];
end;

function RomChosen: Boolean;
begin
  Result := Trim(RomPage.Values[0]) <> '';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  F, Ext: String;
begin
  Result := True;
  if CurPageID <> RomPage.ID then Exit;
  F := Trim(RomPage.Values[0]);
  if WizardSilent then Exit;      { unattended: take /ROM= as given (or none) }
  if F = '' then
  begin
    Result := MsgBox('No disc image chosen. The game will ask for it when it first starts.' + #13#10 +
                     'Continue without it?', mbConfirmation, MB_YESNO) = IDYES;
    Exit;
  end;
  Ext := LowerCase(ExtractFileExt(F));
  if not FileExists(F) then
  begin
    SuppressibleMsgBox('That file does not exist.', mbError, MB_OK, IDOK);
    Result := False;
  end
  else if (Ext <> '.cue') and (Ext <> '.bin') then
  begin
    SuppressibleMsgBox('Please choose the .cue or the .bin file of the disc image.', mbError, MB_OK, IDOK);
    Result := False;
  end;
end;
