#ifndef StageDir
  #error StageDir must point to the prepared installer files.
#endif
#ifndef OutputFolder
  #define OutputFolder "..\dist"
#endif

[Setup]
AppId={{E4BB3429-C471-4EE3-83A6-02A74E32D850}
AppName=Life-sim Workbench
AppVersion=2026.9.23
VersionInfoVersion=2026.9.23.0
DefaultDirName={localappdata}\Programs\LifeSimWorkbench
DefaultGroupName=Life-sim Workbench
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
DisableProgramGroupPage=yes
DisableWelcomePage=no
UsePreviousSetupType=yes
OutputDir={#OutputFolder}
OutputBaseFilename=LifeSimWorkbench-Setup
Compression=lzma2/normal
SolidCompression=yes
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\native\workbench.exe
InfoAfterFile={#StageDir}\WELCOME.txt

[Types]
Name: full; Description: "Workbench with all simulation templates"
Name: empty; Description: "Empty workbench - create or open your own simulations"
Name: custom; Description: "Choose individual simulation templates"; Flags: iscustom

[Components]
Name: core; Description: "Workbench"; Types: full empty custom; Flags: fixed
Name: tools; Description: "C++ build tools and matching source - build your own simulations"; Types: full empty
Name: templates; Description: "Simulation templates"; Types: full

[Files]
Source: "{#StageDir}\native\workbench.exe"; DestDir: "{app}\native"; Flags: ignoreversion; Components: core
Source: "{#StageDir}\native\bench_run.exe"; DestDir: "{app}\native"; Flags: ignoreversion; Components: core
Source: "{#StageDir}\native\workbench.install"; DestDir: "{app}\native"; Flags: ignoreversion; Components: core
Source: "{#StageDir}\native\src\*"; DestDir: "{app}\native\src"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core
Source: "{#StageDir}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core
Source: "{#StageDir}\WELCOME.txt"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "{#StageDir}\LICENSING.md"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "{#StageDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion; Components: core
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core
Source: "{#StageDir}\source\*"; DestDir: "{app}\source"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: core
Source: "{#StageDir}\toolchain\*"; DestDir: "{app}\toolchain"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: tools

[Dirs]
Name: "{app}\templates"; Components: core

[Icons]
Name: "{autoprograms}\Life-sim Workbench"; Filename: "{app}\native\workbench.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\native\workbench.exe"; Description: "Open Life-sim Workbench"; Flags: nowait postinstall skipifsilent

[Messages]
SelectComponentsLabel2=Choose all templates, individual templates, or an empty bench. You can add .benchsim simulations later. Your saved simulations are kept separately from the application.

; Only the installer's named starter files are removed when deselected. User
; projects live in LocalAppData\LifeSimWorkbench and are never uninstalled.
#include StageDir + "\templates.iss"
