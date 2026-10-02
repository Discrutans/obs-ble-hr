; Inno Setup script for BLE Heart Rate OBS plugin
; Compile with: ISCC.exe installer\ble-heart-rate.iss
; Requires dist\ble-heart-rate\ with bin\64bit\ and data\

#define MyAppName "BLE Heart Rate (OBS)"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Discrutans"
#define MyAppURL "https://github.com/Discrutans/obs-ble-hr"
#define MyPluginName "ble-heart-rate"

[Setup]
AppId={{A7C3E9D1-4B2F-4E8A-9C11-0B5E2F8D6A31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
DefaultDirName={commonappdata}\obs-studio\plugins\{#MyPluginName}
DisableDirPage=yes
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename={#MyPluginName}-windows-x64-v{#MyAppVersion}-setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\bin\64bit\{#MyPluginName}.dll
InfoAfterFile=after-install.txt

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
Source: "..\dist\{#MyPluginName}\bin\64bit\*"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion recursesubdirs
Source: "..\dist\{#MyPluginName}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
  if not DirExists(ExpandConstant('{commonappdata}\obs-studio')) then
  begin
    MsgBox('OBS Studio data folder was not found under ProgramData.'#13#10 +
           'The plugin will still be installed; start OBS once after install.',
           mbInformation, MB_OK);
  end;
end;
