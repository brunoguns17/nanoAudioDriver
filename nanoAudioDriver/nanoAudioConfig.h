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
#include <algorithm>
#include <cmath>
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
    bool    ignoreWindowsVolume   = true;  // compensa o volume (por software) do Windows no modo compartilhado
    bool    enableInputCapture    = true;  // true = Full Duplex (Entrada + Saída)
    long    customBufferFrames    = 0;     // 0 = Automático, ou 64, 128, 256, 512, 1024, 2048
    long    preferredSampleRate   = 0;     // 0 = Automático (taxa do Windows), ou 44100, 48000, 88200, 96000, 192000
    long    bitDepth              = 32;    // 16, 24, 32 (Float)
    long    numChannelsOut        = 2;     // 2 (Estéreo), 4, 6, 8
    long    numChannelsIn         = 2;     // 1 (Mono), 2 (Estéreo)
};

struct AudioDeviceEntry {
    std::wstring id;
    std::wstring name;
};

// Fila de saída mantida no modo compartilhado: o que o Windows consome de uma vez (a rajada: um
// período ou mais, conforme o dispositivo) mais a folga para a DAW calcular os próximos blocos.
inline long SharedRenderTarget(long blockFrames, long burstFrames, long periodFrames) {
    return burstFrames + std::max(blockFrames * 2, periodFrames / 2);
}

struct LatencyEstimate {
    long inputFrames  = 0;
    long outputFrames = 0;
};

// Latência prevista com o período padrão de 10 ms do Windows. Com o áudio rodando o driver mede
// a latência real e passa a reportá-la à DAW.
inline LatencyEstimate EstimateLatency(long blockFrames, double sampleRate, bool exclusive) {
    const long period = static_cast<long>(sampleRate * 0.010 + 0.5);
    LatencyEstimate e;
    e.inputFrames  = period + static_cast<long>(sampleRate * 0.004 + 0.5);
    e.outputFrames = exclusive ? blockFrames * 2 + period : SharedRenderTarget(blockFrames, period, period) + period;
    return e;
}

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
    long autoBufferFrames = 256
);
