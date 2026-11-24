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
    bool measured     = false;   // inclui o que o driver mediu nesses dispositivos
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

// Comportamento aprendido por dispositivo (em microssegundos), no Registro. Evita repetir a cada
// abertura da DAW o estalo que ensinou que aquele driver precisa de mais folga, e permite informar a
// latência certa já no createBuffers(), antes de o áudio rodar. Os limites impedem que um travamento
// isolado do PC deixe a latência alta para sempre. A rajada vai até 30 ms (3 períodos do mixer): a
// Behringer USB chega a precisar de ~26 ms.
struct DeviceTuning { const wchar_t* shared; const wchar_t* exclusive; LONG minUs, maxUs; };
inline const DeviceTuning kTuneBurst    = { L"SharedBurstUs",    L"ExclusiveBurstUs",         0,  30000 };
inline const DeviceTuning kTuneMargin   = { L"SharedMarginUs",   L"ExclusiveMarginUs",        0,  12000 };
inline const DeviceTuning kTuneOutExtra = { L"SharedOutExtraUs", L"ExclusiveOutExtraUs", -40000, 200000 };
inline const DeviceTuning kTuneInExtra  = { L"SharedInExtraUs",  L"ExclusiveInExtraUs",  -40000, 200000 };

long LoadDeviceTuning(const std::wstring& id, const DeviceTuning& t, bool exclusive, double sampleRate, bool* found = nullptr);
void SaveDeviceTuning(const std::wstring& id, const DeviceTuning& t, bool exclusive, double frames, double sampleRate);

// Latência que o driver informa à DAW (e que o painel mostra): a prevista mais a medida nos dispositivos.
LatencyEstimate PredictLatency(long blockFrames, double sampleRate, bool exclusive,
                               const std::wstring& renderId, const std::wstring& captureId);

// Dispositivo que o driver vai usar: o escolhido, se estiver ativo; senão (entrada) uma mesa/interface
// USB; senão o padrão do Windows. outName recebe o nome amigável.
typedef void (*StaleEndpointFn)(const wchar_t* name, DWORD state);
IMMDevice* ResolveAudioEndpoint(IMMDeviceEnumerator* pEnum, EDataFlow dataFlow, const wchar_t* preferredId,
                                wchar_t* outName, size_t maxNameLen, StaleEndpointFn onStale = nullptr);
std::wstring EndpointId(IMMDevice* dev);

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
