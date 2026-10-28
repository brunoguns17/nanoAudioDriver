@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
title Desinstalador — nanoAudio ASIO Driver

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Solicitando permissoes de Administrador ^(UAC^) para remover o driver ASIO...
    powershell -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
    exit /b
)

:: Remove versoes antigas (UniversalWasapiAsio) e atuais (nanoAudio)
for %%D in ("%ProgramFiles%\UniversalWasapiAsio" "%ProgramFiles%\nanoAudio") do (
    for %%F in ("UniversalWasapiAsioDriver.dll" "UniversalWasapiAsioDriver32.dll" "nanoAudioDriver.dll" "nanoAudioDriver32.dll") do (
        if exist "%%~D\%%~F" (
            regsvr32 /u /s "%%~D\%%~F"
            del /F /Q "%%~D\%%~F"
        )
    )
    for %%E in ("UniversalWasapiAsioControlPanel.exe" "nanoAudioControlPanel.exe") do (
        if exist "%%~D\%%~E" del /F /Q "%%~D\%%~E"
    )
    if exist "%%~D" rmdir /S /Q "%%~D" 2>nul
)

reg delete "HKLM\SOFTWARE\ASIO\Universal WASAPI Low-Latency ASIO" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\WOW6432Node\ASIO\Universal WASAPI Low-Latency ASIO" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\ASIO\Universal WASAPI Low-Latency ASIO" /f >nul 2>&1
reg delete "HKCU\Software\UniversalWasapiAsio" /f >nul 2>&1
reg delete "HKCU\SOFTWARE\Classes\CLSID\{8E6A2A41-5C3B-4F92-9A1D-7E3F9B02C4D8}" /f >nul 2>&1

echo.
echo ============================================================================
echo   [SUCESSO] Versoes antigas e registros legados removidos com sucesso!
echo ============================================================================
echo.
pause
exit /b 0
