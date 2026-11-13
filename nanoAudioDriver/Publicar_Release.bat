@echo off
chcp 65001 >nul
title Publicar Release — nanoAudio ASIO Driver
cd /d "%~dp0"

echo ============================================================================
echo   PUBLICAR NOVA RELEASE NO GITHUB (com atualização automática)
echo ============================================================================
echo.
for /f "tokens=3" %%v in ('findstr /c:"#define NANOAUDIO_VERSION_STR " nanoAudioVersion.h') do echo Versão atual: %%~v
echo.
set "VER="
set /p "VER=Nova versão (ex.: 2.6.1): "
if not defined VER (
    echo Nenhuma versão informada.
    pause
    exit /b 1
)
set "NOTES="
set /p "NOTES=Notas da release (Enter = geradas automaticamente a partir dos commits): "

if defined NOTES (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Publicar_Release.ps1" -Version "%VER%" -Notes "%NOTES%"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Publicar_Release.ps1" -Version "%VER%"
)
echo.
pause
