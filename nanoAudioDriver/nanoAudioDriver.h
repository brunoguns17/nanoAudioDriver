// ============================================================================
// nanoAudioDriver.h — nanoAudio ASIO Driver (v2.5)
// Header do Driver ASIO Universal nanoAudio de Baixa Latência para Windows 10/11
// Inclui Painel de Controle Gráfico Win32 (Seleção de Mesa de Som, Saída e Buffer),
// Ring Buffer FIFO de Captura e Conversão Automática de Qualquer Formato/Taxa.
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
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiosessiontypes.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "nanoAudioConfig.h"

// CLSID exclusivo do Driver nanoAudio: {8E6A2A41-5C3B-4F92-9A1D-7E3F9B02C4D8}
static const CLSID CLSID_UniversalWasapiAsioDriver = {
    0x8e6a2a41, 0x5c3b, 0x4f92, {0x9a, 0x1d, 0x7e, 0x3f, 0x9b, 0x02, 0xc4, 0xd8}
};

static const wchar_t* kDriverClsidString = L"{8E6A2A41-5C3B-4F92-9A1D-7E3F9B02C4D8}";
static const wchar_t* kDriverName        = L"nanoAudio";
static const char*    kDriverNameAnsi    = "nanoAudio";
static const wchar_t* kLegacyDriverName  = L"Universal WASAPI Low-Latency ASIO";

// ============================================================================
// 1. DEFINIÇÕES BINÁRIAS DO PROTOCOLO STEINBERG ASIO 2.3 (ABI-Compatible)
// ============================================================================

typedef double ASIOSampleRate;
typedef long   ASIOBool;
typedef long   ASIOError;
typedef long   ASIOSampleType;

enum {
    ASIOFalse = 0,
    ASIOTrue  = 1
};

enum {
    ASE_OK               = 0,
    ASE_SUCCESS          = 0x3f4847a0,
    ASE_NotPresent       = -1000,
    ASE_HWMalfunction    = ASE_NotPresent + 1,
    ASE_InvalidParameter = ASE_NotPresent + 2,
    ASE_InvalidMode      = ASE_NotPresent + 3,
    ASE_SPNotAdvancing   = ASE_NotPresent + 4,
    ASE_NoClock          = ASE_NotPresent + 5,
    ASE_NoMemory         = ASE_NotPresent + 6
};

enum {
    ASIOSTInt16LSB   = 16,
    ASIOSTInt24LSB   = 17,
    ASIOSTInt32LSB   = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20
};

enum {
    kAsioSelectorSupported    = 1,
    kAsioEngineVersion        = 2,
    kAsioResetRequest         = 3,
    kAsioBufferSizeChange     = 4,
    kAsioResyncRequest        = 5,
    kAsioLatenciesChanged     = 6,
    kAsioSupportsTimeInfo     = 7,
    kAsioSupportsTimeCode     = 8,
    kAsioMMCCommand           = 9,
    kAsioSupportsInputMonitor = 10,
    kAsioSupportsInputGain    = 11,
    kAsioSupportsInputMeter   = 12,
    kAsioSupportsOutputGain   = 13,
    kAsioSupportsOutputMeter  = 14,
    kAsioOverload             = 15
};

#pragma pack(push, 4)

struct ASIOSamples {
    unsigned long hi;
    unsigned long lo;
};

struct ASIOTimeStamp {
    unsigned long hi;
    unsigned long lo;
};

struct AsioTimeInfo {
    double         speed;
    ASIOTimeStamp  systemTime;
    ASIOSamples    samplePosition;
    ASIOSampleRate sampleRate;
    unsigned long  flags;
    char           reserved[12];
};

struct ASIOTimeCode {
    double        speed;
    ASIOSamples   timeCodeSamples;
    unsigned long flags;
    char          future[64];
};

struct ASIOTime {
    long         reserved[4];
    AsioTimeInfo timeInfo;
    ASIOTimeCode timeCode;
};

enum {
    kSystemTimeValid     = 1 << 0,
    kSamplePositionValid = 1 << 1,
    kSampleRateValid     = 1 << 2
};

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

struct ASIOClockSource {
    long     index;
    long     associatedChannel;
    long     associatedGroup;
    ASIOBool isCurrentSource;
    char     name[32];
};

struct ASIOChannelInfo {
    long           channel;
    ASIOBool       isInput;
    ASIOBool       isActive;
    long           channelGroup;
    ASIOSampleType type;
    char           name[32];
};

struct ASIOBufferInfo {
    ASIOBool isInput;
    long     channelNum;
    void*    buffers[2]; // Double buffer: buffers[0] = A, buffers[1] = B
};

#pragma pack(pop)

// Interface COM IASIO (VTable idêntica ao Steinberg ASIO SDK)
interface IASIO : public IUnknown {
    virtual ASIOBool  init(void* sysHandle) = 0;
    virtual void      getDriverName(char* name) = 0;
    virtual long      getDriverVersion() = 0;
    virtual void      getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
    virtual ASIOError setClockSource(long reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(long selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

// ============================================================================
// 2. ESTRUTURAS DE DISPOSITIVO E CONFIGURAÇÃO DO USUÁRIO
// ============================================================================

struct AudioEndpointItem {
    std::wstring id;
    std::wstring name;
};

class UniversalWasapiAsioDriver final : public IASIO {
public:
    UniversalWasapiAsioDriver();
    ~UniversalWasapiAsioDriver();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IASIO
    ASIOBool  init(void* sysHandle) override;
    void      getDriverName(char* name) override;
    long      getDriverVersion() override;
    void      getErrorMessage(char* string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

    // Métodos auxiliares para o Painel de Controle Win32
    DriverUserConfig& GetConfig() { return m_config; }
    void ApplyNewConfigFromGui(const DriverUserConfig& newCfg);
    const wchar_t* GetActiveCaptureName() const { return m_captureDeviceName; }
    const wchar_t* GetActiveRenderName() const  { return m_renderDeviceName; }
    bool IsCaptureStreaming() const             { return m_captureStreamActive.load(); }
    float GetInputPeakLevel() const             { return m_inputPeakMeter.load(); }
    double GetCurrentSampleRate() const         { return m_sampleRate; }

private:
    bool  ProbeHardwareCapabilities();
    static IMMDevice* ResolveEndpoint(IMMDeviceEnumerator* pEnum, EDataFlow dataFlow, const wchar_t* preferredId, bool preferUsbForInput, wchar_t* outName, size_t maxNameLen);
    static void GetEndpointFriendlyName(IMMDevice* pDevice, wchar_t* outName, size_t maxLen);
    static DWORD WINAPI AudioThreadProc(LPVOID param);
    DWORD RunRealTimeAudioLoop();
    void  ShowNativeControlPanelDialog();
    void  SetError(const char* msg);

private:
    volatile LONG                   m_refCount;
    HWND                            m_sysHandle               = nullptr;
    bool                            m_comInitialized          = false;
    bool                            m_initialized             = false;
    bool                            m_buffersCreated          = false;
    char                            m_errorMsg[128]           = {};

    DriverUserConfig                m_config                  = {};
    wchar_t                         m_renderDeviceName[128]   = L"Saída Padrão do Windows";
    wchar_t                         m_captureDeviceName[128]  = L"Entrada Padrão / Mesa USB";

    long                            m_numInputs               = 2;
    long                            m_hwCaptureChannels       = 2;
    long                            m_numOutputs              = 2;
    double                          m_nativeMixRate           = 48000.0;
    double                          m_nativeCaptureRate       = 48000.0;
    ASIOSampleRate                  m_sampleRate              = 48000.0;

    long                            m_hwMinBufferFrames       = 64;
    long                            m_hwPreferredBufferFrames = 256;
    long                            m_hwMaxBufferFrames       = 2048;
    long                            m_hwGranularityFrames     = 32;
    long                            m_engineOverheadFrames    = 64;

    long                            m_activeBufferFrames      = 256;
    long                            m_inputLatencyFrames      = 256;
    long                            m_outputLatencyFrames     = 320;

    ASIOCallbacks                   m_callbacks               = {};
    std::vector<ASIOBufferInfo>     m_activeChannels;
    std::vector<std::vector<float>> m_channelMemory;

    std::atomic<bool>               m_running{false};
    std::atomic<bool>               m_captureStreamActive{false};
    std::atomic<float>              m_inputPeakMeter{0.0f};
    std::atomic<uint64_t>           m_samplePosition{0};
    std::atomic<uint64_t>           m_systemTimeNs{0};
    HANDLE                          m_audioThread             = nullptr;
    HANDLE                          m_stopEvent               = nullptr;
    HANDLE                          m_renderEvent             = nullptr;
};