// ============================================================================
// nanoAudioConfig.h
// Configuração do Usuário e Painel de Controle Gráfico Win32
// Universal WASAPI Low-Latency ASIO Driver
// ============================================================================

#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>

static const wchar_t* kUserSettingsKey       = L"Software\\nanoAudio";
static const wchar_t* kLegacyUserSettingsKey = L"Software\\UniversalWasapiAsio";

struct DriverUserConfig {
    wchar_t outputDeviceId[256]   = {};  // ID do endpoint WASAPI (vazio = padrão do Windows)
    wchar_t outputDeviceName[128] = {};  // Nome amigável para exibição
    wchar_t inputDeviceId[256]    = {};  // ID do endpoint WASAPI de entrada
    wchar_t inputDeviceName[128]  = {};  // Nome amigável de entrada
    bool    useExclusiveMode      = false; // false = Shared (multi-app), true = Exclusive
    bool    enableInputCapture    = true;  // true = Full Duplex (Entrada + Saída)
    long    customBufferFrames    = 0;     // 0 = Automático, ou 64, 128, 256, 512, 1024, 2048
    long    preferredSampleRate   = 48000; // 44100, 48000, 88200, 96000, 192000
    long    bitDepth              = 32;    // 16, 24, 32 (Float)
    long    numChannelsOut        = 2;     // 2 (Estéreo), 4, 6, 8
    long    numChannelsIn         = 2;     // 1 (Mono), 2 (Estéreo)
};

struct AudioDeviceEntry {
    std::wstring id;
    std::wstring name;
};

// Funções utilitárias
std::vector<AudioDeviceEntry> EnumerateAudioDevices(EDataFlow flow);
DriverUserConfig LoadUserConfig();
void SaveUserConfig(const DriverUserConfig& cfg);

// Exibe a janela gráfica do Painel de Controle (modal à DAW se hWndParent != nullptr)
// Retorna true se o usuário clicou em "Salvar e Aplicar", false se cancelou.
bool ShowControlPanelWindow(
    HWND hWndParent,
    DriverUserConfig* inOutConfig,
    bool inDawMode,
    double nativeMixRate = 48000.0,
    long hwMinFrames = 64,
    long hwDefFrames = 256,
    long hwMaxFrames = 2048
);

