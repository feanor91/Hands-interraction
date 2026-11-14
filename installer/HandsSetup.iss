; HandsSetup.iss — installateur de Hands Interaction (Inno Setup 6).
;
; Compiler :  ISCC.exe /DAppVersion=0.0.1 /DBuildDir=<dossier de build> installer\HandsSetup.iss
; (en pratique : scripts\build-release.ps1, qui construit tout puis appelle ISCC).
;
; Ce que fait l'installation (equivalent de scripts\install.ps1, plus la tray et le pont SimConnect) :
;   * C:\ProgramData\HandsLayer : couche OpenXR (DLL + manifeste), hands.ini (conserve s'il existe), logs et data (zones.ini)
;     avec les droits d'acces voulus : la DLL n'est modifiable que par les administrateurs, mais chargeable par les applications Store (MSFS) ;
;   * le registre OpenXR (couche implicite) ;
;   * C:\Program Files\Hands Interaction : HandsTray.exe (barre des taches) et HandsBridge.exe (pont SimConnect, lance par la tray).

#ifndef AppVersion
  #define AppVersion "0.0.1"
#endif
#ifndef BuildDir
  #define BuildDir "..\build-release"
#endif

#define LayerName "XR_APILAYER_HANDS_bare"
#define DataDir "{commonappdata}\HandsLayer"

[Setup]
AppId={{6F1D3B0A-5C0E-4D7B-9B7E-3A8E2A4C7F11}
AppName=Hands Interaction
AppVersion={#AppVersion}
AppVerName=Hands Interaction {#AppVersion}
AppPublisher=feanor91
AppPublisherURL=https://github.com/feanor91/Hands-interraction
DefaultDirName={autopf}\Hands Interaction
DefaultGroupName=Hands Interaction
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=HandsInteraction-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=hands.ico
UninstallDisplayIcon={app}\HandsTray.exe
UninstallDisplayName=Hands Interaction
VersionInfoVersion={#AppVersion}
SetupLogging=yes

[Languages]
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "autostart"; Description: "Lancer l'icone de la barre des taches au demarrage de Windows"; GroupDescription: "Options :"

[Dirs]
Name: "{#DataDir}"
Name: "{#DataDir}\logs"; Flags: uninsneveruninstall
Name: "{#DataDir}\data"; Flags: uninsneveruninstall

[Files]
; Couche OpenXR : chargee par MSFS.
Source: "{#BuildDir}\layer\Release\{#LayerName}.dll"; DestDir: "{#DataDir}"; Flags: ignoreversion restartreplace
Source: "{#BuildDir}\layer\Release\{#LayerName}.json"; DestDir: "{#DataDir}"; Flags: ignoreversion
; Configuration : jamais ecrasee, jamais supprimee a la desinstallation (vos reglages restent).
Source: "{#BuildDir}\layer\Release\hands.ini"; DestDir: "{#DataDir}"; Flags: onlyifdoesntexist uninsneveruninstall
; Programmes.
Source: "{#BuildDir}\tray\Release\HandsTray.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\tools\Release\HandsBridge.exe"; DestDir: "{app}"; Flags: ignoreversion
; Copie "source" a cote de la tray : c'est la que le bouton Installer / Mettre a jour de la tray cherche la DLL et scripts\install.ps1.
Source: "{#BuildDir}\layer\Release\{#LayerName}.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\layer\Release\{#LayerName}.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\layer\Release\hands.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\scripts\install.ps1"; DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\scripts\uninstall.ps1"; DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Couche implicite OpenXR : nom = chemin du manifeste, DWORD 0 = active.
Root: HKLM; Subkey: "SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"; ValueType: dword; ValueName: "{#DataDir}\{#LayerName}.json"; ValueData: "0"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "HandsInteractionTray"; ValueData: """{app}\HandsTray.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Icons]
Name: "{autoprograms}\Hands Interaction"; Filename: "{app}\HandsTray.exe"
Name: "{autoprograms}\Hands Interaction - dossier de configuration"; Filename: "{#DataDir}"

[Run]
; Droits (SID : S-1-5-18 SYSTEM, S-1-5-32-544 Administrateurs, S-1-5-32-545 Utilisateurs, S-1-15-2-1 tous les packages d'application = MSFS Store).
Filename: "{sys}\icacls.exe"; Parameters: """{#DataDir}"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX *S-1-15-2-1:(OI)(CI)RX *S-1-15-2-2:(OI)(CI)RX"; Flags: runhidden; StatusMsg: "Reglage des droits d'acces..."
Filename: "{sys}\icacls.exe"; Parameters: """{#DataDir}\logs"" /grant *S-1-5-32-545:(OI)(CI)M *S-1-15-2-1:(OI)(CI)M"; Flags: runhidden
Filename: "{sys}\icacls.exe"; Parameters: """{#DataDir}\data"" /grant *S-1-5-32-545:(OI)(CI)M *S-1-15-2-1:(OI)(CI)M"; Flags: runhidden
; Mise a jour : ajoute a hands.ini les reglages apparus depuis (valeurs existantes conservees ; resultat dans logs\install.log).
Filename: "{app}\HandsTray.exe"; Parameters: "--merge-ini ""{app}\hands.ini"" ""{#DataDir}\hands.ini"""; Flags: runhidden waituntilterminated; StatusMsg: "Mise a jour de hands.ini..."
Filename: "{sys}\icacls.exe"; Parameters: """{#DataDir}\hands.ini"" /grant *S-1-5-32-545:M"; Flags: runhidden
Filename: "{app}\HandsTray.exe"; Description: "Lancer Hands Interaction maintenant"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM HandsTray.exe"; Flags: runhidden; RunOnceId: "StopTray"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM HandsBridge.exe"; Flags: runhidden; RunOnceId: "StopBridge"

[Code]
function IsRunning(const Exe: String): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\cmd.exe'), '/c tasklist /fi "imagename eq ' + Exe + '" | find /i "' + Exe + '"', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

function CheckSim(): Boolean;
begin
  Result := True;
  if IsRunning('FlightSimulator2024.exe') or IsRunning('FlightSimulator.exe') then
  begin
    MsgBox('Microsoft Flight Simulator est lance : fermez-le avant d''installer ou de desinstaller (la couche est chargee par le jeu).', mbError, MB_OK);
    Result := False;
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := CheckSim();
end;

function InitializeUninstall(): Boolean;
begin
  Result := CheckSim();
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  // Ferme l'ancienne tray et l'ancien pont pour pouvoir remplacer leurs fichiers.
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM HandsTray.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM HandsBridge.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;
