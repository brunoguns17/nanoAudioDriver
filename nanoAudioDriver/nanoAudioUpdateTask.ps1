# ============================================================================
# nanoAudioUpdateTask.ps1
# Registra / remove a tarefa agendada que verifica atualizações do nanoAudio.
# Chamado pelo instalador (Inno Setup) com privilégios de administrador.
#
#   -Action Register   -UpdaterPath "C:\Program Files\nanoAudio\nanoAudioUpdater.exe"
#   -Action Unregister
#
# A tarefa roda como SYSTEM ao iniciar o Windows (+5 min) e diariamente às 12:00
# (se o PC estiver desligado no horário, roda assim que possível). Ela só
# verifica e grava <pasta de instalação>\update-available.txt; quem decide
# instalar é o usuário, na tela de aviso que aparece ao abrir o Painel de Controle.
# ============================================================================

param(
    [Parameter(Mandatory = $true)][ValidateSet('Register', 'Unregister')][string]$Action,
    [string]$UpdaterPath,
    # Somente para testes sem administrador: registra para o usuário atual.
    [switch]$CurrentUserForTesting,
    [string]$TaskName = 'Atualizador automatico'
)

$ErrorActionPreference = 'Stop'
$TaskPath = '\nanoAudio\'

if ($Action -eq 'Unregister') {
    Unregister-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    exit 0
}

if (-not $UpdaterPath -or -not (Test-Path -LiteralPath $UpdaterPath -PathType Leaf)) {
    Write-Error "Atualizador não encontrado: $UpdaterPath"
    exit 1
}

$taskAction = New-ScheduledTaskAction -Execute $UpdaterPath -Argument '--silent' `
    -WorkingDirectory (Split-Path -Parent $UpdaterPath)

$atStartup = New-ScheduledTaskTrigger -AtStartup
$atStartup.Delay = 'PT5M'
$daily = New-ScheduledTaskTrigger -Daily -At '12:00'

$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -RunOnlyIfNetworkAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 30)

if ($CurrentUserForTesting) {
    $principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
    $triggers = @($daily)   # gatilho de inicialização exige administrador
} else {
    $principal = New-ScheduledTaskPrincipal -UserId 'NT AUTHORITY\SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $triggers = @($atStartup, $daily)
}

Register-ScheduledTask -TaskPath $TaskPath -TaskName $TaskName `
    -Action $taskAction -Trigger $triggers -Settings $settings -Principal $principal `
    -Description 'Verifica no GitHub se há uma nova versão do nanoAudio. Não instala nada: o aviso aparece quando o Painel de Controle for aberto.' `
    -Force | Out-Null

exit 0
