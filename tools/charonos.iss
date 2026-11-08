#ifndef PackageDir
  #error PackageDir must point to the deployed application folder
#endif
#ifndef Version
  #define Version "1.2.2"
#endif
#ifndef DisplayVersion
  #define DisplayVersion Version
#endif
#ifndef PackageVersion
  #define PackageVersion Version
#endif
; Full edition by default; the Lite edition passes /DLite (own AppId, folder and exe name, so
; both can be installed side by side).
#ifdef Lite
  #define AppName "CHARONOS Lite"
  #define ExeName "CHARONOS-Lite.exe"
  #define AppGuid "{{7C1E4B52-3D0A-4F6E-9B7F-2A61C4D8E913}"
  #define SetupBase "CHARONOS-Lite"
  #define ProgId "CHRONOS.Lite.Project"
#else
  #define AppName "CHARONOS"
  #define ExeName "CHARONOS.exe"
  #define AppGuid "{{541A5B3B-408C-478D-AD80-2AE20787183E}"
  #define SetupBase "CHARONOS"
  #define ProgId "CHRONOS.Project"
#endif

[Setup]
AppId={#AppGuid}
AppName={#AppName}
AppVersion={#DisplayVersion}
AppPublisher=CHARONOS
AppComments=Frame-accurate reference review and analysis for animators
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename={#SetupBase}-{#PackageVersion}-Setup-x64
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\{#ExeName}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no
ChangesAssociations=yes
VersionInfoVersion={#Version}
VersionInfoTextVersion={#DisplayVersion}
VersionInfoProductTextVersion={#DisplayVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[Tasks]
Name: "startmenu"; Description: "{cm:CreateStartMenuIcon}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
Name: "associatefrvm"; Description: "Associate .frvm project files with CHARONOS"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#ExeName}"; Tasks: startmenu
Name: "{group}\{#AppName} User Guide"; Filename: "{app}\docs\CHARONOS-User-Guide.html"; Tasks: startmenu
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#ExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\.frvm\OpenWithProgids"; ValueType: string; ValueName: "{#ProgId}"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associatefrvm
Root: HKCU; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: ""; ValueData: "CHARONOS Project"; Flags: uninsdeletekey; Tasks: associatefrvm
Root: HKCU; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#ExeName}"",0"; Tasks: associatefrvm
Root: HKCU; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#ExeName}"" ""%1"""; Tasks: associatefrvm

[Run]
Filename: "{app}\{#ExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

[CustomMessages]
english.CreateStartMenuIcon=Create a Start menu shortcut
korean.CreateStartMenuIcon=시작 메뉴 바로 가기 만들기
