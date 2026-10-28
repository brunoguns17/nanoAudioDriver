; ============================================================================
; Installer_Setup.iss
; Script Inno Setup 6 para gerar o instalador executável oficial do Driver ASIO
; nanoAudio ASIO Driver (32-bit e 64-bit + Painel de Controle Integrado v2.5)
; ============================================================================

#define AppName "nanoAudio ASIO Driver"
#define AppVersion "2.5.0"
#define AppPublisher "nanoAudio Audio Project"
#define AppId "{{8E6A2A41-5C3B-4F92-9A1D-7E3F9B02C4D8}"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\nanoAudio
DefaultGroupName=nanoAudio ASIO
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
OutputBaseFilename=nanoAudio_Setup_v{#AppVersion}
OutputDir=Output
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
DisableProgramGroupPage=yes

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; Remove automaticamente quaisquer arquivos com o nome antigo de versões anteriores
Type: files; Name: "{app}\UniversalWasapiAsio*.dll"
Type: files; Name: "{app}\UniversalWasapiAsio*.exe"
Type: filesandordirs; Name: "{autopf}\UniversalWasapiAsio"

[Files]
; Driver ASIO nativo 64-bit (para DAWs 64-bit como Reaper, Ableton, FL Studio 64, Cubase, Studio One)
Source: "nanoAudioDriver.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 64bit restartreplace

; Driver ASIO nativo 32-bit (para DAWs 32-bit e softwares legados)
Source: "nanoAudioDriver32.dll"; DestDir: "{app}"; Flags: ignoreversion regserver 32bit restartreplace

; Aplicativo Executável do Painel de Controle
Source: "nanoAudioControlPanel.exe"; DestDir: "{app}"; Flags: ignoreversion

; Documentação e Guia
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Painel de Controle nanoAudio"; Filename: "{app}\nanoAudioControlPanel.exe"
Name: "{group}\README - Guia de Uso"; Filename: "{app}\README.md"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Painel de Controle nanoAudio"; Filename: "{app}\nanoAudioControlPanel.exe"; Tasks: desktopicon

[Run]
; Executar o Painel de Controle ao final da instalação
Description: "Abrir o Painel de Controle nanoAudio agora"; Flags: nowait postinstall skipifsilent; Filename: "{app}\nanoAudioControlPanel.exe"
