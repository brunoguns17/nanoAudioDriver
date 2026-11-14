; ============================================================================
; Installer_Setup.iss
; Script Inno Setup 6 para gerar o instalador executável oficial do Driver ASIO
; nanoAudio ASIO Driver (32-bit e 64-bit + Painel de Controle + Atualizador)
;
; Compile pelo Compilar_e_Gerar_Instalador.bat: ele lê a versão em
; nanoAudioVersion.h e a repassa com /DAppVersion=x.y.z.
; ============================================================================

#ifndef AppVersion
  #error "AppVersion não definido. Use Compilar_e_Gerar_Instalador.bat (ele passa /DAppVersion a partir de nanoAudioVersion.h)."
#endif

#define AppName "nanoAudio ASIO Driver"
#define AppPublisher "nanoAudio Audio Project"
#define AppId "{{8E6A2A41-5C3B-4F92-9A1D-7E3F9B02C4D8}"
#define PowerShellExe "{sys}\WindowsPowerShell\v1.0\powershell.exe"

[Setup]
AppId={#AppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
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
; Em instalações manuais o Inno oferece fechar programas que usam o driver.
; O atualizador automático sempre passa /NOCLOSEAPPLICATIONS (nunca fecha a DAW).
CloseApplications=yes
RestartApplications=no
DisableProgramGroupPage=yes

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; A tarefa agendada roda como SYSTEM e só VERIFICA (nunca instala): grava a versão nova em
; {commonappdata}\nanoAudio\update-available.txt, que o painel lê ao abrir. Só existe em
; instalação de administrador (o executável fica em Program Files).
; O nome "autoupdate" é mantido para o Inno lembrar a escolha feita em versões anteriores.
Name: "autoupdate"; Description: "Avisar quando houver atualizações (consulta o GitHub ao iniciar o Windows e uma vez por dia; o aviso aparece ao abrir o Painel de Controle)"; GroupDescription: "Atualizações:"; Check: IsAdminInstallMode

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

; Atualizador + script que registra a tarefa agendada de verificação
Source: "nanoAudioUpdater.exe"; DestDir: "{app}"; Flags: ignoreversion restartreplace
Source: "nanoAudioUpdateTask.ps1"; DestDir: "{app}"; Flags: ignoreversion

; Documentação e Guia
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Painel de Controle nanoAudio"; Filename: "{app}\nanoAudioControlPanel.exe"
Name: "{group}\Verificar atualizações do nanoAudio"; Filename: "{app}\nanoAudioUpdater.exe"; Parameters: "--interactive"
Name: "{group}\README - Guia de Uso"; Filename: "{app}\README.md"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Painel de Controle nanoAudio"; Filename: "{app}\nanoAudioControlPanel.exe"; Tasks: desktopicon

[Run]
; Registra (ou remove) a tarefa agendada do atualizador conforme a opção escolhida.
; Roda também em instalações silenciosas, inclusive as feitas pelo próprio atualizador.
Filename: "{#PowerShellExe}"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\nanoAudioUpdateTask.ps1"" -Action Register -UpdaterPath ""{app}\nanoAudioUpdater.exe"""; Flags: runhidden waituntilterminated; StatusMsg: "Configurando a verificação de atualizações..."; Tasks: autoupdate; Check: IsAdminInstallMode
Filename: "{#PowerShellExe}"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\nanoAudioUpdateTask.ps1"" -Action Unregister"; Flags: runhidden waituntilterminated; Tasks: not autoupdate; Check: IsAdminInstallMode

; Executar o Painel de Controle ao final da instalação
Filename: "{app}\nanoAudioControlPanel.exe"; Description: "Abrir o Painel de Controle nanoAudio agora"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{#PowerShellExe}"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\nanoAudioUpdateTask.ps1"" -Action Unregister"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveUpdateTask"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\Updates"
Type: filesandordirs; Name: "{commonappdata}\nanoAudio"
