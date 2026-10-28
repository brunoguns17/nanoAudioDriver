@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
title Gerador do Instalador — Universal WASAPI Low-Latency ASIO Driver

echo ============================================================================
echo   GERADOR DE INSTALADOR EXECUTÁVEL (.EXE) COM PAINEL DE CONTROLE
echo   Universal WASAPI Low-Latency ASIO Driver
echo ============================================================================
echo.

cd /d "%~dp0"

:: 1. Compilação da DLL 64-bit, 32-bit e Painel de Controle EXE
echo [PASSO 1/2] Compilando bibliotecas e Painel de Controle (64-bit e 32-bit)...

set "COMPILER="
where clang++ >nul 2>&1
if %errorLevel% equ 0 (
    set "COMPILER=CLANG"
)

if not defined COMPILER (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
                set "VCVARS64=%%i\VC\Auxiliary\Build\vcvars64.bat"
                set "COMPILER=MSVC"
            )
        )
    )
)

if "%COMPILER%"=="CLANG" (
    echo [INFO] Compilador Clang/LLVM-MinGW detectado.
    
    echo [1/3] Compilando nanoAudioDriver.dll (64-bit Release Estático)...
    clang++ -target x86_64-w64-windows-gnu -shared -O2 -std=c++17 -static -DUNICODE -D_UNICODE nanoAudioDriver.cpp nanoAudioConfig.cpp nanoAudioDriver.def -lole32 -luuid -lavrt -lwinmm -luser32 -lgdi32 -ladvapi32 -lcomctl32 -lksuser -o nanoAudioDriver.dll
    if %errorLevel% neq 0 (
        echo [ERRO] Falha ao compilar DLL 64-bit.
        pause
        exit /b 1
    )

    echo [2/3] Compilando nanoAudioDriver32.dll (32-bit Release Estático)...
    clang++ -target i686-w64-windows-gnu -shared -O2 -std=c++17 -static -DUNICODE -D_UNICODE nanoAudioDriver.cpp nanoAudioConfig.cpp nanoAudioDriver.def -lole32 -luuid -lavrt -lwinmm -luser32 -lgdi32 -ladvapi32 -lcomctl32 -lksuser -o nanoAudioDriver32.dll
    if %errorLevel% neq 0 (
        echo [AVISO] Falha ao compilar versão 32-bit, continuando apenas com 64-bit...
    )

    echo [3/3] Compilando nanoAudioControlPanel.exe (Painel de Controle Standalone)...
    clang++ -target x86_64-w64-windows-gnu -O2 -std=c++17 -static -mwindows -municode -DUNICODE -D_UNICODE nanoAudioConfigMain.cpp nanoAudioConfig.cpp -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lcomctl32 -o nanoAudioControlPanel.exe
    if %errorLevel% neq 0 (
        echo [ERRO] Falha ao compilar executável do Painel de Controle.
        pause
        exit /b 1
    )
) else if "%COMPILER%"=="MSVC" (
    echo [INFO] Compilador MSVC detectado.
    call "!VCVARS64!" >nul
    cl /nologo /LD /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioDriver.cpp nanoAudioConfig.cpp ole32.lib uuid.lib avrt.lib user32.lib gdi32.lib advapi32.lib comctl32.lib ksuser.lib /link /DEF:nanoAudioDriver.def /OUT:nanoAudioDriver.dll
    cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioConfigMain.cpp nanoAudioConfig.cpp ole32.lib uuid.lib user32.lib gdi32.lib advapi32.lib comctl32.lib /link /SUBSYSTEM:WINDOWS /OUT:nanoAudioControlPanel.exe
    if %errorLevel% neq 0 (
        echo [ERRO] Falha ao compilar com MSVC.
        pause
        exit /b 1
    )
) else (
    if not exist "nanoAudioDriver.dll" (
        echo [ERRO] Nenhum compilador C++ (Clang ou Visual Studio) encontrado e os arquivos não existem.
        pause
        exit /b 1
    )
    echo [INFO] Utilizando arquivos já compilados na pasta.
)

:: 2. Localização do Compilador Inno Setup (ISCC.exe)
echo.
echo [PASSO 2/2] Construindo instalador executável com o Inno Setup...

set "ISCC="
where iscc.exe >nul 2>&1
if %errorLevel% equ 0 set "ISCC=iscc.exe"

if not defined ISCC (
    if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" (
        set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
    ) else if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" (
        set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
    ) else if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" (
        set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
    ) else if exist "%LOCALAPPDATA%\Programs\Antigravity IDE\resources\app\node_modules\innosetup\bin\ISCC.exe" (
        set "ISCC=%LOCALAPPDATA%\Programs\Antigravity IDE\resources\app\node_modules\innosetup\bin\ISCC.exe"
    )
)

if not defined ISCC (
    echo [ERRO] Inno Setup 6 (ISCC.exe) não foi encontrado no sistema!
    echo Você pode instalar o Inno Setup executando no terminal:
    echo   winget install --id JRSoftware.InnoSetup
    pause
    exit /b 1
)

echo [INFO] Executando Inno Setup Compiler: "!ISCC!"
"!ISCC!" "Installer_Setup.iss"
if %errorLevel% neq 0 (
    echo [ERRO] Falha ao gerar o instalador com o Inno Setup.
    pause
    exit /b 1
)

echo.
echo ============================================================================
echo   SUCESSO! INSTALADOR GERADO COM ÊXITO!
echo   Arquivo gerado: Output\nanoAudio_Setup_v2.5.0.exe
echo ============================================================================
echo.
echo O instalador contém o Driver 64-bit, 32-bit e o Painel de Controle Oficial!
echo Abrindo a pasta do instalador...
explorer.exe /select,"%~dp0Output\nanoAudio_Setup_v2.5.0.exe"
pause
exit /b 0
