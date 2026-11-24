// ============================================================================
// nanoAudioDriver.cpp — nanoAudio ASIO Driver (versão em nanoAudioVersion.h)
// Implementação do driver ASIO universal nanoAudio (Windows 10/11) sobre WASAPI
//
// - <initguid.h> vem antes das headers de dispositivo para que PKEY_Device_FriendlyName
//   seja definido em qualquer compilador.
// - Saída: modo compartilhado (com compensação do volume do Windows, veja
//   nanoAudioVolume.h) ou exclusivo (direto no hardware).
// - Entrada: FIFO com controle automático de nível (nanoAudioCaptureFifo.h).
// - O painel de controle fica em nanoAudioConfig.cpp.
// ============================================================================

#include <initguid.h>
#include "nanoAudioDriver.h"
#include "nanoAudioCaptureFifo.h"
#include "nanoAudioVersion.h"
#include "nanoAudioVolume.h"
#include <audiopolicy.h>
#include <cwctype>
#include <cstdarg>
#include <cwchar>
#include <new>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// Exportações padrão do servidor COM In-Process
#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/EXPORT:DllRegisterServer,PRIVATE")
#pragma comment(linker, "/EXPORT:DllUnregisterServer,PRIVATE")

static HMODULE g_hModule = nullptr;
static std::atomic<long> g_dllRefCount{0};

// Log de diagnóstico em %TEMP%\nanoAudio_log.txt (chamado só no start/stop e em eventos raros,
// nunca a cada bloco de áudio). Ajuda a descobrir por que a entrada não chega ao Reaper.
static void NanoLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 32) return;
    std::wcsncat(path, L"nanoAudio_log.txt", MAX_PATH - n - 1);

    // Mantém o arquivo pequeno (recomeça ao passar de 512 KB)
    const wchar_t* mode = L"a, ccs=UTF-8";
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 512 * 1024)) {
        mode = L"w, ccs=UTF-8";
    }
    FILE* f = _wfopen(path, mode);
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fwprintf(f, L"[%02d/%02d %02d:%02d:%02d.%03d] ", st.wDay, st.wMonth, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    std::vfwprintf(f, fmt, ap);
    va_end(ap);
    std::fwprintf(f, L"\n");
    std::fclose(f);
}

static bool ContainsIgnoreCase(const wchar_t* haystack, const wchar_t* needle) {
    if (!haystack || !needle || !*needle) return false;
    for (; *haystack; ++haystack) {
        const wchar_t* h = haystack;
        const wchar_t* n = needle;
        while (*h && *n && std::towlower(*h) == std::towlower(*n)) {
            ++h;
            ++n;
        }
        if (!*n) return true;
    }
    return false;
}

// ============================================================================
// 1. IMPLEMENTAÇÃO DOS MÉTODOS DA INTERFACE IASIO (NANOAUDIO)
// ============================================================================

UniversalWasapiAsioDriver::UniversalWasapiAsioDriver() : m_refCount(1) {
    g_dllRefCount.fetch_add(1);
    std::snprintf(m_errorMsg, sizeof(m_errorMsg), "No Error");
    m_config = LoadUserConfig();
}

UniversalWasapiAsioDriver::~UniversalWasapiAsioDriver() {
    disposeBuffers();
    if (m_outputReadyEvent) CloseHandle(m_outputReadyEvent);
    // Só desfaz o CoInitializeEx() do init() se estiver na mesma thread (exigência do COM)
    if (m_comInitialized && m_comThreadId == GetCurrentThreadId()) CoUninitialize();
    g_dllRefCount.fetch_sub(1);
}

STDMETHODIMP UniversalWasapiAsioDriver::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == CLSID_UniversalWasapiAsioDriver) {
        *ppv = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) UniversalWasapiAsioDriver::AddRef() {
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

STDMETHODIMP_(ULONG) UniversalWasapiAsioDriver::Release() {
    ULONG res = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
    if (res == 0) delete this;
    return res;
}

ASIOBool UniversalWasapiAsioDriver::init(void* sysHandle) {
    HWND candidateHwnd = reinterpret_cast<HWND>(sysHandle);
    m_sysHandle = (candidateHwnd && IsWindow(candidateHwnd)) ? candidateHwnd : nullptr;
    m_config = LoadUserConfig();

    if (!m_comInitialized) {
        m_comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        m_comThreadId    = GetCurrentThreadId();
    }

    // Mesmo que não haja dispositivo de entrada ativo no momento, inicializa o driver
    // com valores seguros para nunca impedir a abertura do Painel de Controle no Reaper.
    ProbeHardwareCapabilities();
    m_sampleRate = (m_config.preferredSampleRate > 0) ? static_cast<double>(m_config.preferredSampleRate)
                                                      : m_nativeMixRate;
    UpdateLatencyEstimate(m_config.customBufferFrames > 0 ? m_config.customBufferFrames : kPreferredBufferFrames);

    m_initialized = true;
    return ASIOTrue;
}

void UniversalWasapiAsioDriver::getDriverName(char* name) {
    if (name) std::snprintf(name, 32, "%s", kDriverNameAnsi);
}

long UniversalWasapiAsioDriver::getDriverVersion() {
    // "2.6.2" -> 0x00020602
    long v[3] = {};
    std::sscanf(NANOAUDIO_VERSION_STR, "%ld.%ld.%ld", &v[0], &v[1], &v[2]);
    return (v[0] << 16) | ((v[1] & 0xFF) << 8) | (v[2] & 0xFF);
}

void UniversalWasapiAsioDriver::getErrorMessage(char* string) {
    if (string) std::snprintf(string, 124, "%s", m_errorMsg);
}

ASIOError UniversalWasapiAsioDriver::getChannels(long* numInputChannels, long* numOutputChannels) {
    if (!numInputChannels || !numOutputChannels) return ASE_InvalidParameter;
    *numInputChannels  = ActiveInputCount();
    *numOutputChannels = ActiveOutputCount();
    return ASE_OK;
}

long UniversalWasapiAsioDriver::ActiveInputCount() const {
    if (!m_config.enableInputCapture) return 0;
    return (m_config.numChannelsIn > 0) ? std::min(m_config.numChannelsIn, m_numInputs) : m_numInputs;
}

// Nunca anuncia mais canais do que o dispositivo tem: o excedente não sairia em lugar nenhum.
long UniversalWasapiAsioDriver::ActiveOutputCount() const {
    return (m_config.numChannelsOut > 0) ? std::min(m_config.numChannelsOut, m_numOutputs) : m_numOutputs;
}

ASIOError UniversalWasapiAsioDriver::getLatencies(long* inputLatency, long* outputLatency) {
    if (!inputLatency || !outputLatency) return ASE_InvalidParameter;
    *inputLatency  = m_inputLatencyFrames.load();
    *outputLatency = m_outputLatencyFrames.load();
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    if (!minSize || !maxSize || !preferredSize || !granularity) return ASE_InvalidParameter;

    const long customBuf = m_config.customBufferFrames;
    *minSize       = kMinBufferFrames;
    *maxSize       = kMaxBufferFrames;
    *preferredSize = (customBuf >= kMinBufferFrames && customBuf <= kMaxBufferFrames) ? customBuf : kPreferredBufferFrames;
    *granularity   = kBufferGranularity;
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::canSampleRate(ASIOSampleRate sampleRate) {
    const double supportedRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    for (double r : supportedRates) {
        if (std::fabs(sampleRate - r) < 1.0) return ASE_OK;
    }
    return ASE_NoClock;
}

ASIOError UniversalWasapiAsioDriver::getSampleRate(ASIOSampleRate* sampleRate) {
    if (!sampleRate) return ASE_InvalidParameter;
    *sampleRate = m_sampleRate;
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::setSampleRate(ASIOSampleRate sampleRate) {
    if (canSampleRate(sampleRate) != ASE_OK) return ASE_NoClock;
    if (std::fabs(m_sampleRate.load() - sampleRate) < 1.0) return ASE_OK;

    // Com o áudio rodando, os streams do WASAPI precisam ser reabertos na nova taxa
    const bool restart = m_running.load() && GetCurrentThreadId() != m_audioThreadId;
    if (restart) stop();
    m_sampleRate = sampleRate;
    UpdateLatencyEstimate(m_activeBufferFrames);
    return restart ? start() : ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::getClockSources(ASIOClockSource* clocks, long* numSources) {
    if (!clocks || !numSources || *numSources < 1) return ASE_InvalidParameter;
    clocks[0].index             = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup   = -1;
    clocks[0].isCurrentSource   = ASIOTrue;
    std::snprintf(clocks[0].name, sizeof(clocks[0].name), "nanoAudio Internal Clock");
    *numSources = 1;
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::setClockSource(long reference) {
    return (reference == 0) ? ASE_OK : ASE_InvalidParameter;
}

ASIOError UniversalWasapiAsioDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    if (!sPos || !tStamp) return ASE_InvalidParameter;
    uint64_t pos = m_samplePosition.load(std::memory_order_relaxed);
    uint64_t ns  = m_systemTimeNs.load(std::memory_order_relaxed);

    sPos->hi   = static_cast<unsigned long>(pos >> 32);
    sPos->lo   = static_cast<unsigned long>(pos & 0xFFFFFFFFULL);
    tStamp->hi = static_cast<unsigned long>(ns >> 32);
    tStamp->lo = static_cast<unsigned long>(ns & 0xFFFFFFFFULL);
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::getChannelInfo(ASIOChannelInfo* info) {
    if (!info) return ASE_InvalidParameter;
    const long maxCh = info->isInput ? ActiveInputCount() : ActiveOutputCount();
    if (info->channel < 0 || info->channel >= maxCh) return ASE_InvalidParameter;

    info->isActive = ASIOFalse;
    for (const ASIOBufferInfo& bi : m_activeChannels) {
        if (!!bi.isInput == !!info->isInput && bi.channelNum == info->channel) info->isActive = ASIOTrue;
    }
    info->channelGroup = 0;
    info->type         = ASIOSTFloat32LSB;
    std::snprintf(
        info->name,
        sizeof(info->name),
        "%s %ld",
        info->isInput ? "nanoAudio In" : "nanoAudio Out",
        info->channel + 1
    );
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::createBuffers(
    ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    if (!bufferInfos || !callbacks || numChannels <= 0) return ASE_InvalidParameter;
    if (!callbacks->bufferSwitch && !callbacks->bufferSwitchTimeInfo) return ASE_InvalidParameter;
    if (bufferSize < 16 || bufferSize > 8192) return ASE_InvalidMode;
    for (long i = 0; i < numChannels; ++i) {
        const long maxCh = bufferInfos[i].isInput ? ActiveInputCount() : ActiveOutputCount();
        if (bufferInfos[i].channelNum < 0 || bufferInfos[i].channelNum >= maxCh) return ASE_InvalidParameter;
    }
    const ASIOError disposed = disposeBuffers();
    if (disposed != ASE_OK) return disposed;

    m_activeBufferFrames = bufferSize;
    m_callbacks          = *callbacks;
    m_activeChannels.resize(numChannels);
    m_channelMemory.resize(static_cast<size_t>(numChannels) * 2);

    for (long i = 0; i < numChannels; ++i) {
        m_channelMemory[i * 2].assign(bufferSize, 0.0f);
        m_channelMemory[i * 2 + 1].assign(bufferSize, 0.0f);

        bufferInfos[i].buffers[0] = m_channelMemory[i * 2].data();
        bufferInfos[i].buffers[1] = m_channelMemory[i * 2 + 1].data();
        m_activeChannels[i]       = bufferInfos[i];
    }

    // O ASIO exige perguntar ao host antes de usar bufferSwitchTimeInfo
    m_hostUsesTimeInfo = m_callbacks.bufferSwitchTimeInfo &&
                         (!m_callbacks.bufferSwitch ||
                          (m_callbacks.asioMessage && m_callbacks.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1));

    UpdateLatencyEstimate(bufferSize);
    m_buffersCreated = true;

    long nIn = 0, nOut = 0;
    for (long i = 0; i < numChannels; ++i) (bufferInfos[i].isInput ? nIn : nOut)++;
    NanoLog(L"createBuffers(): bloco=%ld | canais de ENTRADA ativados pela DAW=%ld | saida=%ld | taxa=%.0f Hz",
            bufferSize, nIn, nOut, m_sampleRate.load());
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::disposeBuffers() {
    // De dentro do bufferSwitch não dá para esperar a própria thread terminar
    if (m_audioThread && GetCurrentThreadId() == m_audioThreadId) return ASE_InvalidMode;
    stop();
    m_activeChannels.clear();
    m_channelMemory.clear();
    m_callbacks      = {};
    m_buffersCreated = false;
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::start() {
    if (!m_buffersCreated || m_running.load()) return ASE_InvalidMode;
    if (m_audioThread && GetCurrentThreadId() == m_audioThreadId) return ASE_InvalidMode;
    stop(); // recolhe uma thread encerrada por stop() chamado de dentro do bufferSwitch

    m_stopEvent   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_outputReadyEvent) m_outputReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_stopEvent || !m_renderEvent || !m_outputReadyEvent) {
        if (m_stopEvent)   { CloseHandle(m_stopEvent);   m_stopEvent   = nullptr; }
        if (m_renderEvent) { CloseHandle(m_renderEvent); m_renderEvent = nullptr; }
        SetError("nanoAudio: Falha ao criar eventos de sincronizacao.");
        return ASE_HWMalfunction;
    }

    m_samplePosition.store(0);
    m_hostUsesOutputReady.store(false);
    m_streamConfig = m_config;
    m_running.store(true);

    m_audioThread = CreateThread(nullptr, 0, &UniversalWasapiAsioDriver::AudioThreadProc, this, 0, &m_audioThreadId);
    if (!m_audioThread) {
        m_running.store(false);
        CloseHandle(m_stopEvent);   m_stopEvent   = nullptr;
        CloseHandle(m_renderEvent); m_renderEvent = nullptr;
        SetError("nanoAudio: Falha ao iniciar thread Pro Audio.");
        return ASE_HWMalfunction;
    }

    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::stop() {
    m_running.store(false);
    if (!m_audioThread) return ASE_OK;
    if (m_stopEvent) SetEvent(m_stopEvent);
    // Chamado de dentro do bufferSwitch: a thread sai sozinha e é recolhida depois
    if (GetCurrentThreadId() == m_audioThreadId) return ASE_OK;

    // Espera a thread terminar DE FATO: liberar os buffers com ela viva derrubaria a DAW.
    // Enquanto espera, atende mensagens enviadas por ela (um SendMessage do bufferSwitch
    // para a janela da DAW travaria os dois lados).
    while (MsgWaitForMultipleObjectsEx(1, &m_audioThread, INFINITE, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    CloseHandle(m_audioThread);
    m_audioThread   = nullptr;
    m_audioThreadId = 0;
    if (m_stopEvent)   { CloseHandle(m_stopEvent);   m_stopEvent   = nullptr; }
    if (m_renderEvent) { CloseHandle(m_renderEvent); m_renderEvent = nullptr; }
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::controlPanel() {
    ShowNativeControlPanelDialog();
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::future(long /*selector*/, void* /*opt*/) {
    return ASE_InvalidParameter;
}

// Hosts que processam fora do bufferSwitch avisam por aqui que o bloco de saída está pronto
ASIOError UniversalWasapiAsioDriver::outputReady() {
    if (m_running.load(std::memory_order_relaxed) && m_outputReadyEvent) {
        m_hostUsesOutputReady.store(true, std::memory_order_relaxed);
        SetEvent(m_outputReadyEvent);
    }
    return ASE_OK;
}

bool UniversalWasapiAsioDriver::HostSupports(long selector) const {
    return m_callbacks.asioMessage && m_callbacks.asioMessage(kAsioSelectorSupported, selector, nullptr, nullptr) == 1;
}

void UniversalWasapiAsioDriver::RequestHostReset() {
    if (HostSupports(kAsioResetRequest)) m_callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
}

// Comportamento aprendido por dispositivo (em microssegundos), no Registro. Evita repetir a cada
// abertura da DAW o estalo que ensinou que aquele driver precisa de mais folga, e permite informar a
// latência certa já no createBuffers(), antes de o áudio rodar. Os limites impedem que um travamento
// isolado do PC deixe a latência alta para sempre.
struct DeviceTuning { const wchar_t* shared; const wchar_t* exclusive; LONG minUs, maxUs; };
static const DeviceTuning kTuneBurst    = { L"SharedBurstUs",    L"ExclusiveBurstUs",         0,  21000 };
static const DeviceTuning kTuneMargin   = { L"SharedMarginUs",   L"ExclusiveMarginUs",        0,  12000 };
static const DeviceTuning kTuneOutExtra = { L"SharedOutExtraUs", L"ExclusiveOutExtraUs", -40000, 200000 };
static const DeviceTuning kTuneInExtra  = { L"SharedInExtraUs",  L"ExclusiveInExtraUs",  -40000, 200000 };

static std::wstring EndpointId(IMMDevice* dev) {
    std::wstring id;
    LPWSTR p = nullptr;
    if (dev && SUCCEEDED(dev->GetId(&p)) && p) {
        id = p;
        CoTaskMemFree(p);
    }
    return id;
}

static std::wstring DeviceTuningKey(const std::wstring& id) {
    return std::wstring(kUserSettingsKey) + L"\\Devices\\" + id;
}

// Retorna em frames (0 = nada aprendido)
static long LoadDeviceTuning(const std::wstring& id, const DeviceTuning& t, bool exclusive, double sampleRate) {
    DWORD value = 0, size = sizeof(value);
    if (id.empty() || RegGetValueW(HKEY_CURRENT_USER, DeviceTuningKey(id).c_str(), exclusive ? t.exclusive : t.shared,
                                   RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return 0;
    const LONG us = std::clamp(static_cast<LONG>(value), t.minUs, t.maxUs);
    return static_cast<long>(us * sampleRate / 1e6);
}

static void SaveDeviceTuning(const std::wstring& id, const DeviceTuning& t, bool exclusive, double frames, double sampleRate) {
    HKEY h = nullptr;
    if (id.empty() || RegCreateKeyExW(HKEY_CURRENT_USER, DeviceTuningKey(id).c_str(), 0, nullptr, 0, KEY_WRITE,
                                      nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    const double us    = std::clamp(frames * 1e6 / sampleRate, static_cast<double>(t.minUs), static_cast<double>(t.maxUs));
    const DWORD  value = static_cast<DWORD>(static_cast<LONG>(us));
    RegSetValueExW(h, exclusive ? t.exclusive : t.shared, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(h);
}

// Latência a informar antes de abrir os streams: a prevista mais o que se mediu nesse dispositivo.
void UniversalWasapiAsioDriver::UpdateLatencyEstimate(long blockFrames) {
    const double rate      = m_sampleRate.load();
    const bool   exclusive = m_config.useExclusiveMode;
    const LatencyEstimate e = EstimateLatency(blockFrames, rate, exclusive);
    m_inputLatencyFrames  = std::max(blockFrames, e.inputFrames  + LoadDeviceTuning(m_captureId, kTuneInExtra, exclusive, rate));
    m_outputLatencyFrames = std::max(blockFrames, e.outputFrames + LoadDeviceTuning(m_renderId, kTuneOutExtra, exclusive, rate));
}

// ============================================================================
// 3. DESCOBERTA E SELEÇÃO DE DISPOSITIVOS DE ENTRADA (MESA USB) E SAÍDA
// ============================================================================

void UniversalWasapiAsioDriver::GetEndpointFriendlyName(IMMDevice* pDevice, wchar_t* outName, size_t maxLen) {
    if (!outName || maxLen == 0) return;
    std::wcsncpy(outName, L"Dispositivo de Áudio", maxLen - 1);
    outName[maxLen - 1] = L'\0';
    if (!pDevice) return;

    IPropertyStore* pProps = nullptr;
    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR && varName.pwszVal) {
            std::wcsncpy(outName, varName.pwszVal, maxLen - 1);
            outName[maxLen - 1] = L'\0';
        }
        PropVariantClear(&varName);
        pProps->Release();
    }
}

IMMDevice* UniversalWasapiAsioDriver::ResolveEndpoint(
    IMMDeviceEnumerator* pEnum,
    EDataFlow dataFlow,
    const wchar_t* preferredId,
    bool preferUsbForInput,
    wchar_t* outName,
    size_t maxNameLen) {
    if (!pEnum) return nullptr;
    IMMDevice* pResult = nullptr;

    // 1. Se o usuário escolheu um dispositivo específico no Painel de Controle do nanoAudio,
    //    verifica obrigatoriamente se ele está ATIVO (DEVICE_STATE_ACTIVE).
    //    Quando a mesa muda de porta USB, o Windows cria um novo GUID ("2- BEHRINGER...") e o
    //    GUID antigo fica NOTPRESENT/UNPLUGGED, mas GetDevice() ainda retornaria S_OK!
    if (preferredId && preferredId[0] != L'\0') {
        if (SUCCEEDED(pEnum->GetDevice(preferredId, &pResult)) && pResult) {
            DWORD devState = 0;
            if (SUCCEEDED(pResult->GetState(&devState)) && devState == DEVICE_STATE_ACTIVE) {
                GetEndpointFriendlyName(pResult, outName, maxNameLen);
                return pResult;
            }
            wchar_t staleName[128] = {};
            GetEndpointFriendlyName(pResult, staleName, 128);
            NanoLog(L"Dispositivo salvo inativo/desconectado: '%ls' (state=0x%lX). Usando fallback ativo.",
                    staleName, static_cast<unsigned long>(devState));
            pResult->Release();
            pResult = nullptr;
        }
    }

    // 2. Se for entrada (eCapture) e estiver em modo automático, procura primeiro uma Mesa/Interface USB
    if (dataFlow == eCapture && preferUsbForInput) {
        IMMDeviceCollection* pCol = nullptr;
        if (SUCCEEDED(pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCol)) && pCol) {
            UINT count = 0;
            pCol->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                    wchar_t name[128] = {};
                    GetEndpointFriendlyName(pDev, name, 128);
                    // CORREÇÃO: "Line"/"Linha" foram removidos porque capturavam a
                    // "Entrada de linha (Realtek)" da placa-mãe no lugar da mesa USB.
                    // A Behringer XENYX aparece como "Microfone (USB Audio CODEC)".
                    const bool isOnboard = ContainsIgnoreCase(name, L"Realtek") ||
                                           ContainsIgnoreCase(name, L"Stereo Mix") ||
                                           ContainsIgnoreCase(name, L"Mixagem");
                    if (!isOnboard &&
                        (ContainsIgnoreCase(name, L"USB") || ContainsIgnoreCase(name, L"CODEC") ||
                         ContainsIgnoreCase(name, L"Behringer") || ContainsIgnoreCase(name, L"XENYX") ||
                         ContainsIgnoreCase(name, L"Mixer") || ContainsIgnoreCase(name, L"Mesa") ||
                         ContainsIgnoreCase(name, L"Interface"))) {
                        std::wcsncpy(outName, name, maxNameLen - 1);
                        outName[maxNameLen - 1] = L'\0';
                        pCol->Release();
                        return pDev;
                    }
                    pDev->Release();
                }
            }
            pCol->Release();
        }
    }

    // 3. Usa o dispositivo padrão do Windows (eConsole ou eCommunications)
    if (FAILED(pEnum->GetDefaultAudioEndpoint(dataFlow, eConsole, &pResult)) || !pResult) {
        pEnum->GetDefaultAudioEndpoint(dataFlow, eCommunications, &pResult);
    }
    if (pResult) {
        GetEndpointFriendlyName(pResult, outName, maxNameLen);
    }
    return pResult;
}

bool UniversalWasapiAsioDriver::ProbeHardwareCapabilities() {
    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum)
    );
    if (FAILED(hr) || !pEnum) return false;

    auto probe = [](IMMDevice* dev, double* rate, long* channels) {
        IAudioClient* client = nullptr;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) || !client) return;
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            if (rate) *rate = static_cast<double>(mix->nSamplesPerSec);
            *channels = std::max<long>(2, mix->nChannels); // mono é aberto como estéreo
            CoTaskMemFree(mix);
        }
        client->Release();
    };

    wchar_t name[128] = {};
    if (IMMDevice* pRender = ResolveEndpoint(pEnum, eRender, m_config.outputDeviceId, false, name, 128)) {
        probe(pRender, &m_nativeMixRate, &m_numOutputs);
        m_renderId = EndpointId(pRender);
        pRender->Release();
    }
    if (IMMDevice* pCapture = ResolveEndpoint(pEnum, eCapture, m_config.inputDeviceId, true, name, 128)) {
        probe(pCapture, nullptr, &m_numInputs);
        m_captureId = EndpointId(pCapture);
        pCapture->Release();
    }
    pEnum->Release();
    return true;
}

// ============================================================================
// 4. THREAD DE STREAMING EM TEMPO REAL (MTA-SAFE + GETMIXFORMAT + FIFO)
// ============================================================================

// Interface COM IPolicyConfig do Windows (10/11) para sincronizar a taxa de amostragem entre
// Entrada e Saída quando a mesa USB tem clock único de hardware (ex.: Behringer USB WDM 2.8.40 / PCM2902).
// Sem isso, quando o Windows deixa a Saída em 48000 Hz e a Entrada em 44100 Hz, o driver WDM da
// Behringer rejeita o segundo pino com 0x8889000F (AUDCLNT_E_ENDPOINT_CREATE_FAILED).
static const CLSID CLSID_CPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}
};
static const IID IID_IPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}
};

interface IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};

// Identificador do aparelho físico (entrada e saída da mesma mesa USB têm o mesmo)
static const PROPERTYKEY kPkeyDeviceContainerId = {
    { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2
};

static bool GetContainerId(IMMDevice* dev, GUID* out) {
    bool ok = false;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kPkeyDeviceContainerId, &v)) && v.vt == VT_CLSID && v.puuid) {
            *out = *v.puuid;
            ok = true;
        }
        PropVariantClear(&v);
        props->Release();
    }
    return ok;
}

static bool IsSamePhysicalDevice(IMMDevice* a, IMMDevice* b) {
    // {00000000-0000-0000-FFFF-FFFFFFFFFFFF} = "o próprio computador" (áudio da placa-mãe)
    static const GUID kThisPc = { 0, 0, 0, { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
    GUID ga, gb;
    return GetContainerId(a, &ga) && GetContainerId(b, &gb) && IsEqualGUID(ga, gb) && !IsEqualGUID(ga, kThisPc);
}

// Só mexe no formato do Windows quando é preciso: entrada e saída do MESMO aparelho em taxas
// diferentes. Com aparelhos distintos cada um tem o próprio clock e não há conflito.
static void SyncEndpointSampleRates(IMMDevice* pRender, IMMDevice* pCapture) {
    if (!pRender || !pCapture || !IsSamePhysicalDevice(pRender, pCapture)) return;
    LPWSTR renId = nullptr, capId = nullptr;
    pRender->GetId(&renId);
    pCapture->GetId(&capId);
    if (renId && capId) {
        IPolicyConfig* pPolicy = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
                                       IID_IPolicyConfig, reinterpret_cast<void**>(&pPolicy))) && pPolicy) {
            WAVEFORMATEX *fmtCap = nullptr, *fmtRen = nullptr;
            if (SUCCEEDED(pPolicy->GetDeviceFormat(capId, 0, &fmtCap)) &&
                SUCCEEDED(pPolicy->GetDeviceFormat(renId, 0, &fmtRen)) &&
                fmtCap && fmtRen && fmtCap->nSamplesPerSec != fmtRen->nSamplesPerSec) {
                const DWORD targetRate = fmtCap->nSamplesPerSec;
                NanoLog(L"Sincronizando taxa de Entrada (%lu Hz) e Saida (%lu Hz -> %lu Hz) para evitar erro 0x8889000F...",
                        fmtCap->nSamplesPerSec, fmtRen->nSamplesPerSec, targetRate);
                fmtRen->nSamplesPerSec  = targetRate;
                fmtRen->nAvgBytesPerSec = targetRate * fmtRen->nBlockAlign;
                pPolicy->SetDeviceFormat(renId, fmtRen, nullptr);
            }
            if (fmtCap) CoTaskMemFree(fmtCap);
            if (fmtRen) CoTaskMemFree(fmtRen);
            pPolicy->Release();
        }
    }
    if (renId) CoTaskMemFree(renId);
    if (capId) CoTaskMemFree(capId);
}

// Sessão de áudio própria do driver: o controle "por aplicativo" do Windows para a DAW em
// outros modos (WASAPI, DirectSound) não interfere aqui, e vice-versa.
static const GUID kAudioSessionGuid = {
    0x3b7c1d52, 0x9e44, 0x4c0a, { 0x8d, 0x21, 0x5f, 0x6a, 0x90, 0xe3, 0x7b, 0x14 }
};
static const GUID kSubtypePcm   = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

template <class T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// Int32 também cobre "24 bits válidos em 32" (alinhados no bit mais significativo).
enum class SampleKind { Float32, Int16, Int24, Int32 };

static const wchar_t* SampleKindName(SampleKind k) {
    switch (k) {
        case SampleKind::Int16: return L"16-bit";
        case SampleKind::Int24: return L"24-bit";
        case SampleKind::Int32: return L"32-bit";
        default:                return L"float";
    }
}

static bool SampleKindOf(const WAVEFORMATEX* w, SampleKind* kind) {
    WORD tag = w->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        tag = static_cast<WORD>(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w)->SubFormat.Data1);
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && w->wBitsPerSample == 32) { *kind = SampleKind::Float32; return true; }
    if (tag != WAVE_FORMAT_PCM) return false;
    if (w->wBitsPerSample == 16) { *kind = SampleKind::Int16; return true; }
    if (w->wBitsPerSample == 24) { *kind = SampleKind::Int24; return true; }
    if (w->wBitsPerSample == 32) { *kind = SampleKind::Int32; return true; }
    return false;
}

// Lê um canal intercalado para float. Retorna o pico.
static float LoadChannel(const BYTE* src, size_t stride, SampleKind kind, float* dst, size_t n) {
    switch (kind) {
        case SampleKind::Float32:
            for (size_t i = 0; i < n; ++i, src += stride) std::memcpy(&dst[i], src, 4);
            break;
        case SampleKind::Int16:
            for (size_t i = 0; i < n; ++i, src += stride) {
                int16_t v;
                std::memcpy(&v, src, 2);
                dst[i] = static_cast<float>(v) * (1.0f / 32768.0f);
            }
            break;
        case SampleKind::Int24:
            for (size_t i = 0; i < n; ++i, src += stride) {
                const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(src[0]) << 8) |
                                                       (static_cast<uint32_t>(src[1]) << 16) |
                                                       (static_cast<uint32_t>(src[2]) << 24));
                dst[i] = static_cast<float>(v >> 8) * (1.0f / 8388608.0f);
            }
            break;
        case SampleKind::Int32:
            for (size_t i = 0; i < n; ++i, src += stride) {
                int32_t v;
                std::memcpy(&v, src, 4);
                dst[i] = static_cast<float>(v) * (1.0f / 2147483648.0f);
            }
            break;
    }
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(dst[i]));
    return peak;
}

// Grava um canal float no formato do dispositivo, com ganho em rampa (g0 -> g1) ao longo do bloco.
static void StoreChannel(BYTE* dst, size_t stride, SampleKind kind, const float* src, size_t n, float g0, float g1) {
    const float dg = (g1 - g0) / static_cast<float>(n);
    float g = g0;
    switch (kind) {
        case SampleKind::Float32:
            // Sem corte em 0 dBFS: o ganho de compensação é desfeito pelo volume do Windows
            for (size_t i = 0; i < n; ++i, dst += stride) {
                g += dg;
                const float v = src[i] * g;
                std::memcpy(dst, &v, 4);
            }
            break;
        case SampleKind::Int16:
            for (size_t i = 0; i < n; ++i, dst += stride) {
                g += dg;
                const int16_t v = static_cast<int16_t>(std::lrint(std::clamp(src[i] * g * 32768.0f, -32768.0f, 32767.0f)));
                std::memcpy(dst, &v, 2);
            }
            break;
        case SampleKind::Int24:
            for (size_t i = 0; i < n; ++i, dst += stride) {
                g += dg;
                const int32_t v = static_cast<int32_t>(std::lrint(std::clamp(src[i] * g * 8388608.0f, -8388608.0f, 8388607.0f)));
                dst[0] = static_cast<BYTE>(v);
                dst[1] = static_cast<BYTE>(v >> 8);
                dst[2] = static_cast<BYTE>(v >> 16);
            }
            break;
        case SampleKind::Int32:
            for (size_t i = 0; i < n; ++i, dst += stride) {
                g += dg;
                // 2147483520 = maior float que cabe em int32
                const int32_t v = static_cast<int32_t>(std::lrint(std::clamp(src[i] * g * 2147483648.0f, -2147483648.0f, 2147483520.0f)));
                std::memcpy(dst, &v, 4);
            }
            break;
    }
}

struct RenderStream {
    IAudioClient*       client       = nullptr;
    IAudioRenderClient* service      = nullptr;
    SampleKind          kind         = SampleKind::Float32;
    long                channels     = 2;
    long                blockAlign   = 8;
    UINT32              bufferFrames = 0;
    long                periodFrames = 480;
    bool                exclusive    = false;
    bool                usesEvent    = false;
    HRESULT             hr           = E_FAIL;

    void Close() {
        SafeRelease(service);
        SafeRelease(client);
        usesEvent = false;
    }
};

struct CaptureStream {
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* service    = nullptr;
    SampleKind           kind       = SampleKind::Float32;
    long                 channels   = 2;
    long                 blockAlign = 8;
    UINT32               bufferFrames = 0;
    double               rate       = 48000.0;
    bool                 exclusive  = false;
    HRESULT              hr         = E_FAIL;

    void Close() {
        SafeRelease(service);
        SafeRelease(client);
    }
};

static HRESULT ActivateClient(IMMDevice* dev, IAudioClient** client) {
    *client = nullptr;
    const HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client));
    return (SUCCEEDED(hr) && !*client) ? E_FAIL : hr;
}

// Formato do mixer do dispositivo (canais e máscara). Se falhar, assume estéreo.
static void GetMixLayout(IMMDevice* dev, WORD* channels, DWORD* mask) {
    *channels = 2;
    *mask     = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    IAudioClient* client = nullptr;
    if (FAILED(ActivateClient(dev, &client))) return;
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
        *channels = std::max<WORD>(1, mix->nChannels);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
            *mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        } else if (mix->nChannels != 2) {
            *mask = 0;
        }
        CoTaskMemFree(mix);
    }
    client->Release();
}

static WAVEFORMATEXTENSIBLE MakeFormat(WORD channels, DWORD mask, DWORD rate, WORD container, WORD valid, bool isFloat, bool extensible) {
    WAVEFORMATEXTENSIBLE w = {};
    w.Format.wFormatTag      = extensible ? WAVE_FORMAT_EXTENSIBLE
                                          : static_cast<WORD>(isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
    w.Format.nChannels       = channels;
    w.Format.nSamplesPerSec  = rate;
    w.Format.wBitsPerSample  = container;
    w.Format.nBlockAlign     = static_cast<WORD>(channels * (container / 8));
    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
    if (extensible) {
        w.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        w.Samples.wValidBitsPerSample = valid;
        w.dwChannelMask               = mask;
        w.SubFormat                   = isFloat ? kSubtypeFloat : kSubtypePcm;
    }
    return w;
}

// Modo exclusivo: tenta os formatos na ordem da resolução escolhida no painel.
static HRESULT InitExclusive(IMMDevice* dev, DWORD rate, long bitDepth, REFERENCE_TIME bufferDur,
                             IAudioClient** outClient, SampleKind* kind, long* channels, long* blockAlign) {
    struct Fmt { WORD container, valid; };
    static const Fmt kOrder32[4] = { {32, 32}, {32, 24}, {24, 24}, {16, 16} };
    static const Fmt kOrder24[4] = { {32, 24}, {24, 24}, {32, 32}, {16, 16} };
    static const Fmt kOrder16[4] = { {16, 16}, {32, 24}, {24, 24}, {32, 32} };
    const Fmt* order = (bitDepth == 16) ? kOrder16 : (bitDepth == 24) ? kOrder24 : kOrder32;

    WORD  mixChannels = 2;
    DWORD mixMask     = 0;
    GetMixLayout(dev, &mixChannels, &mixMask);

    HRESULT last = AUDCLNT_E_UNSUPPORTED_FORMAT;
    for (int i = 0; i < 4; ++i) {
        for (int ext = 1; ext >= 0; --ext) {
            if (!ext && order[i].container != order[i].valid) continue;
            const WAVEFORMATEXTENSIBLE w = MakeFormat(mixChannels, mixMask, rate, order[i].container, order[i].valid, false, ext == 1);

            IAudioClient* client = nullptr;
            HRESULT hr = ActivateClient(dev, &client);
            if (FAILED(hr)) return hr;
            hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &w.Format, nullptr);
            if (hr != S_OK) {
                client->Release();
                if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || hr == AUDCLNT_E_DEVICE_IN_USE) return hr;
                continue;
            }
            REFERENCE_TIME defPeriod = 0, minPeriod = 0;
            client->GetDevicePeriod(&defPeriod, &minPeriod);
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, bufferDur, minPeriod, &w.Format, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 frames = 0;
                client->GetBufferSize(&frames);
                client->Release();
                hr = ActivateClient(dev, &client);
                if (FAILED(hr)) return hr;
                const REFERENCE_TIME aligned = static_cast<REFERENCE_TIME>(10000000.0 * frames / rate + 0.5);
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, aligned, minPeriod, &w.Format, nullptr);
            }
            if (SUCCEEDED(hr)) {
                *outClient  = client;
                *channels   = w.Format.nChannels;
                *blockAlign = w.Format.nBlockAlign;
                SampleKindOf(&w.Format, kind);
                return hr;
            }
            client->Release();
            last = hr;
            if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || hr == AUDCLNT_E_DEVICE_IN_USE) return hr;
        }
    }
    return last;
}

static bool FinishRender(RenderStream& rs, long blockFrames) {
    if (SUCCEEDED(rs.hr)) rs.hr = rs.client->GetBufferSize(&rs.bufferFrames);
    if (SUCCEEDED(rs.hr)) rs.hr = rs.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rs.service));
    if (SUCCEEDED(rs.hr) && rs.bufferFrames < static_cast<UINT32>(blockFrames)) rs.hr = AUDCLNT_E_BUFFER_SIZE_ERROR;
    if (SUCCEEDED(rs.hr)) return true;
    rs.Close();
    return false;
}

// Saída exclusiva por tempo (sem evento): o mesmo loop por "padding" do modo compartilhado serve
// para os dois, e funciona em drivers que não suportam eventos no modo exclusivo.
static bool OpenRenderExclusive(IMMDevice* dev, double rate, long blockFrames, long bitDepth, RenderStream& rs) {
    const REFERENCE_TIME dur = static_cast<REFERENCE_TIME>(10000000.0 * (blockFrames * 2 + rate * 0.030) / rate + 0.5);
    rs.hr = InitExclusive(dev, static_cast<DWORD>(rate), bitDepth, dur, &rs.client, &rs.kind, &rs.channels, &rs.blockAlign);
    if (FAILED(rs.hr)) return false;
    rs.exclusive    = true;
    rs.periodFrames = static_cast<long>(rate * 0.010 + 0.5);
    return FinishRender(rs, blockFrames);
}

// Saída compartilhada: float 32-bit; o Windows converte taxa e canais se preciso.
static bool OpenRenderShared(IMMDevice* dev, double rate, long blockFrames, HANDLE hEvent, RenderStream& rs) {
    WORD  channels = 2;
    DWORD mask     = 0;
    GetMixLayout(dev, &channels, &mask);
    if (channels < 2) { channels = 2; mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; } // o Windows soma em mono

    for (int attempt = 0; attempt < 2; ++attempt) { // 0 = por evento, 1 = por tempo
        rs.Close();
        // Um IAudioClient que falhou no Initialize não pode ser reutilizado: cria um novo.
        rs.hr = ActivateClient(dev, &rs.client);
        if (FAILED(rs.hr)) return false;

        const WAVEFORMATEXTENSIBLE fmt = MakeFormat(channels, mask, static_cast<DWORD>(rate), 32, 32, true, channels > 2);
        REFERENCE_TIME defPeriod = 100000;
        rs.client->GetDevicePeriod(&defPeriod, nullptr);
        rs.periodFrames = std::max<long>(1, static_cast<long>(std::ceil(static_cast<double>(defPeriod) * rate / 1e7)));

        // Espaço para o alvo crescer se o PC não acompanhar (ver "targetFill" no loop)
        const double frames = std::max<double>(blockFrames * 3.0, 1024.0) + rs.periodFrames * 2.0;
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
        if (attempt == 0) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        rs.hr = rs.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, static_cast<REFERENCE_TIME>(1e7 * frames / rate + 0.5), 0,
                                      &fmt.Format, &kAudioSessionGuid);
        if (SUCCEEDED(rs.hr) && attempt == 0) rs.hr = rs.client->SetEventHandle(hEvent);
        if (FAILED(rs.hr)) continue;

        rs.kind       = SampleKind::Float32;
        rs.channels   = channels;
        rs.blockAlign = fmt.Format.nBlockAlign;
        rs.exclusive  = false;
        rs.usesEvent  = (attempt == 0);
        if (FinishRender(rs, blockFrames)) return true;
    }
    rs.Close();
    return false;
}

// Abre a entrada (sem iniciar).
static bool OpenCapture(IMMDevice* dev, double rate, long blockFrames, bool exclusive, long bitDepth, CaptureStream& cs) {
    WORD  channels = 2;
    DWORD mask     = 0;
    GetMixLayout(dev, &channels, &mask);
    // Em tempo, não em frames: 2048 frames a 96 kHz são 21 ms, menos que o intervalo entre as rajadas
    // de alguns drivers USB (~23 ms), e o Windows perdia áudio. Buffer maior não aumenta a latência.
    const REFERENCE_TIME blockDur  = static_cast<REFERENCE_TIME>(1e7 * blockFrames * 4 / rate);
    const REFERENCE_TIME dur       = std::max<REFERENCE_TIME>(blockDur, 1000000);  // 100 ms
    const REFERENCE_TIME durExclus = std::max<REFERENCE_TIME>(blockDur, 500000);   //  50 ms

    // Cada tentativa usa um IAudioClient novo (um cliente que falhou não pode ser reinicializado)
    auto tryShared = [&](const WAVEFORMATEX* fmt, DWORD flags) -> bool {
        cs.Close();
        cs.hr = ActivateClient(dev, &cs.client);
        if (SUCCEEDED(cs.hr)) cs.hr = cs.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, fmt, nullptr);
        if (FAILED(cs.hr) || !SampleKindOf(fmt, &cs.kind)) {
            if (SUCCEEDED(cs.hr)) cs.hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
            cs.Close();
            return false;
        }
        cs.channels   = fmt->nChannels;
        cs.blockAlign = fmt->nBlockAlign;
        cs.rate       = static_cast<double>(fmt->nSamplesPerSec);
        cs.exclusive  = false;
        return true;
    };
    auto tryExclusive = [&](double r, long bits) -> bool {
        cs.Close();
        cs.hr = InitExclusive(dev, static_cast<DWORD>(r), bits, durExclus, &cs.client, &cs.kind, &cs.channels, &cs.blockAlign);
        if (FAILED(cs.hr)) return false;
        cs.rate      = r;
        cs.exclusive = true;
        return true;
    };

    bool opened = false;
    if (exclusive) {
        opened = tryExclusive(rate, bitDepth);
        if (!opened) NanoLog(L"Entrada exclusiva indisponivel (hr=0x%08lX). Usando modo compartilhado.", static_cast<unsigned long>(cs.hr));
    }

    // Tentativa A: float 32-bit na taxa do ASIO, com conversão automática do Windows
    const DWORD autoFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (!opened) {
        const WAVEFORMATEXTENSIBLE fmt = MakeFormat(channels, mask, static_cast<DWORD>(rate), 32, 32, true, channels > 2);
        opened = tryShared(&fmt.Format, autoFlags);
        if (!opened && channels != 2) {
            const WAVEFORMATEXTENSIBLE stereo = MakeFormat(2, 0, static_cast<DWORD>(rate), 32, 32, true, false);
            opened = tryShared(&stereo.Format, autoFlags);
        }
    }

    // Tentativa B: formato nativo da mesa (sem conversão do Windows) + conversor de taxa do FIFO
    if (!opened) {
        NanoLog(L"Tentativa A falhou (hr=0x%08lX). Abrindo a entrada no formato nativo...", static_cast<unsigned long>(cs.hr));
        IAudioClient* probe = nullptr;
        WAVEFORMATEX* mix   = nullptr;
        if (SUCCEEDED(ActivateClient(dev, &probe))) {
            probe->GetMixFormat(&mix);
            probe->Release();
        }
        if (mix) {
            opened = tryShared(mix, 0);
            CoTaskMemFree(mix);
        }
    }

    // Tentativa C: exclusivo 16-bit (caso o driver WDM USB 1.1, como o Behringer 2.8.40, recuse o
    // modo compartilhado com 0x8889000F)
    if (!opened) {
        opened = tryExclusive(rate, 16) || tryExclusive(rate == 44100.0 ? 48000.0 : 44100.0, 16);
        if (opened) NanoLog(L"Tentativa C (Exclusivo %.0f Hz) abriu com sucesso!", cs.rate);
    }

    if (opened) cs.hr = cs.client->GetBufferSize(&cs.bufferFrames);
    if (opened && SUCCEEDED(cs.hr)) cs.hr = cs.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cs.service));
    if (!opened || FAILED(cs.hr)) {
        cs.Close();
        return false;
    }
    return true;
}

// Deixa o stream em 100% no Mixer de Volume e fora da "atenuação durante chamadas" do Windows.
static void SetupRenderSession(IAudioClient* client, bool fullVolume) {
    IAudioSessionControl* control = nullptr;
    if (SUCCEEDED(client->GetService(__uuidof(IAudioSessionControl), reinterpret_cast<void**>(&control))) && control) {
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) && control2) {
            control2->SetDuckingPreference(TRUE); // TRUE = não participar da atenuação
            control2->Release();
        }
        control->Release();
    }
    if (!fullVolume) return;
    ISimpleAudioVolume* volume = nullptr;
    if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))) && volume) {
        volume->SetMasterVolume(1.0f, nullptr);
        volume->SetMute(FALSE, nullptr);
        volume->Release();
    }
}

DWORD WINAPI UniversalWasapiAsioDriver::AudioThreadProc(LPVOID param) {
    return static_cast<UniversalWasapiAsioDriver*>(param)->RunRealTimeAudioLoop();
}

DWORD UniversalWasapiAsioDriver::RunRealTimeAudioLoop() {
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);

    DWORD mmcssTaskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    if (hMmcss) {
        AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
    }
    _mm_setcsr(_mm_getcsr() | 0x8040); // FTZ + DAZ: números denormais custam caro na CPU

    // Cópia local dos parâmetros (m_streamConfig foi copiado no start()): o Painel de Controle pode
    // alterar m_config em outra thread enquanto o áudio roda.
    const long             blockFrames = m_activeBufferFrames;
    const double           sampleRate  = (m_sampleRate.load() > 1000.0) ? m_sampleRate.load() : 48000.0;
    const DriverUserConfig cfg         = m_streamConfig;

    long numIn = 0;
    for (const ASIOBufferInfo& bi : m_activeChannels) {
        if (bi.isInput) numIn = std::max(numIn, bi.channelNum + 1);
    }
    // Sem canal de entrada ativo na DAW não há por que abrir o microfone
    const bool wantInput = cfg.enableInputCapture && numIn > 0;

    NanoLog(L"==== start(): bloco=%ld frames | taxa ASIO=%.0f Hz | entradas ASIO=%ld | captura=%ls | modo=%ls",
            blockFrames, sampleRate, numIn, wantInput ? L"LIGADA" : L"DESLIGADA",
            cfg.useExclusiveMode ? L"exclusivo" : L"compartilhado");

    // Cria o enumerador e os endpoints dentro da thread MTA (evita falha de marshaling COM STA->MTA)
    IMMDeviceEnumerator* pEnum          = nullptr;
    IMMDevice*           pRenderDevice  = nullptr;
    IMMDevice*           pCaptureDevice = nullptr;
    wchar_t renderName[128] = L"(nenhuma)", captureName[128] = L"(nenhuma)";
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum))) && pEnum) {
        pRenderDevice = ResolveEndpoint(pEnum, eRender, cfg.outputDeviceId, false, renderName, 128);
        if (wantInput) {
            pCaptureDevice = ResolveEndpoint(pEnum, eCapture, cfg.inputDeviceId, true, captureName, 128);
            if (!cfg.useExclusiveMode) SyncEndpointSampleRates(pRenderDevice, pCaptureDevice);
        }
    }
    NanoLog(L"Saida escolhida: %ls | Entrada escolhida: %ls", renderName, captureName);

    // ------------------------------------------------------------------------
    // 1. STREAM DE SAÍDA (Render)
    // ------------------------------------------------------------------------
    RenderStream rs;
    bool renderOk = false;
    if (pRenderDevice) {
        if (cfg.useExclusiveMode) {
            renderOk = OpenRenderExclusive(pRenderDevice, sampleRate, blockFrames, cfg.bitDepth, rs);
            if (!renderOk) NanoLog(L"Saida exclusiva indisponivel (hr=0x%08lX). Usando modo compartilhado.", static_cast<unsigned long>(rs.hr));
        }
        if (!renderOk) renderOk = OpenRenderShared(pRenderDevice, sampleRate, blockFrames, m_renderEvent, rs);
    }
    NanoLog(L"Saida WASAPI: hr=0x%08lX | %ls %ls | canais=%ld | buffer=%u frames | evento=%ls | OK=%ls",
            static_cast<unsigned long>(rs.hr), rs.exclusive ? L"exclusivo" : L"compartilhado", SampleKindName(rs.kind),
            rs.channels, rs.bufferFrames, rs.usesEvent ? L"sim" : L"nao", renderOk ? L"SIM" : L"NAO (usando relogio interno)");

    nanoAudioVolume::Compensator volume;
    if (renderOk && !rs.exclusive) {
        SetupRenderSession(rs.client, cfg.ignoreWindowsVolume);
        const nanoAudioVolume::EndpointVolume ev = nanoAudioVolume::Query(pRenderDevice);
        const bool compensating = cfg.ignoreWindowsVolume && volume.Start(pRenderDevice, rs.channels);
        NanoLog(L"Volume do Windows na saida: %.0f%% (%.1f dB, %ls%ls) | compensacao=%ls (%+.1f dB)",
                ev.scalar * 100.0f, ev.db, ev.software ? L"por software" : L"no hardware", ev.muted ? L", MUDO" : L"",
                compensating ? L"LIGADA" : L"desligada", 20.0f * std::log10(volume.Gain(0)));
    }

    // ------------------------------------------------------------------------
    // 2. STREAM DE CAPTURA (Mesa de Som USB / Microfone)
    // ------------------------------------------------------------------------
    CaptureStream cs;
    cs.rate = sampleRate;
    bool captureOk = wantInput && pCaptureDevice && OpenCapture(pCaptureDevice, sampleRate, blockFrames, cfg.useExclusiveMode, cfg.bitDepth, cs);
    if (wantInput) {
        if (!captureOk) SetError("nanoAudio: falha ao abrir a entrada (mesa de som).");
        NanoLog(L"Entrada WASAPI: hr=0x%08lX | %ls %ls | canais=%ld | taxa=%.0f Hz | ATIVA=%ls",
                static_cast<unsigned long>(cs.hr), cs.exclusive ? L"exclusivo" : L"compartilhado", SampleKindName(cs.kind),
                cs.channels, cs.rate, captureOk ? L"SIM" : L"NAO");
        if (cs.hr == E_ACCESSDENIED) {
            NanoLog(L"ACESSO NEGADO AO MICROFONE: ative em Configuracoes > Privacidade > Microfone "
                    L"a opcao 'Permitir que aplicativos da area de trabalho acessem o microfone'.");
        }
    }

    // ------------------------------------------------------------------------
    // 3. INÍCIO DA SAÍDA E CALIBRAÇÃO
    //    Toca 400 ms de silêncio medindo quantos frames o dispositivo consome de uma vez
    //    (a "rajada"): ~48 em HD Audio exclusivo, 480 no mixer do Windows, o dobro em
    //    drivers USB que puxam dois períodos seguidos. A fila-alvo sai daí, e ainda sobe
    //    sozinha se chegar a esvaziar.
    // ------------------------------------------------------------------------
    const std::wstring renderId  = EndpointId(pRenderDevice);
    const std::wstring captureId = EndpointId(pCaptureDevice);
    const long savedBurst  = LoadDeviceTuning(renderId, kTuneBurst, rs.exclusive, sampleRate);
    const long savedMargin = LoadDeviceTuning(captureId, kTuneMargin, cs.exclusive, sampleRate);
    long targetFill  = 0;
    long burstFrames = rs.periodFrames;
    HANDLE waitHandles[2] = { m_stopEvent, m_renderEvent };
    uint64_t framesWritten = 0;

    auto writeSilence = [&](UINT32 frames) {
        BYTE* pData = nullptr;
        if (frames > 0 && SUCCEEDED(rs.service->GetBuffer(frames, &pData))) {
            rs.service->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            framesWritten += frames;
        }
    };

    if (renderOk) {
        writeSilence(rs.bufferFrames);
        const HRESULT hrStart = rs.client->Start();
        if (FAILED(hrStart)) {
            renderOk = false;
            NanoLog(L"Falha ao iniciar a saida (hr=0x%08lX); usando relogio interno.", static_cast<unsigned long>(hrStart));
        }
    }
    if (renderOk) {
        struct Drop { DWORD ms; long frames; } recent[8] = {};
        const DWORD windowMs = 6; // consumos quase simultâneos contam como um só
        long   burst = 0;
        size_t idx   = 0;
        const DWORD t0 = timeGetTime();
        while (timeGetTime() - t0 < 400) {
            const DWORD w = rs.usesEvent ? WaitForMultipleObjects(2, waitHandles, FALSE, 20) : WaitForSingleObject(m_stopEvent, 1);
            UINT32 padding = 0;
            if (w == WAIT_OBJECT_0 || FAILED(rs.client->GetCurrentPadding(&padding))) break;
            const long drop = static_cast<long>(rs.bufferFrames) - static_cast<long>(padding);
            if (drop <= 0) continue;
            writeSilence(static_cast<UINT32>(drop));
            const DWORD now = timeGetTime();
            if (now - t0 < 60) continue; // na partida o mixer puxa mais para encher a própria fila
            recent[idx++ % 8] = { now, drop };
            long sum = 0;
            for (const Drop& d : recent) {
                if (d.frames > 0 && now - d.ms <= windowMs) sum += d.frames;
            }
            burst = std::max(burst, rs.exclusive ? drop : sum);
        }
        burst       = std::max(burst, savedBurst);
        burstFrames = rs.exclusive ? std::max<long>(burst, 1) : std::clamp<long>(burst, rs.periodFrames, rs.periodFrames * 3);
        targetFill  = rs.exclusive ? burstFrames + blockFrames * 2 + static_cast<long>(sampleRate * 0.002)
                                   : SharedRenderTarget(blockFrames, burstFrames, rs.periodFrames);
        targetFill  = std::min<long>(static_cast<long>(rs.bufferFrames), targetFill);
        NanoLog(L"Saida calibrada: periodo=%ld | rajada=%ld (salva=%ld) | fila-alvo=%ld frames", rs.periodFrames, burstFrames, savedBurst, targetFill);
    }

    // ------------------------------------------------------------------------
    // 4. FIFO DE CAPTURA
    // ------------------------------------------------------------------------
    const long packetFrames = static_cast<long>(sampleRate * 0.010 + 0.5);
    CaptureFifo fifo;
    if (captureOk) {
        // Só agora: durante a calibração ninguém estaria lendo, e a fila da captura transbordaria
        cs.hr = cs.client->Start();
        if (FAILED(cs.hr)) {
            captureOk = false;
            SetError("nanoAudio: falha ao iniciar a entrada.");
            NanoLog(L"Falha ao iniciar a entrada (hr=0x%08lX).", static_cast<unsigned long>(cs.hr));
        }
    }
    if (captureOk) fifo.Configure(numIn, cs.rate, sampleRate, blockFrames, burstFrames, packetFrames, savedMargin);

    std::vector<float>  inMem(static_cast<size_t>(std::max<long>(1, numIn)) * blockFrames, 0.0f);
    std::vector<float*> inPtr(static_cast<size_t>(std::max<long>(1, numIn)));
    for (size_t c = 0; c < inPtr.size(); ++c) inPtr[c] = &inMem[c * blockFrames];
    std::vector<float> curGain(static_cast<size_t>(std::max<long>(1, rs.channels)), 1.0f);
    for (size_t c = 0; c < curGain.size(); ++c) curGain[c] = volume.Gain(static_cast<long>(c));
    const float gainCoef = static_cast<float>(1.0 - std::exp(-blockFrames / (sampleRate * 0.010)));

    LARGE_INTEGER qpcFreq, qpcStart;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);
    auto qpcSeconds = [&]() {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        return static_cast<double>(q.QuadPart) / static_cast<double>(qpcFreq.QuadPart);
    };

    // Latência REAL, pelos relógios do WASAPI: quando foi captada a próxima amostra a entregar à
    // DAW e quando vai tocar a próxima a ser gravada. A soma é o que a DAW usa para alinhar gravações.
    // No modo exclusivo a posição informada por alguns drivers não é confiável (na Realtek começa com
    // ~100 ms de erro): ali vale a fila, mais o passo em trânsito no hardware e a latência declarada.
    IAudioClock*   renderClock     = nullptr;
    UINT64         renderClockFreq = 0;
    REFERENCE_TIME hwLatency       = 0;
    if (renderOk && rs.exclusive) rs.client->GetStreamLatency(&hwLatency);
    if (renderOk && !rs.exclusive && (FAILED(rs.client->GetService(__uuidof(IAudioClock), reinterpret_cast<void**>(&renderClock))) ||
                     FAILED(renderClock->GetFrequency(&renderClockFreq)) || renderClockFreq == 0)) {
        SafeRelease(renderClock);
    }
    double newestCapture = 0.0;   // instante (s) da amostra mais nova no FIFO
    double avgIn = 0.0, avgOut = 0.0;
    long   latSamples  = 0;
    double nextMeasure = qpcSeconds() + 1.5; // espera o FIFO assentar

    auto measureLatency = [&]() {
        const double now = qpcSeconds();
        if (now < nextMeasure || !renderOk) return;
        nextMeasure = now + 0.05;
        double out = -1.0;
        if (rs.exclusive) {
            UINT32 padding = 0;
            if (FAILED(rs.client->GetCurrentPadding(&padding))) return;
            out = static_cast<double>(padding) + burstFrames + static_cast<double>(hwLatency) * sampleRate / 1e7;
        } else {
            UINT64 pos = 0, posQpc = 0;
            if (!renderClock || FAILED(renderClock->GetPosition(&pos, &posQpc)) || posQpc == 0) return;
            const double played = static_cast<double>(pos) * sampleRate / static_cast<double>(renderClockFreq);
            out = (static_cast<double>(posQpc) / 1e7 - now) * sampleRate + (static_cast<double>(framesWritten) - played);
        }
        double in = 0.0;
        if (captureOk) {
            if (newestCapture <= 0.0 || !fifo.Running()) return;
            in = (now - newestCapture + static_cast<double>(fifo.PendingFrames()) / cs.rate) * sampleRate;
        }
        // Descarta leituras sem sentido (drivers com carimbo de tempo ruim)
        if (out < 0.0 || out > sampleRate * 0.5 || in < 0.0 || in > sampleRate * 0.5) return;
        ++latSamples;
        const double k = 1.0 / static_cast<double>(std::min<long>(latSamples, 40));
        avgIn  += (in - avgIn) * k;
        avgOut += (out - avgOut) * k;
        if (latSamples >= 20) {
            m_outputLatencyFrames = static_cast<long>(avgOut + 0.5);
            if (captureOk) m_inputLatencyFrames = static_cast<long>(avgIn + 0.5);
        }
    };

    // Estatísticas para o log de diagnóstico (gravado só no stop)
    uint64_t statPackets = 0, statSilentPackets = 0, statFrames = 0, statGaps = 0, statBlocks = 0, statOutUnderruns = 0;
    float    statMaxPeak = 0.0f;

    uint64_t clockFrames = 0;       // relógio interno (só sem dispositivo de saída)
    bool     deviceLost  = false;

    auto onDeviceLost = [&](const wchar_t* which, HRESULT h) {
        NanoLog(L"Dispositivo de %ls perdido (hr=0x%08lX).", which, static_cast<unsigned long>(h));
        deviceLost = true;
    };

    auto onCaptureLost = [&](HRESULT h) {
        captureOk = false; // a entrada vira silêncio; o áudio da saída continua
        std::fill(inMem.begin(), inMem.end(), 0.0f);
        onDeviceLost(L"entrada", h);
    };

    // Alguns drivers (ex.: Realtek/Intel SST) às vezes entram, no modo exclusivo, em um estado em que
    // devolvem o buffer inteiro com dados velhos. Reiniciar a entrada resolve; se insistir, ela passa
    // para o modo compartilhado (a saída continua exclusiva).
    int    captureRestarts = 0;
    double lastDrain       = 0.0;
    auto recoverExclusiveCapture = [&]() {
        ++captureRestarts;
        NanoLog(L"Entrada exclusiva devolveu dados repetidos; %ls.", captureRestarts <= 3 ? L"reiniciando" : L"passando para o modo compartilhado");
        cs.client->Stop();
        HRESULT h = cs.client->Reset();
        if (captureRestarts <= 3) {
            if (SUCCEEDED(h)) h = cs.client->Start();
        } else {
            cs.Close();
            h = OpenCapture(pCaptureDevice, sampleRate, blockFrames, false, cfg.bitDepth, cs) ? cs.client->Start() : E_FAIL;
            if (SUCCEEDED(h)) fifo.Configure(numIn, cs.rate, sampleRate, blockFrames, burstFrames, packetFrames, savedMargin);
        }
        lastDrain = 0.0;
        if (FAILED(h)) onCaptureLost(h);
    };

    // Lê TODOS os pacotes disponíveis da mesa e coloca no FIFO
    auto drainCapture = [&]() {
        const double drainStart = qpcSeconds();
        const double sinceLast  = drainStart - lastDrain;
        lastDrain = drainStart;
        while (captureOk) {
            BYTE*  data  = nullptr;
            UINT32 n     = 0;
            DWORD  flags = 0;
            UINT64 qpcPos = 0;
            HRESULT h = S_OK;
            if (!cs.exclusive) { // GetNextPacketSize só vale para o modo compartilhado
                h = cs.service->GetNextPacketSize(&n);
                if (SUCCEEDED(h) && n == 0) return;
            }
            if (SUCCEEDED(h)) h = cs.service->GetBuffer(&data, &n, &flags, nullptr, &qpcPos);
            if (h == AUDCLNT_S_BUFFER_EMPTY) return;
            if (FAILED(h)) { onCaptureLost(h); return; }
            if (n == 0) { cs.service->ReleaseBuffer(0); return; }

            // Buffer inteiro cheio sem que o driver tenha demorado a ler: dados repetidos
            if (cs.exclusive && n >= cs.bufferFrames && sinceLast < 0.5 * cs.bufferFrames / cs.rate) {
                cs.service->ReleaseBuffer(n);
                recoverExclusiveCapture();
                return;
            }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
            ++statPackets;
            statFrames += n;
            if (silent) ++statSilentPackets;
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) && statPackets > 1) ++statGaps;
            if (qpcPos != 0 && !(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                // Uma amostra não pode ter sido captada depois de recebida (há drivers que carimbam a entrega)
                newestCapture = std::min(static_cast<double>(qpcPos) / 1e7 + static_cast<double>(n) / cs.rate, qpcSeconds());
            }

            for (UINT32 done = 0; done < n;) {
                const size_t chunk = std::min<size_t>(n - done, fifo.MaxWrite());
                fifo.PrepareWrite(chunk);
                for (long c = 0; c < numIn; ++c) {
                    float* dst = fifo.WritePtr(c);
                    if (silent) {
                        std::memset(dst, 0, chunk * sizeof(float));
                        continue;
                    }
                    // Mesa mono -> duplica o canal 1; mesa estéreo -> In1=L, In2=R
                    const size_t srcCh = static_cast<size_t>(std::min<long>(c, cs.channels - 1));
                    const BYTE*  src   = data + static_cast<size_t>(done) * cs.blockAlign + srcCh * (cs.blockAlign / cs.channels);
                    statMaxPeak = std::max(statMaxPeak, LoadChannel(src, cs.blockAlign, cs.kind, dst, chunk));
                }
                fifo.CommitWrite(chunk);
                done += static_cast<UINT32>(chunk);
            }
            cs.service->ReleaseBuffer(n);
        }
    };

    const DWORD blockMs = std::max<DWORD>(1, static_cast<DWORD>((1000.0 * blockFrames) / sampleRate));
    int outputReadyMisses = 0;

    // Processa UM bloco ASIO: entrada -> bufferSwitch da DAW -> saída
    auto processBlock = [&](long dbIdx) {
        if (captureOk && fifo.Starving()) {
            // Pacote de entrada levemente atrasado: espera um pouco, gastando no máximo metade do que
            // já está na fila de saída, em vez de entregar silêncio à DAW.
            UINT32 queued = 0;
            if (!renderOk || FAILED(rs.client->GetCurrentPadding(&queued))) queued = 0;
            const double deadline = qpcSeconds() + std::min(0.004, static_cast<double>(queued) * 0.5 / sampleRate);
            while (captureOk && fifo.Starving() && qpcSeconds() < deadline) {
                if (WaitForSingleObject(m_stopEvent, 1) == WAIT_OBJECT_0) break;
                drainCapture();
            }
        }
        if (captureOk) fifo.ReadBlock(inPtr.data());
        for (const ASIOBufferInfo& bi : m_activeChannels) {
            if (bi.isInput) std::memcpy(bi.buffers[dbIdx], inPtr[bi.channelNum], static_cast<size_t>(blockFrames) * sizeof(float));
        }

        // O ASIO define systemTime na mesma base do timeGetTime() (usada pelas DAWs para o MIDI)
        const uint64_t nowNs = static_cast<uint64_t>(timeGetTime()) * 1000000ULL;
        m_systemTimeNs.store(nowNs, std::memory_order_relaxed);
        ResetEvent(m_outputReadyEvent);

        if (m_hostUsesTimeInfo) {
            ASIOTime timeParams = {};
            const uint64_t pos = m_samplePosition.load(std::memory_order_relaxed);
            timeParams.timeInfo.speed             = 1.0;
            timeParams.timeInfo.sampleRate        = sampleRate;
            timeParams.timeInfo.samplePosition.hi = static_cast<unsigned long>(pos >> 32);
            timeParams.timeInfo.samplePosition.lo = static_cast<unsigned long>(pos & 0xFFFFFFFFULL);
            timeParams.timeInfo.systemTime.hi     = static_cast<unsigned long>(nowNs >> 32);
            timeParams.timeInfo.systemTime.lo     = static_cast<unsigned long>(nowNs & 0xFFFFFFFFULL);
            timeParams.timeInfo.flags             = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
            m_callbacks.bufferSwitchTimeInfo(&timeParams, dbIdx, ASIOTrue);
        } else {
            m_callbacks.bufferSwitch(dbIdx, ASIOTrue);
        }

        // Host que processa em outra thread: espera o outputReady() antes de ler a saída
        if (m_hostUsesOutputReady.load(std::memory_order_relaxed)) {
            if (WaitForSingleObject(m_outputReadyEvent, blockMs * 2) == WAIT_OBJECT_0) {
                outputReadyMisses = 0;
            } else if (++outputReadyMisses >= 4) {
                m_hostUsesOutputReady.store(false, std::memory_order_relaxed);
                outputReadyMisses = 0;
            }
        }

        if (renderOk) {
            BYTE* pData = nullptr;
            if (SUCCEEDED(rs.service->GetBuffer(static_cast<UINT32>(blockFrames), &pData)) && pData) {
                std::memset(pData, 0, static_cast<size_t>(blockFrames) * rs.blockAlign);
                const size_t bytesPerSample = static_cast<size_t>(rs.blockAlign / rs.channels);
                for (const ASIOBufferInfo& bi : m_activeChannels) {
                    if (bi.isInput || bi.channelNum >= rs.channels) continue;
                    const float g0 = curGain[bi.channelNum];
                    const float gt = volume.Gain(bi.channelNum);
                    const float g1 = (std::fabs(gt - g0) < gt * 1e-4f) ? gt : g0 + (gt - g0) * gainCoef;
                    StoreChannel(pData + bytesPerSample * bi.channelNum, rs.blockAlign, rs.kind,
                                 static_cast<const float*>(bi.buffers[dbIdx]), static_cast<size_t>(blockFrames), g0, g1);
                    curGain[bi.channelNum] = g1;
                }
                rs.service->ReleaseBuffer(static_cast<UINT32>(blockFrames), 0);
                framesWritten += static_cast<uint64_t>(blockFrames);
            }
        }

        m_samplePosition.fetch_add(static_cast<uint64_t>(blockFrames), std::memory_order_relaxed);
        ++statBlocks;
    };

    // ------------------------------------------------------------------------
    // 5. LOOP PRINCIPAL
    //    Processa QUANTOS blocos couberem a cada despertar. Se a saída não abrir (ou
    //    for desconectada), um relógio interno mantém a DAW viva e a ENTRADA chegando.
    // ------------------------------------------------------------------------
    long dbIdx          = 0;
    bool resetRequested = false;

    while (m_running.load(std::memory_order_relaxed)) {
        const DWORD waitResult = (renderOk && rs.usesEvent)
            ? WaitForMultipleObjects(2, waitHandles, FALSE, blockMs * 2 + 10)
            : WaitForSingleObject(m_stopEvent, 1);
        if (waitResult == WAIT_OBJECT_0) break; // stop()

        drainCapture();

        bool firstPass = true;
        for (int guard = 0; guard < 256 && m_running.load(std::memory_order_relaxed); ++guard) {
            if (renderOk) {
                UINT32 padding = 0;
                const HRESULT hp = rs.client->GetCurrentPadding(&padding);
                if (FAILED(hp)) {
                    renderOk    = false;
                    clockFrames = 0;
                    QueryPerformanceCounter(&qpcStart);
                    onDeviceLost(L"saida", hp);
                    break;
                }
                if (firstPass && padding == 0 && statBlocks > 0) {
                    // A fila esvaziou (estalo): passa a manter mais áudio adiantado. O acréscimo entra
                    // como silêncio; produzido pela DAW, ele consumiria a folga do FIFO de entrada.
                    ++statOutUnderruns;
                    const long wanted = rs.exclusive ? targetFill + std::max<long>(blockFrames, rs.periodFrames / 2)
                                                     : std::max(targetFill + std::max<long>(blockFrames, rs.periodFrames / 2),
                                                                SharedRenderTarget(blockFrames, rs.periodFrames * 2, rs.periodFrames));
                    const long grown  = std::min<long>(static_cast<long>(rs.bufferFrames), wanted);
                    writeSilence(static_cast<UINT32>(std::max<long>(0, grown - targetFill)));
                    targetFill = grown;
                    padding    = 0;
                    rs.client->GetCurrentPadding(&padding);
                }
                firstPass = false;
                if (static_cast<long>(padding) + blockFrames > targetFill) break;
            } else {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const double elapsed = static_cast<double>(now.QuadPart - qpcStart.QuadPart) * sampleRate /
                                       static_cast<double>(qpcFreq.QuadPart);
                if (elapsed - static_cast<double>(clockFrames) > blockFrames * 8.0) {
                    clockFrames = static_cast<uint64_t>(elapsed) - static_cast<uint64_t>(blockFrames);
                }
                if (static_cast<double>(clockFrames + blockFrames) > elapsed) break;
                clockFrames += static_cast<uint64_t>(blockFrames);
            }

            measureLatency();
            processBlock(dbIdx);
            dbIdx ^= 1;
            drainCapture();
        }

        // Mesa desconectada ou formato alterado no Windows: pede à DAW para reabrir o driver
        if (deviceLost && !resetRequested) {
            resetRequested = true;
            SetError("nanoAudio: dispositivo de audio desconectado.");
            RequestHostReset();
        }
    }

    if (rs.client) rs.client->Stop();
    if (cs.client) cs.client->Stop();
    volume.Stop();

    if (latSamples >= 20) {
        const LatencyEstimate e = EstimateLatency(blockFrames, sampleRate, rs.exclusive);
        SaveDeviceTuning(renderId, kTuneOutExtra, rs.exclusive, avgOut - e.outputFrames, sampleRate);
        if (captureOk) SaveDeviceTuning(captureId, kTuneInExtra, rs.exclusive, avgIn - e.inputFrames, sampleRate);
    }
    if (statOutUnderruns > 0 && renderOk) {
        const long slack = rs.exclusive ? blockFrames * 2 + static_cast<long>(sampleRate * 0.002)
                                        : SharedRenderTarget(blockFrames, 0, rs.periodFrames);
        SaveDeviceTuning(renderId, kTuneBurst, rs.exclusive, targetFill - slack, sampleRate);
    }
    if (fifo.Underruns() > 0 && captureOk) {
        SaveDeviceTuning(captureId, kTuneMargin, cs.exclusive, fifo.MarginFrames(), sampleRate);
    }
    SafeRelease(renderClock);
    NanoLog(L"==== stop(): blocos=%llu | pacotes entrada=%llu (silenciosos=%llu, descontinuidades=%llu) | frames entrada=%llu | "
            L"pico max=%.4f | faltas=%llu | descartados=%llu | margem=%ld | reinicios=%d | esvaziamentos da saida=%llu | fila=%ld | "
            L"latencia medida: entrada=%.0f saida=%.0f frames (%.1f ms ida e volta)",
            static_cast<unsigned long long>(statBlocks), static_cast<unsigned long long>(statPackets),
            static_cast<unsigned long long>(statSilentPackets), static_cast<unsigned long long>(statGaps),
            static_cast<unsigned long long>(statFrames), statMaxPeak,
            static_cast<unsigned long long>(fifo.Underruns()), static_cast<unsigned long long>(fifo.SkippedFrames()),
            captureOk ? fifo.MarginFrames() : 0L, captureRestarts, static_cast<unsigned long long>(statOutUnderruns), targetFill,
            avgIn, avgOut, (avgIn + avgOut) * 1000.0 / sampleRate);
    if (statPackets > 200 && statMaxPeak < 0.0005f) {
        NanoLog(L"AVISO: a entrada abriu mas so chegou SILENCIO. Verifique: privacidade do microfone do Windows, "
                L"nivel/mudo do dispositivo de entrada em Som > Gravacao, e o fader MAIN da mesa.");
    }

    cs.Close();
    rs.Close();
    SafeRelease(pCaptureDevice);
    SafeRelease(pRenderDevice);
    SafeRelease(pEnum);

    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    timeEndPeriod(1);
    if (SUCCEEDED(hrCom)) CoUninitialize();
    return 0;
}

// ============================================================================
// 5. PAINEL DE CONTROLE GRÁFICO WIN32 DO NANOAUDIO (JANELA INTERATIVA REAL)
// ============================================================================

void UniversalWasapiAsioDriver::ApplyNewConfigFromGui(const DriverUserConfig& newCfg) {
    const bool rateChanged   = newCfg.preferredSampleRate != m_config.preferredSampleRate;
    const bool deviceChanged = std::wcscmp(newCfg.outputDeviceId, m_config.outputDeviceId) != 0 ||
                               std::wcscmp(newCfg.inputDeviceId, m_config.inputDeviceId) != 0;
    m_config = newCfg; // o painel já gravou no Registro
    ProbeHardwareCapabilities();

    // Só troca a taxa se o usuário mexeu nela (a DAW pode ter definido outra via setSampleRate)
    const double wantedRate = (newCfg.preferredSampleRate > 0) ? static_cast<double>(newCfg.preferredSampleRate) : m_nativeMixRate;
    if (rateChanged || (deviceChanged && newCfg.preferredSampleRate == 0)) m_sampleRate = wantedRate;

    UpdateLatencyEstimate(m_buffersCreated ? m_activeBufferFrames
                                           : (m_config.customBufferFrames > 0 ? m_config.customBufferFrames : kPreferredBufferFrames));
    if (m_callbacks.asioMessage) m_callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
}

void UniversalWasapiAsioDriver::ShowNativeControlPanelDialog() {
    // A janela dona precisa ser deste processo: ela é desabilitada enquanto o painel está aberto
    HWND hParent = (m_sysHandle && IsWindow(m_sysHandle)) ? m_sysHandle : GetForegroundWindow();
    if (hParent) hParent = GetAncestor(hParent, GA_ROOT);
    DWORD pid = 0;
    if (hParent) GetWindowThreadProcessId(hParent, &pid);
    if (pid != GetCurrentProcessId()) hParent = nullptr;

    DriverUserConfig cfgCopy = m_config;
    if (ShowControlPanelWindow(hParent, &cfgCopy, true /* inDawMode */, kPreferredBufferFrames)) {
        ApplyNewConfigFromGui(cfgCopy);
    }
}

void UniversalWasapiAsioDriver::SetError(const char* msg) {
    std::snprintf(m_errorMsg, sizeof(m_errorMsg), "%s", msg);
}

// ============================================================================
// 6. FÁBRICA DE CLASSES COM (IClassFactory) E REGISTRO AUTOMÁTICO NAS DAWS
// ============================================================================

class AsioDriverClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;
        UniversalWasapiAsioDriver* driver = new (std::nothrow) UniversalWasapiAsioDriver();
        if (!driver) return E_OUTOFMEMORY;
        HRESULT hr = driver->QueryInterface(riid, ppv);
        driver->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) g_dllRefCount.fetch_add(1);
        else       g_dllRefCount.fetch_sub(1);
        return S_OK;
    }
};

static AsioDriverClassFactory g_classFactory;

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid != CLSID_UniversalWasapiAsioDriver) return CLASS_E_CLASSNOTAVAILABLE;
    return g_classFactory.QueryInterface(riid, ppv);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return (g_dllRefCount.load() == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(g_hModule, modulePath, MAX_PATH)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t legacyAsioKeyPath[256], inprocKeyPath[256], clsidKeyPath[256];
    std::swprintf(legacyAsioKeyPath, 256, L"SOFTWARE\\ASIO\\%s", kLegacyDriverName);
    std::swprintf(inprocKeyPath,     256, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", kDriverClsidString);
    std::swprintf(clsidKeyPath,      256, L"SOFTWARE\\Classes\\CLSID\\%s", kDriverClsidString);

    // Remove qualquer registro antigo ou sobrescrita órfã em HKCU que apontava para a DLL antiga
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, legacyAsioKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  legacyAsioKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  inprocKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  clsidKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  kLegacyUserSettingsKey);

    HKEY hClsidKey = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hClsidKey, nullptr) != ERROR_SUCCESS) {
        return E_ACCESSDENIED;
    }
    RegSetValueExW(hClsidKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kDriverName),
                   static_cast<DWORD>((std::wcslen(kDriverName) + 1) * sizeof(wchar_t)));

    HKEY hInprocKey = nullptr;
    if (RegCreateKeyExW(hClsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &hInprocKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hInprocKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(modulePath),
                       static_cast<DWORD>((std::wcslen(modulePath) + 1) * sizeof(wchar_t)));
        const wchar_t* threadingModel = L"Both";
        RegSetValueExW(hInprocKey, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(threadingModel),
                       static_cast<DWORD>((std::wcslen(threadingModel) + 1) * sizeof(wchar_t)));
        RegCloseKey(hInprocKey);
    }
    RegCloseKey(hClsidKey);

    wchar_t asioKeyPath[256];
    std::swprintf(asioKeyPath, 256, L"SOFTWARE\\ASIO\\%s", kDriverName);

    HKEY hAsioKey = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, asioKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hAsioKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hAsioKey, L"CLSID", 0, REG_SZ, reinterpret_cast<const BYTE*>(kDriverClsidString),
                       static_cast<DWORD>((std::wcslen(kDriverClsidString) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hAsioKey, L"Description", 0, REG_SZ, reinterpret_cast<const BYTE*>(kDriverName),
                       static_cast<DWORD>((std::wcslen(kDriverName) + 1) * sizeof(wchar_t)));
        RegCloseKey(hAsioKey);
    }

    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    wchar_t inprocKeyPath[256], clsidKeyPath[256], asioKeyPath[256], legacyAsioKeyPath[256];
    std::swprintf(inprocKeyPath,     256, L"SOFTWARE\\Classes\\CLSID\\%s\\InprocServer32", kDriverClsidString);
    std::swprintf(clsidKeyPath,      256, L"SOFTWARE\\Classes\\CLSID\\%s", kDriverClsidString);
    std::swprintf(asioKeyPath,       256, L"SOFTWARE\\ASIO\\%s", kDriverName);
    std::swprintf(legacyAsioKeyPath, 256, L"SOFTWARE\\ASIO\\%s", kLegacyDriverName);

    RegDeleteKeyW(HKEY_LOCAL_MACHINE, inprocKeyPath);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, clsidKeyPath);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, asioKeyPath);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, legacyAsioKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  inprocKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  clsidKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  asioKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  legacyAsioKeyPath);
    RegDeleteKeyW(HKEY_CURRENT_USER,  kLegacyUserSettingsKey);
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

// Ponto de entrada para abrir o Painel de Controle diretamente pelo Windows (rundll32)
#pragma comment(linker, "/EXPORT:OpenNanoAudioControlPanel")
extern "C" void CALLBACK OpenNanoAudioControlPanel(HWND hwnd, HINSTANCE /*hinst*/, LPSTR /*lpszCmdLine*/, int /*nCmdShow*/) {
    UniversalWasapiAsioDriver* drv = new (std::nothrow) UniversalWasapiAsioDriver();
    if (drv) {
        drv->init(reinterpret_cast<void*>(hwnd));
        drv->controlPanel();
        drv->Release();
    }
}
