// ============================================================================
// nanoAudioVolume.h
// Volume do Windows na saída: consulta (painel) e compensação (driver).
//
// No modo compartilhado o Windows aplica o volume do dispositivo a tudo que passa
// pelo mixer. Quando o dispositivo não tem controle de volume no próprio hardware,
// esse volume é só uma multiplicação por software, e o áudio da DAW sai mais baixo
// do que sairia por WDM/KS ou ASIO nativo, que não passam pelo mixer (ex.: 40% na
// Behringer USB = -13,9 dB).
//
// A atenuação é aplicada ao stream em ponto flutuante ANTES do limitador do Windows.
// Por isso, multiplicar o áudio pelo ganho inverso devolve exatamente o nível
// original, sem distorção e sem mexer no volume dos outros programas.
//
// Se o volume é do hardware (ex.: Realtek), ele também vale para o WDM/KS: nada a fazer.
// ============================================================================

#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>

namespace nanoAudioVolume {

// Limite de segurança do ganho de compensação (volume do Windows perto de zero).
constexpr float kMaxMakeupDb = 30.0f;

struct EndpointVolume {
    bool  valid    = false;
    bool  software = false;   // true = atenuação feita pelo mixer do Windows
    bool  muted    = false;
    float scalar   = 1.0f;    // posição do controle (0..1)
    float db       = 0.0f;
};

inline EndpointVolume Query(IAudioEndpointVolume* vol) {
    EndpointVolume v;
    if (!vol) return v;
    DWORD hw   = 0;
    BOOL  mute = FALSE;
    if (FAILED(vol->GetMasterVolumeLevelScalar(&v.scalar)) || FAILED(vol->GetMasterVolumeLevel(&v.db))) return v;
    if (FAILED(vol->QueryHardwareSupport(&hw))) hw = ENDPOINT_HARDWARE_SUPPORT_VOLUME;  // na dúvida, não compensa
    vol->GetMute(&mute);
    v.software = (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) == 0;
    v.muted    = (mute != FALSE);
    v.valid    = true;
    return v;
}

inline EndpointVolume Query(IMMDevice* device) {
    EndpointVolume v;
    IAudioEndpointVolume* vol = nullptr;
    if (device && SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(&vol))) && vol) {
        v = Query(vol);
        vol->Release();
    }
    return v;
}

// Ganho que anula uma atenuação de "db". Mudo e volume zero são respeitados: continuam mudos.
inline float MakeupGain(float db, float scalar, bool muted) {
    if (muted || scalar < 0.005f || !(db < 0.0f)) return 1.0f;
    return std::pow(10.0f, std::min(-db, kMaxMakeupDb) / 20.0f);
}

// Acompanha o volume do endpoint em uma thread comum; a thread de áudio só lê atômicos.
class Compensator {
public:
    static constexpr long kMaxChannels = 8;

    Compensator() { for (auto& g : m_gain) g.store(1.0f); }
    ~Compensator() { Stop(); }
    Compensator(const Compensator&) = delete;
    Compensator& operator=(const Compensator&) = delete;

    // Retorna true se o endpoint usa volume por software (compensação ativa).
    bool Start(IMMDevice* device, long streamChannels) {
        Stop();
        m_channels = std::clamp<long>(streamChannels, 1, kMaxChannels);
        if (!device || FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                               reinterpret_cast<void**>(&m_vol))) || !m_vol) {
            m_vol = nullptr;
            return false;
        }
        if (!Query(m_vol).software) { Stop(); return false; }
        Refresh();

        m_stop   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_notify = new (std::nothrow) Notify();
        if (!m_stop || !m_notify || !m_notify->Event()) { Stop(); return false; }
        m_registered = SUCCEEDED(m_vol->RegisterControlChangeNotify(m_notify));
        m_thread = CreateThread(nullptr, 0, &Compensator::ThreadProc, this, 0, nullptr);
        if (!m_thread) { Stop(); return false; }
        return true;
    }

    void Stop() {
        if (m_thread) {
            SetEvent(m_stop);
            WaitForSingleObject(m_thread, INFINITE);
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
        if (m_registered) { m_vol->UnregisterControlChangeNotify(m_notify); m_registered = false; }
        if (m_notify) { m_notify->Release(); m_notify = nullptr; }
        if (m_stop)   { CloseHandle(m_stop); m_stop = nullptr; }
        if (m_vol)    { m_vol->Release(); m_vol = nullptr; }
        for (auto& g : m_gain) g.store(1.0f, std::memory_order_relaxed);
    }

    float Gain(long ch) const {
        return m_gain[std::clamp<long>(ch, 0, kMaxChannels - 1)].load(std::memory_order_relaxed);
    }

private:
    // O callback do Windows só sinaliza um evento (não pode bloquear nem liberar objetos).
    class Notify final : public IAudioEndpointVolumeCallback {
    public:
        Notify() : m_event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
        HANDLE Event() const { return m_event; }
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (!ppv) return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
                *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&m_ref)); }
        STDMETHODIMP_(ULONG) Release() override {
            const LONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return static_cast<ULONG>(r);
        }
        STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA) override {
            SetEvent(m_event);
            return S_OK;
        }
    private:
        ~Notify() { if (m_event) CloseHandle(m_event); }
        volatile LONG m_ref = 1;
        HANDLE        m_event;
    };

    static DWORD WINAPI ThreadProc(LPVOID param) {
        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Compensator* self = static_cast<Compensator*>(param);
        HANDLE handles[2] = { self->m_stop, self->m_notify->Event() };
        // O tempo limite cobre drivers que não notificam mudanças de volume.
        while (WaitForMultipleObjects(2, handles, FALSE, 500) != WAIT_OBJECT_0) self->Refresh();
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 0;
    }

    void Refresh() {
        const EndpointVolume v = Query(m_vol);
        if (!v.valid) return;
        UINT endpointChannels = 0;
        m_vol->GetChannelCount(&endpointChannels);
        for (long c = 0; c < m_channels; ++c) {
            float db = v.db;
            if (endpointChannels == static_cast<UINT>(m_channels)) {
                float chDb = 0.0f;
                if (SUCCEEDED(m_vol->GetChannelVolumeLevel(static_cast<UINT>(c), &chDb))) db = chDb;
            }
            m_gain[c].store(MakeupGain(db, v.scalar, v.muted), std::memory_order_relaxed);
        }
    }

    IAudioEndpointVolume* m_vol        = nullptr;
    Notify*               m_notify     = nullptr;
    HANDLE                m_stop       = nullptr;
    HANDLE                m_thread     = nullptr;
    bool                  m_registered = false;
    long                  m_channels   = 2;
    std::atomic<float>    m_gain[kMaxChannels];
};

} // namespace nanoAudioVolume
