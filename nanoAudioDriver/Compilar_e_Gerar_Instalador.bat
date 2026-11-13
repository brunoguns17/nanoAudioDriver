@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
title Gerador do Instalador — nanoAudio ASIO Driver

:: Quando chamado pelo Publicar_Release.ps1, NANO_RELEASE=1: sem pausas, sem abrir
:: o Explorer, e qualquer falha de compilação interrompe o processo (nunca usa
:: binários antigos).

echo ============================================================================
echo   GERADOR DE INSTALADOR EXECUTÁVEL (.EXE) COM PAINEL DE CONTROLE
echo   nanoAudio ASIO Driver
echo ============================================================================
echo.

cd /d "%~dp0"

:: Versão: fonte única em nanoAudioVersion.h
set "VER="
for /f "tokens=3" %%v in ('findstr /c:"#define NANOAUDIO_VERSION_STR " nanoAudioVersion.h') do set "VER=%%~v"
if not defined VER (
    echo [ERRO] Não foi possível ler a versão em nanoAudioVersion.h
    goto :FAIL
)
echo [INFO] Versão: %VER%
echo.

:: 1. Compilação da DLL 64-bit, 32-bit, Painel de Controle e Atualizador
echo [PASSO 1/2] Compilando bibliotecas, Painel de Controle e Atualizador...

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

set "CFLAGS=-O2 -std=c++17 -static -DUNICODE -D_UNICODE"
set "DRVLIBS=-lole32 -luuid -lavrt -lwinmm -luser32 -lgdi32 -ladvapi32 -lcomctl32 -lksuser"
set "UPDLIBS=-lwinhttp -lbcrypt -lrstrtmgr -lshell32 -luser32 -ladvapi32"

if "%COMPILER%"=="CLANG" (
    echo [INFO] Compilador Clang/LLVM-MinGW detectado.

    echo [1/4] Compilando nanoAudioDriver.dll ^(64-bit^)...
    clang++ -target x86_64-w64-windows-gnu -shared %CFLAGS% nanoAudioDriver.cpp nanoAudioConfig.cpp nanoAudioDriver.def %DRVLIBS% -o nanoAudioDriver.dll
    if !errorLevel! neq 0 ( echo [ERRO] Falha ao compilar DLL 64-bit. & goto :FAIL )

    echo [2/4] Compilando nanoAudioDriver32.dll ^(32-bit^)...
    clang++ -target i686-w64-windows-gnu -shared %CFLAGS% nanoAudioDriver.cpp nanoAudioConfig.cpp nanoAudioDriver.def %DRVLIBS% -o nanoAudioDriver32.dll
    if !errorLevel! neq 0 ( echo [ERRO] Falha ao compilar DLL 32-bit. & goto :FAIL )

    echo [3/4] Compilando nanoAudioControlPanel.exe...
    clang++ -target x86_64-w64-windows-gnu %CFLAGS% -mwindows -municode nanoAudioConfigMain.cpp nanoAudioConfig.cpp -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lcomctl32 -o nanoAudioControlPanel.exe
    if !errorLevel! neq 0 ( echo [ERRO] Falha ao compilar o Painel de Controle. & goto :FAIL )

    echo [4/4] Compilando nanoAudioUpdater.exe...
    clang++ -target x86_64-w64-windows-gnu %CFLAGS% -mwindows -municode nanoAudioUpdater.cpp %UPDLIBS% -o nanoAudioUpdater.exe
    if !errorLevel! neq 0 ( echo [ERRO] Falha ao compilar o Atualizador. & goto :FAIL )
) else if "%COMPILER%"=="MSVC" (
    echo [INFO] Compilador MSVC detectado.
    if defined NANO_RELEASE (
        echo [ERRO] A build MSVC não gera a DLL 32-bit; use o Clang/LLVM-MinGW para publicar releases.
        goto :FAIL
    )
    call "!VCVARS64!" >nul
    cl /nologo /LD /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioDriver.cpp nanoAudioConfig.cpp ole32.lib uuid.lib avrt.lib winmm.lib user32.lib gdi32.lib advapi32.lib comctl32.lib ksuser.lib /link /DEF:nanoAudioDriver.def /OUT:nanoAudioDriver.dll
    if !errorLevel! neq 0 ( echo [ERRO] Falha ao compilar DLL com MSVC. & goto :FAIL )
    cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioConfigMain.cpp nanoAudioConfig.cpp ole32.lib uuid.lib user32.lib gdi32.lib advapi32.lib comctl32.lib /link /SUBSYSTEM:WINDOWS /OUT:nanoAudioControlPanel.exe
    if !errorLevel! neq 0 ( echo [ERRO] Falha ao compilar o Painel com MSVC. & goto :FAIL )
    cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioUpdater.cpp winhttp.lib bcrypt.lib rstrtmgr.lib shell32.lib user32.lib advapi32.lib /link /SUBSYSTEM:WINDOWS /OUT:nanoAudioUpdater.exe
    if !errorLevel! neq 0 ( echo [ERRO] Falha ao compilar o Atualizador com MSVC. & goto :FAIL )
) else (
    if defined NANO_RELEASE (
        echo [ERRO] Nenhum compilador C++ encontrado. Uma release nunca usa binários antigos.
        goto :FAIL
    )
    if not exist "nanoAudioDriver.dll" (
        echo [ERRO] Nenhum compilador C++ ^(Clang ou Visual Studio^) encontrado e os arquivos não existem.
        goto :FAIL
    )
    echo [AVISO] Nenhum compilador encontrado: utilizando arquivos já compilados na pasta.
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
    echo [ERRO] Inno Setup 6 ^(ISCC.exe^) não foi encontrado no sistema!
    echo Você pode instalar o Inno Setup executando no terminal:
    echo   winget install --id JRSoftware.InnoSetup
    goto :FAIL
)

echo [INFO] Executando Inno Setup Compiler: "!ISCC!"
"!ISCC!" /Q "/DAppVersion=%VER%" "Installer_Setup.iss"
if %errorLevel% neq 0 (
    echo [ERRO] Falha ao gerar o instalador com o Inno Setup.
    goto :FAIL
)

echo.
echo ============================================================================
echo   SUCESSO: instalador gerado com êxito.
echo   Arquivo gerado: Output\nanoAudio_Setup_v%VER%.exe
echo ============================================================================
echo.
if defined NANO_RELEASE exit /b 0
echo O instalador contém o Driver 64-bit, 32-bit, o Painel de Controle e o Atualizador.
echo Abrindo a pasta do instalador...
explorer.exe /select,"%~dp0Output\nanoAudio_Setup_v%VER%.exe"
pause
exit /b 0

:FAIL
if not defined NANO_RELEASE pause
exit /b 1
