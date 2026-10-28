// ============================================================================
// nanoAudioDriver.cpp — nanoAudio ASIO Driver (v2.5)
// Implementação Completa do Driver ASIO Universal nanoAudio (Windows 10/11)
//
// Novidades da v2.5:
// 1. Inclui <initguid.h> antes das headers de dispositivo para garantir que a
//    compilação via cl.exe/CMake nunca falhe com erro de símbolo PKEY_Device_FriendlyName.
// 2. Painel de Controle Gráfico Win32 real (janela interativa com listas dropdown)
//    acessível pelo botão "ASIO Configuration..." do Reaper (mesmo quando sysHandle
//    é nulo ou inválido), permitindo escolher:
//    - Dispositivo de Entrada (Mesa de Som USB / Linha / Microfone)
//    - Dispositivo de Saída (Fones / Caixas / Interface)
//    - Tamanho do Buffer (64, 128, 256, 512, 1024, 2048 samples)
//    - Modo Compartilhado vs. Exclusivo e Medidor de Pico da Entrada!
// 3. Captura de áudio MTA-Safe usando o próprio GetMixFormat() da mesa de som
//    com conversão automática (16/24/32-bit, Mono->Stereo, 44.1<->48 kHz) e
//    loop híbrido (Evento WASAPI + Fallback de Timer de 1 ms) que nunca trava.
// ============================================================================

#include <initguid.h>
#include "nanoAudioDriver.h"
#include <cwctype>
#include <cstdarg>
#include <cwchar>

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
    stop();
    disposeBuffers();
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
    if (m_config.preferredSampleRate > 0) {
        m_sampleRate = static_cast<double>(m_config.preferredSampleRate);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(hr);

    // Mesmo que não haja dispositivo de entrada ativo no momento, inicializa o driver
    // com valores seguros para nunca impedir a abertura do Painel de Controle no Reaper.
    ProbeHardwareCapabilities();

    m_initialized = true;
    return ASIOTrue;
}

void UniversalWasapiAsioDriver::getDriverName(char* name) {
    if (name) std::snprintf(name, 32, "%s", kDriverNameAnsi);
}

long UniversalWasapiAsioDriver::getDriverVersion() {
    return 0x00020005L; // nanoAudio v2.5
}

void UniversalWasapiAsioDriver::getErrorMessage(char* string) {
    if (string) std::snprintf(string, 124, "%s", m_errorMsg);
}

ASIOError UniversalWasapiAsioDriver::getChannels(long* numInputChannels, long* numOutputChannels) {
    if (!numInputChannels || !numOutputChannels) return ASE_InvalidParameter;
    *numInputChannels  = m_config.enableInputCapture ? (m_config.numChannelsIn > 0 ? m_config.numChannelsIn : m_numInputs) : 0;
    *numOutputChannels = (m_config.numChannelsOut > 0) ? m_config.numChannelsOut : m_numOutputs;
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::getLatencies(long* inputLatency, long* outputLatency) {
    if (!inputLatency || !outputLatency) return ASE_InvalidParameter;
    *inputLatency  = m_inputLatencyFrames;
    *outputLatency = m_outputLatencyFrames;
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    if (!minSize || !maxSize || !preferredSize || !granularity) return ASE_InvalidParameter;

    *minSize       = m_hwMinBufferFrames;
    *maxSize       = m_hwMaxBufferFrames;
    long customBuf = m_config.customBufferFrames;
    if (customBuf > 0 && customBuf >= m_hwMinBufferFrames && customBuf <= m_hwMaxBufferFrames) {
        *preferredSize = customBuf;
    } else {
        *preferredSize = m_hwPreferredBufferFrames;
    }
    *granularity   = m_hwGranularityFrames;
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
    m_sampleRate = sampleRate;
    return ASE_OK;
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
    long maxCh = info->isInput
        ? (m_config.enableInputCapture ? (m_config.numChannelsIn > 0 ? m_config.numChannelsIn : m_numInputs) : 0)
        : ((m_config.numChannelsOut > 0) ? m_config.numChannelsOut : m_numOutputs);
    if (info->channel < 0 || info->channel >= maxCh) return ASE_InvalidParameter;

    info->isActive     = m_buffersCreated ? ASIOTrue : ASIOFalse;
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
    if (!bufferInfos || !callbacks || bufferSize <= 0) return ASE_InvalidParameter;
    disposeBuffers();

    m_activeBufferFrames = bufferSize;
    m_callbacks          = *callbacks;
    m_activeChannels.resize(numChannels);
    m_channelMemory.resize(numChannels * 2);

    for (long i = 0; i < numChannels; ++i) {
        m_channelMemory[i * 2].assign(bufferSize, 0.0f);
        m_channelMemory[i * 2 + 1].assign(bufferSize, 0.0f);

        bufferInfos[i].buffers[0] = m_channelMemory[i * 2].data();
        bufferInfos[i].buffers[1] = m_channelMemory[i * 2 + 1].data();
        m_activeChannels[i]       = bufferInfos[i];
    }

    m_inputLatencyFrames  = m_activeBufferFrames;
    m_outputLatencyFrames = m_activeBufferFrames + m_engineOverheadFrames;
    m_buffersCreated      = true;

    long nIn = 0, nOut = 0;
    for (long i = 0; i < numChannels; ++i) (bufferInfos[i].isInput ? nIn : nOut)++;
    NanoLog(L"createBuffers(): bloco=%ld | canais de ENTRADA ativados pelo Reaper=%ld | saida=%ld | taxa=%.0f Hz",
            bufferSize, nIn, nOut, m_sampleRate);
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::disposeBuffers() {
    stop();
    m_activeChannels.clear();
    m_channelMemory.clear();
    m_buffersCreated = false;
    return ASE_OK;
}

ASIOError UniversalWasapiAsioDriver::start() {
    if (!m_buffersCreated || m_running.load()) return ASE_InvalidMode;

    m_stopEvent   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_stopEvent || !m_renderEvent) {
        SetError("nanoAudio: Falha ao criar eventos de sincronizacao.");
        return ASE_HWMalfunction;
    }

    m_samplePosition.store(0);
    m_running.store(true);

    m_audioThread = CreateThread(nullptr, 0, &UniversalWasapiAsioDriver::AudioThreadProc, this, 0, nullptr);
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
    if (!m_running.load()) return ASE_OK;
    m_running.store(false);

    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_audioThread) {
        WaitForSingleObject(m_audioThread, 3000);
        CloseHandle(m_audioThread);
        m_audioThread = nullptr;
    }
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

ASIOError UniversalWasapiAsioDriver::outputReady() {
    return ASE_OK;
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

    IMMDevice* pRender  = ResolveEndpoint(pEnum, eRender,  m_config.outputDeviceId, false, m_renderDeviceName, 128);
    IMMDevice* pCapture = ResolveEndpoint(pEnum, eCapture, m_config.inputDeviceId,  true,  m_captureDeviceName, 128);

    if (pRender) {
        IAudioClient3* pClient3 = nullptr;
        if (SUCCEEDED(pRender->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pClient3))) && pClient3) {
            WAVEFORMATEX* pMixFormat = nullptr;
            if (SUCCEEDED(pClient3->GetMixFormat(&pMixFormat)) && pMixFormat) {
                m_nativeMixRate = static_cast<double>(pMixFormat->nSamplesPerSec);
                // Não sobrescreve a taxa que o Reaper já definiu via setSampleRate()
                if (!m_buffersCreated) {
                    m_sampleRate = (m_config.preferredSampleRate > 0)
                                       ? static_cast<double>(m_config.preferredSampleRate)
                                       : m_nativeMixRate;
                }
                m_numOutputs    = std::max<long>(2, pMixFormat->nChannels);

                UINT32 defaultPeriod = 0, fundamentalPeriod = 0, minPeriod = 0, maxPeriod = 0;
                if (SUCCEEDED(pClient3->GetSharedModeEnginePeriod(
                        pMixFormat, &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod))) {
                    // Com o Ring Buffer FIFO desacoplado, qualquer tamanho de bloco ASIO (32..2048)
                    // é suportado independentemente do período interno do engine WASAPI.
                    m_hwMinBufferFrames       = 32;
                    m_hwPreferredBufferFrames = 256;
                    m_hwMaxBufferFrames       = 2048;
                    m_hwGranularityFrames     = 32;
                    m_engineOverheadFrames    = static_cast<long>(minPeriod);
                }
                CoTaskMemFree(pMixFormat);
            }
            pClient3->Release();
        }
        pRender->Release();
    }

    if (pCapture) {
        IAudioClient* pCapClient = nullptr;
        if (SUCCEEDED(pCapture->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pCapClient))) && pCapClient) {
            WAVEFORMATEX* pCapMix = nullptr;
            if (SUCCEEDED(pCapClient->GetMixFormat(&pCapMix)) && pCapMix) {
                m_hwCaptureChannels = std::max<long>(1, pCapMix->nChannels);
                m_nativeCaptureRate = static_cast<double>(pCapMix->nSamplesPerSec);
                m_numInputs         = std::max<long>(2, m_hwCaptureChannels);
                CoTaskMemFree(pCapMix);
            }
            pCapClient->Release();
        }
        pCapture->Release();
    }

    pEnum->Release();

    m_inputLatencyFrames  = (m_config.customBufferFrames > 0) ? m_config.customBufferFrames : m_hwPreferredBufferFrames;
    m_outputLatencyFrames = m_inputLatencyFrames + m_engineOverheadFrames;
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

static void SyncEndpointSampleRates(IMMDevice* pRender, IMMDevice* pCapture) {
    if (!pRender || !pCapture) return;
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

DWORD WINAPI UniversalWasapiAsioDriver::AudioThreadProc(LPVOID param) {
    return static_cast<UniversalWasapiAsioDriver*>(param)->RunRealTimeAudioLoop();
}

DWORD UniversalWasapiAsioDriver::RunRealTimeAudioLoop() {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);

    DWORD mmcssTaskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    if (hMmcss) {
        AvSetMmThreadPriority(hMmcss, AVRT_PRIORITY_CRITICAL);
    }

    // Cópia local dos parâmetros: o Painel de Controle pode alterar m_numInputs / m_config
    // em outra thread enquanto o áudio roda (evita estouro de buffer por condição de corrida).
    const long   blockFrames = std::max<long>(16, m_activeBufferFrames);
    const double sampleRate  = (m_sampleRate > 1000.0) ? m_sampleRate : 48000.0;
    const long   numIn       = std::max<long>(1, m_numInputs);
    const bool   wantInput   = m_config.enableInputCapture;
    wchar_t inputId[256]  = {};
    wchar_t outputId[256] = {};
    std::wcsncpy(inputId,  m_config.inputDeviceId,  255);
    std::wcsncpy(outputId, m_config.outputDeviceId, 255);

    NanoLog(L"==== start(): bloco=%ld frames | taxa ASIO=%.0f Hz | entradas ASIO=%ld | captura=%ls",
            blockFrames, sampleRate, numIn, wantInput ? L"LIGADA" : L"DESLIGADA");

    // Cria o enumerador e os endpoints dentro da thread MTA (evita falha de marshaling COM STA->MTA)
    IMMDeviceEnumerator* pEnum           = nullptr;
    IMMDevice*           pRenderDevice   = nullptr;
    IMMDevice*           pCaptureDevice  = nullptr;
    IAudioClient*        pRenderClient   = nullptr;
    IAudioRenderClient*  pRenderService  = nullptr;
    IAudioClient*        pCaptureClient  = nullptr;
    IAudioCaptureClient* pCaptureService = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum)
    );
    if (SUCCEEDED(hr) && pEnum) {
        pRenderDevice  = ResolveEndpoint(pEnum, eRender,  outputId, false, m_renderDeviceName, 128);
        pCaptureDevice = ResolveEndpoint(pEnum, eCapture, inputId,  true,  m_captureDeviceName, 128);
        SyncEndpointSampleRates(pRenderDevice, pCaptureDevice);
    }
    NanoLog(L"Saida escolhida: %ls | Entrada escolhida: %ls",
            pRenderDevice  ? m_renderDeviceName  : L"(nenhuma)",
            pCaptureDevice ? m_captureDeviceName : L"(nenhuma)");

    // ------------------------------------------------------------------------
    // 1. STREAM DE SAÍDA (Render) — float 32-bit + AutoConvertPCM
    // ------------------------------------------------------------------------
    long    renderChannels   = 2;
    bool    renderUsesEvent  = false;
    bool    renderOk         = false;
    UINT32  renderBufferSize = 0;
    HRESULT hrRender         = E_FAIL;

    if (pRenderDevice && SUCCEEDED(pRenderDevice->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pRenderClient))) && pRenderClient) {

        WAVEFORMATEX* pMix = nullptr;
        if (SUCCEEDED(pRenderClient->GetMixFormat(&pMix)) && pMix) {
            renderChannels = std::max<long>(1, pMix->nChannels);
            CoTaskMemFree(pMix);
        }

        WAVEFORMATEX wfxOut = {};
        wfxOut.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        wfxOut.nChannels       = static_cast<WORD>(renderChannels);
        wfxOut.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
        wfxOut.wBitsPerSample  = 32;
        wfxOut.nBlockAlign     = static_cast<WORD>((wfxOut.nChannels * 32) / 8);
        wfxOut.nAvgBytesPerSec = wfxOut.nSamplesPerSec * wfxOut.nBlockAlign;
        wfxOut.cbSize          = 0;

        const REFERENCE_TIME renderDur = static_cast<REFERENCE_TIME>(
            (10000000.0 * std::max<long>(blockFrames * 3, 1024)) / sampleRate + 0.5);

        hrRender = pRenderClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            renderDur, 0, &wfxOut, nullptr);

        if (SUCCEEDED(hrRender)) {
            renderUsesEvent = SUCCEEDED(pRenderClient->SetEventHandle(m_renderEvent));
        } else {
            // Um IAudioClient que falhou no Initialize não deve ser reutilizado: cria um novo.
            pRenderClient->Release();
            pRenderClient = nullptr;
            if (SUCCEEDED(pRenderDevice->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pRenderClient))) && pRenderClient) {
                hrRender = pRenderClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                    renderDur, 0, &wfxOut, nullptr);
            }
        }

        if (SUCCEEDED(hrRender) && pRenderClient &&
            SUCCEEDED(pRenderClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pRenderService))) &&
            pRenderService &&
            SUCCEEDED(pRenderClient->GetBufferSize(&renderBufferSize)) &&
            renderBufferSize >= static_cast<UINT32>(blockFrames)) {
            renderOk = true;
        }
    }
    NanoLog(L"Saida WASAPI: hr=0x%08lX | canais=%ld | buffer=%u frames | evento=%ls | OK=%ls",
            static_cast<unsigned long>(hrRender), renderChannels, renderBufferSize,
            renderUsesEvent ? L"sim" : L"nao", renderOk ? L"SIM" : L"NAO (usando relogio interno)");

    // ------------------------------------------------------------------------
    // 2. STREAM DE CAPTURA (Mesa de Som USB / Microfone)
    // ------------------------------------------------------------------------
    long    capChannels       = 2;
    double  capRate           = sampleRate;
    bool    capIsFloat        = true;
    UINT    capBytesPerSample = 4;
    UINT    capBlockAlign     = 8;
    HRESULT hrCapture         = E_FAIL;
    m_captureStreamActive.store(false);

    if (wantInput && pCaptureDevice) {
        WAVEFORMATEX* pCapMix = nullptr;
        {
            IAudioClient* pProbe = nullptr;
            if (SUCCEEDED(pCaptureDevice->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pProbe))) && pProbe) {
                pProbe->GetMixFormat(&pCapMix);
                pProbe->Release();
            }
        }
        if (pCapMix) {
            capChannels = std::max<long>(1, pCapMix->nChannels);
            NanoLog(L"Formato nativo da entrada: %lu Hz | %u canais | %u bits | tag=0x%04X",
                    static_cast<unsigned long>(pCapMix->nSamplesPerSec), pCapMix->nChannels,
                    pCapMix->wBitsPerSample, pCapMix->wFormatTag);
        }

        const REFERENCE_TIME capDur = static_cast<REFERENCE_TIME>(
            (10000000.0 * std::max<long>(blockFrames * 4, 2048)) / sampleRate + 0.5);

        // Cada tentativa usa um IAudioClient novo (um cliente que falhou não pode ser reinicializado)
        auto tryInitCapture = [&](const WAVEFORMATEX* fmt, DWORD flags) -> HRESULT {
            if (pCaptureClient) { pCaptureClient->Release(); pCaptureClient = nullptr; }
            HRESULT h = pCaptureDevice->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pCaptureClient));
            if (FAILED(h) || !pCaptureClient) return FAILED(h) ? h : E_FAIL;
            return pCaptureClient->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, capDur, 0, fmt, nullptr);
        };

        // Tentativa A: float 32-bit na taxa do ASIO, com conversão automática do Windows
        WAVEFORMATEX wfxIn = {};
        wfxIn.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
        wfxIn.nChannels       = static_cast<WORD>(capChannels);
        wfxIn.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
        wfxIn.wBitsPerSample  = 32;
        wfxIn.nBlockAlign     = static_cast<WORD>(wfxIn.nChannels * 4);
        wfxIn.nAvgBytesPerSec = wfxIn.nSamplesPerSec * wfxIn.nBlockAlign;
        wfxIn.cbSize          = 0;

        const DWORD autoFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hrCapture = tryInitCapture(&wfxIn, autoFlags);
        if (FAILED(hrCapture) && wfxIn.nChannels != 2) {
            wfxIn.nChannels       = 2;
            wfxIn.nBlockAlign     = 8;
            wfxIn.nAvgBytesPerSec = wfxIn.nSamplesPerSec * 8;
            hrCapture = tryInitCapture(&wfxIn, autoFlags);
        }
        if (SUCCEEDED(hrCapture)) {
            capChannels       = wfxIn.nChannels;
            capRate           = sampleRate;
            capIsFloat        = true;
            capBytesPerSample = 4;
            capBlockAlign     = wfxIn.nBlockAlign;
        }

        // Tentativa B: formato nativo da mesa (sem conversão do Windows) + conversor de taxa interno
        if (FAILED(hrCapture) && pCapMix) {
            NanoLog(L"Tentativa A falhou (hr=0x%08lX). Abrindo a entrada no formato nativo...",
                    static_cast<unsigned long>(hrCapture));
            hrCapture = tryInitCapture(pCapMix, 0);
            if (SUCCEEDED(hrCapture)) {
                capChannels       = std::max<long>(1, pCapMix->nChannels);
                capRate           = static_cast<double>(pCapMix->nSamplesPerSec);
                capBytesPerSample = std::max<UINT>(1, pCapMix->wBitsPerSample / 8);
                capBlockAlign     = pCapMix->nBlockAlign;
                capIsFloat        = (pCapMix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                                    (pCapMix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                     reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pCapMix)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
            }
        }
        if (pCapMix) CoTaskMemFree(pCapMix);

        // Tentativa C: Modo Exclusivo 16-bit PCM (caso o driver WDM USB 1.1 como Behringer 2.8.40
        // recuse o modo compartilhado com 0x8889000F)
        if (FAILED(hrCapture)) {
            const DWORD ratesToTry[2] = { static_cast<DWORD>(sampleRate), (sampleRate == 44100.0 ? 48000u : 44100u) };
            for (int r = 0; r < 2 && FAILED(hrCapture); ++r) {
                WAVEFORMATEX pcm16 = {};
                pcm16.wFormatTag      = WAVE_FORMAT_PCM;
                pcm16.nChannels       = 2;
                pcm16.nSamplesPerSec  = ratesToTry[r];
                pcm16.wBitsPerSample  = 16;
                pcm16.nBlockAlign     = 4;
                pcm16.nAvgBytesPerSec = ratesToTry[r] * 4;
                if (pCaptureClient) { pCaptureClient->Release(); pCaptureClient = nullptr; }
                if (SUCCEEDED(pCaptureDevice->Activate(
                        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pCaptureClient))) && pCaptureClient) {
                    hrCapture = pCaptureClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, capDur, capDur, &pcm16, nullptr);
                    if (SUCCEEDED(hrCapture)) {
                        capChannels       = 2;
                        capRate           = static_cast<double>(ratesToTry[r]);
                        capIsFloat        = false;
                        capBytesPerSample = 2;
                        capBlockAlign     = 4;
                        NanoLog(L"Tentativa C (Exclusivo 16-bit %lu Hz) abriu com sucesso!", ratesToTry[r]);
                    }
                }
            }
        }

        if (SUCCEEDED(hrCapture) && pCaptureClient &&
            SUCCEEDED(pCaptureClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&pCaptureService))) &&
            pCaptureService &&
            SUCCEEDED(pCaptureClient->Start())) {
            m_captureStreamActive.store(true);
        } else {
            if (SUCCEEDED(hrCapture)) hrCapture = E_FAIL;
            if (pCaptureService) { pCaptureService->Release(); pCaptureService = nullptr; }
            SetError("nanoAudio: falha ao abrir a entrada (mesa de som).");
        }

        NanoLog(L"Entrada WASAPI: hr=0x%08lX | canais=%ld | taxa=%.0f Hz | float=%ls | ATIVA=%ls",
                static_cast<unsigned long>(hrCapture), capChannels, capRate,
                capIsFloat ? L"sim" : L"nao", m_captureStreamActive.load() ? L"SIM" : L"NAO");
        if (hrCapture == E_ACCESSDENIED) {
            NanoLog(L"ACESSO NEGADO AO MICROFONE: ative em Configuracoes > Privacidade > Microfone "
                    L"a opcao 'Permitir que aplicativos da area de trabalho acessem o microfone'.");
        }
    }

    // ------------------------------------------------------------------------
    // 3. RING BUFFER FIFO + CONVERSOR DE TAXA (Mesa USB -> blocos do Reaper)
    // ------------------------------------------------------------------------
    const size_t fifoCap = std::max<size_t>(static_cast<size_t>(blockFrames) * 16, 16384);
    std::vector<float> fifo(fifoCap * static_cast<size_t>(numIn), 0.0f);
    size_t fifoW = 0, fifoR = 0, fifoAvail = 0;

    // Pré-carga: 1 bloco + ~12 ms (a mesa entrega pacotes de ~10 ms) para nunca faltar amostra.
    // Limite: se o FIFO acumular demais (relógios diferentes de entrada e saída), descarta o excesso
    // para a latência não crescer indefinidamente.
    const size_t primeFrames = static_cast<size_t>(blockFrames) + static_cast<size_t>(sampleRate * 0.012);
    const size_t maxFill     = primeFrames + static_cast<size_t>(blockFrames) * 2 + static_cast<size_t>(sampleRate * 0.020);
    bool primed = false;

    const bool   needResample = std::fabs(capRate - sampleRate) > 0.5;
    const double rsStep       = capRate / sampleRate; // frames de entrada por frame de saída
    double       rsPhase      = 0.0;
    std::vector<float> rsPrev(numIn, 0.0f), rsOut(numIn, 0.0f), curFrame(numIn, 0.0f);

    // Estatísticas para o log de diagnóstico
    uint64_t statPackets = 0, statSilentPackets = 0, statFrames = 0;
    uint64_t statUnderruns = 0, statDropped = 0, statBlocks = 0;
    float    statMaxPeak = 0.0f;
    bool     loggedFirstSignal = false;

    auto fifoPush = [&](const float* frame) {
        float* dst = &fifo[fifoW * static_cast<size_t>(numIn)];
        for (long c = 0; c < numIn; ++c) dst[c] = frame[c];
        fifoW = (fifoW + 1) % fifoCap;
        if (fifoAvail < fifoCap) {
            ++fifoAvail;
        } else {
            fifoR = (fifoR + 1) % fifoCap;
            ++statDropped;
        }
    };

    auto pushCaptured = [&](const float* frame) {
        if (!needResample) { fifoPush(frame); return; }
        // Interpolação linear entre o frame anterior e o atual
        while (rsPhase < 1.0) {
            const float t = static_cast<float>(rsPhase);
            for (long c = 0; c < numIn; ++c) rsOut[c] = rsPrev[c] + (frame[c] - rsPrev[c]) * t;
            fifoPush(rsOut.data());
            rsPhase += rsStep;
        }
        rsPhase -= 1.0;
        for (long c = 0; c < numIn; ++c) rsPrev[c] = frame[c];
    };

    auto readSample = [&](const BYTE* base, UINT32 frame, long ch) -> float {
        const BYTE* p = base + static_cast<size_t>(frame) * capBlockAlign + static_cast<size_t>(ch) * capBytesPerSample;
        if (capIsFloat) { float v; std::memcpy(&v, p, 4); return v; }
        switch (capBytesPerSample) {
            case 2: { int16_t v; std::memcpy(&v, p, 2); return static_cast<float>(v) / 32768.0f; }
            case 3: {
                const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) |
                                                       (static_cast<uint32_t>(p[1]) << 16) |
                                                       (static_cast<uint32_t>(p[2]) << 24));
                return static_cast<float>(v >> 8) / 8388608.0f;
            }
            case 4: { int32_t v; std::memcpy(&v, p, 4); return static_cast<float>(v) / 2147483648.0f; }
            default: return 0.0f;
        }
    };

    // Lê TODOS os pacotes disponíveis da mesa e coloca no FIFO
    auto drainCapture = [&]() {
        if (!pCaptureService) return;
        UINT32 pkt = 0;
        float  peak = 0.0f;
        bool   gotData = false;
        while (SUCCEEDED(pCaptureService->GetNextPacketSize(&pkt)) && pkt > 0) {
            BYTE*  pCap  = nullptr;
            UINT32 n     = 0;
            DWORD  flags = 0;
            if (FAILED(pCaptureService->GetBuffer(&pCap, &n, &flags, nullptr, nullptr))) break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !pCap;
            ++statPackets;
            if (silent) ++statSilentPackets;
            statFrames += n;

            for (UINT32 f = 0; f < n; ++f) {
                for (long c = 0; c < numIn; ++c) {
                    if (silent) {
                        curFrame[c] = 0.0f;
                    } else {
                        // Mesa mono -> duplica o canal 1; mesa estéreo -> In1=L, In2=R
                        const long srcCh = std::min<long>(c, capChannels - 1);
                        const float s = readSample(pCap, f, srcCh);
                        curFrame[c] = s;
                        peak = std::max(peak, std::fabs(s));
                    }
                }
                pushCaptured(curFrame.data());
            }
            pCaptureService->ReleaseBuffer(n);
            gotData = true;
        }
        if (gotData) {
            m_inputPeakMeter.store(peak, std::memory_order_relaxed);
            statMaxPeak = std::max(statMaxPeak, peak);
            if (!loggedFirstSignal && peak > 0.0005f) {
                loggedFirstSignal = true;
                NanoLog(L"Primeiro sinal real recebido da entrada (pico=%.4f).", peak);
            }
        }
    };

    // Processa UM bloco ASIO: entrada -> bufferSwitch do Reaper -> saída
    auto processBlock = [&](long dbIdx) {
        if (!primed && fifoAvail >= primeFrames) primed = true;
        if (primed && fifoAvail < static_cast<size_t>(blockFrames)) {
            primed = false;
            ++statUnderruns;
        }
        if (primed && fifoAvail > maxFill) {
            const size_t drop = fifoAvail - primeFrames;
            fifoR = (fifoR + drop) % fifoCap;
            fifoAvail -= drop;
            statDropped += drop;
        }

        for (long f = 0; f < blockFrames; ++f) {
            const float* fr = primed ? &fifo[fifoR * static_cast<size_t>(numIn)] : nullptr;
            for (size_t chIdx = 0; chIdx < m_activeChannels.size(); ++chIdx) {
                const ASIOBufferInfo& bi = m_activeChannels[chIdx];
                if (bi.isInput) {
                    float* dst = static_cast<float*>(bi.buffers[dbIdx]);
                    const long hwCh = std::clamp<long>(bi.channelNum, 0, numIn - 1);
                    dst[f] = fr ? fr[hwCh] : 0.0f;
                }
            }
            if (primed) {
                fifoR = (fifoR + 1) % fifoCap;
                --fifoAvail;
            }
        }

        LARGE_INTEGER qpc, freq;
        QueryPerformanceCounter(&qpc);
        QueryPerformanceFrequency(&freq);
        const uint64_t nowNs = static_cast<uint64_t>((static_cast<double>(qpc.QuadPart) * 1e9) / freq.QuadPart);
        m_systemTimeNs.store(nowNs, std::memory_order_relaxed);

        if (m_callbacks.bufferSwitchTimeInfo) {
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
        } else if (m_callbacks.bufferSwitch) {
            m_callbacks.bufferSwitch(dbIdx, ASIOTrue);
        }

        if (renderOk) {
            BYTE* pData = nullptr;
            const UINT32 framesToWrite = static_cast<UINT32>(blockFrames);
            if (SUCCEEDED(pRenderService->GetBuffer(framesToWrite, &pData)) && pData) {
                float* dst = reinterpret_cast<float*>(pData);
                std::memset(dst, 0, static_cast<size_t>(framesToWrite) * renderChannels * sizeof(float));
                for (size_t chIdx = 0; chIdx < m_activeChannels.size(); ++chIdx) {
                    const ASIOBufferInfo& bi = m_activeChannels[chIdx];
                    if (!bi.isInput && bi.channelNum >= 0 && bi.channelNum < renderChannels) {
                        const float* src = static_cast<const float*>(bi.buffers[dbIdx]);
                        for (UINT32 f = 0; f < framesToWrite; ++f) {
                            dst[static_cast<size_t>(f) * renderChannels + bi.channelNum] = src[f];
                        }
                    }
                }
                pRenderService->ReleaseBuffer(framesToWrite, 0);
            }
        }

        m_samplePosition.fetch_add(static_cast<uint64_t>(blockFrames), std::memory_order_relaxed);
        ++statBlocks;
    };

    // ------------------------------------------------------------------------
    // 4. LOOP PRINCIPAL
    //    CORREÇÃO: processa QUANTOS blocos couberem a cada despertar. Antes era
    //    apenas 1 bloco por evento do WASAPI (~10 ms), o que fazia o Reaper receber
    //    menos amostras do que a mesa enviava (áudio picotado / entrada "sumindo").
    //    Se a saída não abrir, o driver usa um relógio interno e a ENTRADA continua
    //    chegando ao Reaper normalmente.
    // ------------------------------------------------------------------------
    if (renderOk) {
        BYTE* pData = nullptr;
        if (SUCCEEDED(pRenderService->GetBuffer(renderBufferSize, &pData))) {
            pRenderService->ReleaseBuffer(renderBufferSize, AUDCLNT_BUFFERFLAGS_SILENT);
        }
        if (FAILED(pRenderClient->Start())) {
            renderOk = false;
            NanoLog(L"Falha ao iniciar a saida; usando relogio interno.");
        }
    }

    LARGE_INTEGER qpcFreq, qpcStart;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);
    uint64_t clockFrames = 0;
    long     dbIdx       = 0;
    bool     deviceLost  = false;
    const DWORD blockMs  = std::max<DWORD>(1, static_cast<DWORD>((1000.0 * blockFrames) / sampleRate));
    HANDLE waitHandles[2] = { m_stopEvent, m_renderEvent };

    while (m_running.load(std::memory_order_relaxed)) {
        DWORD waitResult;
        if (renderOk && renderUsesEvent) {
            waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, blockMs * 2 + 10);
        } else {
            waitResult = WaitForSingleObject(m_stopEvent, 1);
        }
        if (waitResult == WAIT_OBJECT_0) break; // stop()

        drainCapture();

        for (int guard = 0; guard < 64 && m_running.load(std::memory_order_relaxed); ++guard) {
            if (renderOk) {
                UINT32 padding = 0;
                const HRESULT hp = pRenderClient->GetCurrentPadding(&padding);
                if (FAILED(hp)) {
                    deviceLost = true;
                    NanoLog(L"Dispositivo de saida perdido (hr=0x%08lX).", static_cast<unsigned long>(hp));
                    break;
                }
                const UINT32 freeFrames = (renderBufferSize > padding) ? (renderBufferSize - padding) : 0;
                if (freeFrames < static_cast<UINT32>(blockFrames)) break;
            } else {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const double elapsed = static_cast<double>(now.QuadPart - qpcStart.QuadPart) * sampleRate /
                                       static_cast<double>(qpcFreq.QuadPart);
                if (elapsed - static_cast<double>(clockFrames) > blockFrames * 8.0) {
                    clockFrames = static_cast<uint64_t>(elapsed) - static_cast<uint64_t>(blockFrames);
                }
                if (static_cast<double>(clockFrames + blockFrames) > elapsed) break;
            }

            processBlock(dbIdx);
            dbIdx ^= 1;
            clockFrames += static_cast<uint64_t>(blockFrames);
            drainCapture();
        }
        if (deviceLost) {
            SetError("nanoAudio: dispositivo de saida desconectado.");
            break;
        }
    }

    if (renderOk) pRenderClient->Stop();

    NanoLog(L"==== stop(): blocos=%llu | pacotes entrada=%llu (silenciosos=%llu) | frames entrada=%llu | "
            L"pico max=%.4f | faltas=%llu | descartados=%llu",
            static_cast<unsigned long long>(statBlocks), static_cast<unsigned long long>(statPackets),
            static_cast<unsigned long long>(statSilentPackets), static_cast<unsigned long long>(statFrames),
            statMaxPeak, static_cast<unsigned long long>(statUnderruns), static_cast<unsigned long long>(statDropped));
    if (m_captureStreamActive.load() && statPackets > 0 && statMaxPeak < 0.0005f) {
        NanoLog(L"AVISO: a entrada abriu mas so chegou SILENCIO. Verifique: privacidade do microfone do Windows, "
                L"nivel/mudo do 'Microfone (USB Audio CODEC)' em Som > Gravacao, e o fader MAIN da mesa.");
    }

    m_captureStreamActive.store(false);
    if (pCaptureClient)  pCaptureClient->Stop();
    if (pCaptureService) pCaptureService->Release();
    if (pCaptureClient)  pCaptureClient->Release();
    if (pRenderService)  pRenderService->Release();
    if (pRenderClient)   pRenderClient->Release();
    if (pCaptureDevice)  pCaptureDevice->Release();
    if (pRenderDevice)   pRenderDevice->Release();
    if (pEnum)           pEnum->Release();

    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    timeEndPeriod(1);
    if (SUCCEEDED(hrCom)) CoUninitialize();
    return 0;
}

// ============================================================================
// 5. PAINEL DE CONTROLE GRÁFICO WIN32 DO NANOAUDIO (JANELA INTERATIVA REAL)
// ============================================================================

void UniversalWasapiAsioDriver::ApplyNewConfigFromGui(const DriverUserConfig& newCfg) {
    m_config = newCfg;
    SaveUserConfig(m_config);
    ProbeHardwareCapabilities();
    if (m_callbacks.asioMessage) {
        m_callbacks.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }
}

void UniversalWasapiAsioDriver::ShowNativeControlPanelDialog() {
    HWND hParent = (m_sysHandle && IsWindow(m_sysHandle)) ? m_sysHandle : GetForegroundWindow();
    if (hParent && !IsWindow(hParent)) hParent = nullptr;

    DriverUserConfig cfgCopy = m_config;
    bool applied = ShowControlPanelWindow(
        hParent,
        &cfgCopy,
        true, // inDawMode
        m_nativeMixRate > 0 ? m_nativeMixRate : 48000.0,
        m_hwMinBufferFrames,
        m_hwPreferredBufferFrames,
        m_hwMaxBufferFrames
    );

    if (applied) {
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
