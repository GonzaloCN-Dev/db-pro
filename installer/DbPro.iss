; Instalador de Db Pro (Inno Setup 6).
; Compilar con: installer\build-installer.ps1

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#define AppName "Db Pro"
#define AppPublisher "Gonzalo Cruz Núñez"
#define AppExe "DbPro.exe"

[Setup]
AppId={{6E3A8F42-9C1D-4B7E-A5D0-2F8B14C73E96}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL=https://github.com/GonzaloCN-Dev/db-pro
AppCopyright=© 2026 {#AppPublisher}
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription=Instalador de {#AppName}
DefaultDirName={autopf}\Db Pro
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
SetupIconFile=..\assets\icon.ico
OutputDir=..\dist
OutputBaseFilename=DbPro-Setup-{#AppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
CloseApplications=yes
RestartApplications=no
DisableProgramGroupPage=yes
LicenseFile=LICENSES.txt

[Languages]
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"

[Components]
Name: "main"; Description: "Db Pro (incluye SQLite)"; Types: full compact custom; Flags: fixed
#ifexist "payload\drivers\dbpro_driver_mysql.dll"
Name: "drv_mysql"; Description: "Driver MySQL / MariaDB (probado con MariaDB 10.4; MySQL sin probar)"; Types: full
#endif
#ifexist "payload\drivers\dbpro_driver_postgres.dll"
Name: "drv_postgres"; Description: "Driver PostgreSQL (probado con PostgreSQL 17)"; Types: full
#endif
#ifexist "payload\drivers\dbpro_driver_mongodb.dll"
Name: "drv_mongodb"; Description: "Driver MongoDB (probado con MongoDB 8.0)"; Types: full
#endif
#ifexist "payload\drivers\dbpro_driver_mssql.dll"
Name: "drv_mssql"; Description: "Driver SQL Server (experimental: sin probar contra un servidor; requiere ODBC Driver 18)"; Types: full
#endif

[Tasks]
Name: "desktopicon"; Description: "Crear un acceso directo en el escritorio"; GroupDescription: "Accesos directos:"; Flags: unchecked

[Files]
Source: "payload\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "payload\*.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "payload\README.md"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "payload\LICENSE"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "LICENSES.txt"; DestDir: "{app}"; Flags: ignoreversion; Components: main
#ifexist "payload\drivers\dbpro_driver_mysql.dll"
Source: "payload\drivers\dbpro_driver_mysql.dll"; DestDir: "{app}\drivers"; Flags: ignoreversion; Components: drv_mysql
#endif
#ifexist "payload\drivers\dbpro_driver_postgres.dll"
Source: "payload\drivers\dbpro_driver_postgres.dll"; DestDir: "{app}\drivers"; Flags: ignoreversion; Components: drv_postgres
#endif
#ifexist "payload\drivers\dbpro_driver_mongodb.dll"
Source: "payload\drivers\dbpro_driver_mongodb.dll"; DestDir: "{app}\drivers"; Flags: ignoreversion; Components: drv_mongodb
#endif
#ifexist "payload\drivers\dbpro_driver_mssql.dll"
Source: "payload\drivers\dbpro_driver_mssql.dll"; DestDir: "{app}\drivers"; Flags: ignoreversion; Components: drv_mssql
#endif


[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "Abrir {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM {#AppExe}"; Flags: runhidden; RunOnceId: "KillApp"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and (not UninstallSilent()) then
    if MsgBox('¿Quieres borrar también tus conexiones guardadas, ajustes y drivers descargados de Db Pro?' + #13#10 +
              '(Las contraseñas están en el Administrador de credenciales de Windows y no se borran aquí.)',
              mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      DelTree(ExpandConstant('{userappdata}\DbPro'), True, True, True);
end;