# ============================================================================
# Publicar_Release.ps1
# Publica uma nova versão do nanoAudio no GitHub. Toda instalação com a opção
# "Atualizar automaticamente" recebe a atualização na próxima verificação.
#
#   powershell -ExecutionPolicy Bypass -File Publicar_Release.ps1 -Version 2.6.1
#   powershell -ExecutionPolicy Bypass -File Publicar_Release.ps1 -Version 2.6.1 -Notes "Correções..."
#
# Etapas:
#   1. Confere pré-requisitos (gh logado, árvore git limpa, tag inédita).
#   2. Grava a versão em nanoAudioVersion.h (fonte única).
#   3. Compila DLLs, painel, atualizador e o instalador (falha = para tudo).
#   4. Gera Output\update-manifest.txt (versão, arquivo, SHA-256).
#   5. Commit + tag vX.Y.Z + push.
#   6. Cria a release como RASCUNHO, envia os arquivos, baixa de volta e
#      confere o SHA-256; só então publica (o "latest" só vê releases publicadas).
#   7. Confirma que o link público do manifesto já aponta para a nova versão.
# ============================================================================

param(
    [Parameter(Mandatory = $true)][ValidatePattern('^\d{1,5}\.\d{1,5}\.\d{1,5}$')][string]$Version,
    [string]$Notes,
    [string]$NotesFile
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$utf8NoBom = New-Object System.Text.UTF8Encoding $false

function Step([string]$msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Fail([string]$msg) { Write-Host "`n[ERRO] $msg" -ForegroundColor Red; exit 1 }
# git/gh escrevem progresso no stderr; no PowerShell 5.1 isso não pode virar erro
# fatal, então as chamadas nativas rodam com 'Continue' e são julgadas pelo código de saída.
function Invoke-Native([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Fail "$exe $($arguments -join ' ') falhou (código $LASTEXITCODE)." }
}
function Test-Native([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    & $exe @arguments *> $null
    return ($LASTEXITCODE -eq 0)
}
function Get-NativeOutput([string]$exe, [string[]]$arguments) {
    $ErrorActionPreference = 'Continue'
    $out = & $exe @arguments 2> $null
    return (($out | Out-String).Trim())
}

# --------------------------------------------------------------------------
Step "Conferindo pré-requisitos"
foreach ($tool in 'git', 'gh') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "'$tool' não encontrado no PATH." }
}
if (-not (Test-Native gh @('auth', 'status'))) { Fail "GitHub CLI não está autenticado. Rode: gh auth login" }

$headerPath = Join-Path $PSScriptRoot 'nanoAudioVersion.h'
$header = [IO.File]::ReadAllText($headerPath)
$repo = [regex]::Match($header, '#define NANOAUDIO_GITHUB_REPO "([^"]+)"').Groups[1].Value
$manifestName = [regex]::Match($header, '#define NANOAUDIO_UPDATE_MANIFEST "([^"]+)"').Groups[1].Value
$current = [regex]::Match($header, '#define NANOAUDIO_VERSION_STR "([^"]+)"').Groups[1].Value
if (-not $repo -or -not $manifestName -or -not $current) { Fail "Não foi possível ler nanoAudioVersion.h." }
if ([version]$Version -lt [version]$current) { Fail "A versão $Version é menor que a atual ($current)." }

$tag = "v$Version"
if (Get-NativeOutput git @('tag', '-l', $tag)) { Fail "A tag $tag já existe." }
if (Test-Native gh @('release', 'view', $tag, '-R', $repo)) { Fail "Já existe uma release $tag no GitHub." }

if (Get-NativeOutput git @('status', '--porcelain')) { Fail "Há alterações não commitadas. Faça commit (ou descarte) antes de publicar." }

$branch = Get-NativeOutput git @('rev-parse', '--abbrev-ref', 'HEAD')
Invoke-Native git @('fetch', '--quiet', 'origin')
$behind = Get-NativeOutput git @('rev-list', '--count', "HEAD..origin/$branch")
if ($behind -and [int]$behind -gt 0) { Fail "O branch local '$branch' está atrás de origin/$branch. Rode git pull antes." }

Write-Host "Repositório: $repo   Branch: $branch   Versão: $current -> $Version"

# --------------------------------------------------------------------------
Step "Gravando versão $Version em nanoAudioVersion.h"
$newHeader = [regex]::Replace($header, '(#define NANOAUDIO_VERSION_STR ")[^"]+(")', "`${1}$Version`${2}")
[IO.File]::WriteAllText($headerPath, $newHeader, $utf8NoBom)

# --------------------------------------------------------------------------
Step "Compilando e gerando o instalador"
$env:NANO_RELEASE = '1'
try {
    $ErrorActionPreference = 'Continue'
    & cmd.exe /c "`"$PSScriptRoot\Compilar_e_Gerar_Instalador.bat`""
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
} finally {
    Remove-Item Env:\NANO_RELEASE -ErrorAction SilentlyContinue
}
$installerName = "nanoAudio_Setup_v$Version.exe"
$installer = Join-Path $PSScriptRoot "Output\$installerName"
if ($buildExit -ne 0 -or -not (Test-Path -LiteralPath $installer)) {
    [IO.File]::WriteAllText($headerPath, $header, $utf8NoBom)   # desfaz a troca de versão
    Fail "A compilação falhou; nada foi publicado."
}

# --------------------------------------------------------------------------
Step "Gerando $manifestName"
$hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = Join-Path $PSScriptRoot "Output\$manifestName"
$manifestText = "# nanoAudio update manifest`nversion=$Version`nfile=$installerName`nsha256=$hash`n"
[IO.File]::WriteAllText($manifest, $manifestText, $utf8NoBom)
Write-Host $manifestText

# --------------------------------------------------------------------------
Step "Commit, tag $tag e push"
$releaseFiles = @('nanoAudioVersion.h', 'nanoAudioDriver.dll', 'nanoAudioDriver32.dll',
                  'nanoAudioControlPanel.exe', 'nanoAudioUpdater.exe')
Invoke-Native git (@('add', '--') + $releaseFiles)
if (Get-NativeOutput git @('diff', '--cached', '--name-only')) {
    Invoke-Native git @('commit', '-m', "release: $tag")
}
# A tag precisa apontar para os mesmos binários que vão no instalador.
$pending = Get-NativeOutput git (@('status', '--porcelain', '--') + $releaseFiles)
if ($pending) { Fail "Os arquivos compilados não ficaram commitados (nada foi publicado):`n$pending" }
Invoke-Native git @('tag', '-a', $tag, '-m', "nanoAudio $Version")
Invoke-Native git @('push', 'origin', $branch)
Invoke-Native git @('push', 'origin', $tag)

# --------------------------------------------------------------------------
Step "Criando release $tag (rascunho)"
$createArgs = @('release', 'create', $tag, $installer, $manifest, '-R', $repo, '--draft', '--verify-tag',
                '--title', "nanoAudio $Version")
if ($NotesFile)  { $createArgs += @('--notes-file', $NotesFile) }
elseif ($Notes)  { $createArgs += @('--notes', $Notes) }
else             { $createArgs += '--generate-notes' }
Invoke-Native gh $createArgs

Step "Conferindo os arquivos enviados"
$check = Join-Path ([IO.Path]::GetTempPath()) ("nanoAudio-release-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $check | Out-Null
try {
    Invoke-Native gh @('release', 'download', $tag, '-R', $repo, '-D', $check)
    $upInstaller = Join-Path $check $installerName
    $upManifest  = Join-Path $check $manifestName
    if (-not (Test-Path $upInstaller) -or -not (Test-Path $upManifest)) { Fail "Arquivos ausentes na release (rascunho mantido)." }
    if ((Get-FileHash $upInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) { Fail "SHA-256 do instalador enviado não confere (rascunho mantido)." }
    if ([IO.File]::ReadAllText($upManifest) -ne $manifestText) { Fail "Manifesto enviado difere do local (rascunho mantido)." }
} finally {
    Remove-Item -LiteralPath $check -Recurse -Force -ErrorAction SilentlyContinue
}

Step "Publicando release $tag"
Invoke-Native gh @('release', 'edit', $tag, '-R', $repo, '--draft=false', '--latest')

# --------------------------------------------------------------------------
Step "Confirmando o link público usado pelo atualizador"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$url = "https://github.com/$repo/releases/latest/download/$manifestName"
$ok = $false
for ($i = 0; $i -lt 10 -and -not $ok; $i++) {
    try {
        $resp = Invoke-WebRequest -Uri $url -UseBasicParsing -Headers @{ 'Cache-Control' = 'no-cache' }
        $body = if ($resp.Content -is [byte[]]) { [Text.Encoding]::UTF8.GetString($resp.Content) } else { [string]$resp.Content }
        $ok = $body -match "(?m)^version=$([regex]::Escape($Version))\s*$"
    } catch { }
    if (-not $ok) { Start-Sleep -Seconds 3 }
}
if (-not $ok) {
    Write-Host "[AVISO] A release foi publicada, mas $url ainda não retornou a versão $Version. Confira em alguns minutos." -ForegroundColor Yellow
} else {
    Write-Host "OK: $url -> $Version"
}

Write-Host "`nRelease publicada: https://github.com/$repo/releases/tag/$tag" -ForegroundColor Green
Write-Host "As instalações com atualização automática vão receber a $Version na próxima verificação (ao ligar o PC ou às 12:00)."
exit 0
