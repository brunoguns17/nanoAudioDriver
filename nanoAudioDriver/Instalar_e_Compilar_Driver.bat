@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
title Instalador — nanoAudio ASIO Driver

echo ============================================================================
echo   INSTALADOR: nanoAudio ASIO Driver (Windows 10 / 11)
echo ============================================================================
echo.

:: 1. Verifica permissoes de Administrador e solicita elevacao (UAC) automaticamente
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [INFO] Solicitando permissoes de Administrador ^(UAC^) para registrar o driver ASIO...
    powershell -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"
set "INSTALL_DIR=%ProgramFiles%\nanoAudio"
set "DLL_NAME=nanoAudioDriver.dll"
set "DLL32_NAME=nanoAudioDriver32.dll"
set "PANEL_EXE=nanoAudioControlPanel.exe"

:: 2. Verifica se a DLL ja existe na pasta ou em build\Release; caso contrario, compila automaticamente
if exist "%~dp0%DLL_NAME%" (
    set "BUILT_DLL=%~dp0%DLL_NAME%"
    goto :INSTALL_STEP
)
if exist "%~dp0build\Release\%DLL_NAME%" (
    set "BUILT_DLL=%~dp0build\Release\%DLL_NAME%"
    goto :INSTALL_STEP
)

echo [PASSO 1/3] DLL pre-compilada nao encontrada. Buscando compilador C++...

where clang++ >nul 2>&1
if %errorLevel% equ 0 (
    echo [INFO] Compilador Clang/LLVM-MinGW detectado...
    echo [INFO] Compilando nanoAudioDriver.dll ^(64-bit Release Estatico^)...
    clang++ -target x86_64-w64-windows-gnu -shared -O2 -std=c++17 -static -DUNICODE -D_UNICODE nanoAudioDriver.cpp nanoAudioConfig.cpp nanoAudioDriver.def -lole32 -luuid -lavrt -luser32 -lgdi32 -ladvapi32 -lcomctl32 -lksuser -o "%~dp0%DLL_NAME%"
    clang++ -target i686-w64-windows-gnu -shared -O2 -std=c++17 -static -DUNICODE -D_UNICODE nanoAudioDriver.cpp nanoAudioConfig.cpp nanoAudioDriver.def -lole32 -luuid -lavrt -luser32 -lgdi32 -ladvapi32 -lcomctl32 -lksuser -o "%~dp0%DLL32_NAME%"
    clang++ -target x86_64-w64-windows-gnu -O2 -std=c++17 -static -mwindows -municode -DUNICODE -D_UNICODE nanoAudioConfigMain.cpp nanoAudioConfig.cpp -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lcomctl32 -o "%~dp0%PANEL_EXE%"
    if exist "%~dp0%DLL_NAME%" (
        set "BUILT_DLL=%~dp0%DLL_NAME%"
        goto :INSTALL_STEP
    )
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

if defined VCVARS (
    echo [INFO] Compilador MSVC encontrado: "!VCVARS!"
    call "!VCVARS!" >nul
    echo [INFO] Compilando nanoAudioDriver.dll ^(64-bit Release^)...
    cl /nologo /LD /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioDriver.cpp nanoAudioConfig.cpp ole32.lib uuid.lib avrt.lib user32.lib gdi32.lib advapi32.lib comctl32.lib ksuser.lib /link /DEF:nanoAudioDriver.def /OUT:"%~dp0%DLL_NAME%"
    cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioConfigMain.cpp nanoAudioConfig.cpp ole32.lib uuid.lib user32.lib gdi32.lib advapi32.lib comctl32.lib /link /SUBSYSTEM:WINDOWS /OUT:"%~dp0%PANEL_EXE%"
    if exist "%~dp0%DLL_NAME%" (
        set "BUILT_DLL=%~dp0%DLL_NAME%"
        goto :INSTALL_STEP
    )
)

echo.
echo [AVISO] Nenhum compilador C++ (Clang ou Visual Studio) encontrado.
echo.
pause
exit /b 1

:INSTALL_STEP
echo.
echo [PASSO 2/3] Copiando arquivos do driver para "%INSTALL_DIR%"...
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
copy /Y "%BUILT_DLL%" "%INSTALL_DIR%\%DLL_NAME%" >nul
if %errorLevel% neq 0 (
    echo [ERRO] Nao foi possivel copiar a DLL ^(verifique se alguma DAW esta aberta usando o driver^).
    pause
    exit /b 1
)

if exist "%~dp0%DLL32_NAME%" (
    copy /Y "%~dp0%DLL32_NAME%" "%INSTALL_DIR%\%DLL32_NAME%" >nul
)

if exist "%~dp0%PANEL_EXE%" (
    copy /Y "%~dp0%PANEL_EXE%" "%INSTALL_DIR%\%PANEL_EXE%" >nul
)

echo [PASSO 3/3] Registrando servidor COM e entrada ASIO no Windows ^(regsvr32^)...
regsvr32 /s "%INSTALL_DIR%\%DLL_NAME%"
if %errorLevel% neq 0 (
    echo [ERRO] Falha ao registrar a DLL 64-bit via regsvr32.
    pause
    exit /b 1
)

if exist "%INSTALL_DIR%\%DLL32_NAME%" (
    regsvr32 /s "%INSTALL_DIR%\%DLL32_NAME%"
)

echo.
echo ============================================================================
echo   SUCESSO! Driver e Painel de Controle instalados com sucesso!
echo   Nome na DAW: nanoAudio
echo   Localizacao: %INSTALL_DIR%\%DLL_NAME%
echo ============================================================================
echo.
echo Agora abra sua DAW ^(Reaper, FL Studio, Ableton Live, Cubase, Studio One^)
echo e clique em "Control Panel" / "Painel do Driver" para configurar dispositivos,
echo sample rate, resolucao, canais e latencia!
echo.
pause
exit /b 0
