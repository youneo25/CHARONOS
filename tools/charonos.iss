#ifndef PackageDir
  #error PackageDir must point to the deployed application folder
#endif
#ifndef Version
  #define Version "1.1.0"
#endif
#ifndef DisplayVersion
  #define DisplayVersion Version
#endif
#ifndef PackageVersion
  #define PackageVersion Version
#endif

[Setup]
AppId={{541A5B3B-408C-478D-AD80-2AE20787183E}
AppName=CHARONOS
AppVersion={#DisplayVersion}
AppPublisher=CHARONOS
AppComments=Frame-accurate reference review and analysis for animators
DefaultDirName={localappdata}\Programs\CHARONOS
DefaultGroupName=CHARONOS
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=CHARONOS-{#PackageVersion}-Setup-x64
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\CHARONOS.exe
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
Name: "{group}\CHARONOS"; Filename: "{app}\CHARONOS.exe"; Tasks: startmenu
Name: "{group}\CHARONOS User Guide"; Filename: "{app}\docs\CHARONOS-User-Guide.html"; Tasks: startmenu
Name: "{autodesktop}\CHARONOS"; Filename: "{app}\CHARONOS.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\.frvm\OpenWithProgids"; ValueType: string; ValueName: "CHRONOS.Project"; ValueData: ""; Flags: uninsdeletevalue; Tasks: associatefrvm
Root: HKCU; Subkey: "Software\Classes\CHRONOS.Project"; ValueType: string; ValueName: ""; ValueData: "CHARONOS Project"; Flags: uninsdeletekey; Tasks: associatefrvm
Root: HKCU; Subkey: "Software\Classes\CHRONOS.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\CHARONOS.exe"",0"; Tasks: associatefrvm
Root: HKCU; Subkey: "Software\Classes\CHRONOS.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\CHARONOS.exe"" ""%1"""; Tasks: associatefrvm

[Run]
Filename: "{app}\CHARONOS.exe"; Description: "{cm:LaunchProgram,CHARONOS}"; Flags: nowait postinstall skipifsilent

[CustomMessages]
english.CreateStartMenuIcon=Create a Start menu shortcut
korean.CreateStartMenuIcon=시작 메뉴 바로 가기 만들기
