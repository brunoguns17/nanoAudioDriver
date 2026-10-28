// ============================================================================
// nanoAudioConfig.cpp
// Menu Gráfico de Configuração de Áudio ASIO (Win32)
// Universal WASAPI Low-Latency ASIO Driver
// ============================================================================

#include "nanoAudioConfig.h"
#include <commctrl.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// IDs dos Controles
enum ControlIDs {
    IDC_STATIC_HEADER = 100,
    IDC_GRP_OUTPUT,
    IDC_LBL_OUT_DEV,
    IDC_CMB_OUT_DEV,
    IDC_LBL_OUT_CH,
    IDC_CMB_OUT_CH,

    IDC_GRP_INPUT,
    IDC_CHK_ENABLE_IN,
    IDC_LBL_IN_DEV,
    IDC_CMB_IN_DEV,
    IDC_LBL_IN_CH,
    IDC_CMB_IN_CH,

    IDC_GRP_BUFFER,
    IDC_LBL_BUFFER_SIZE,
    IDC_CMB_BUFFER_SIZE,
    IDC_BTN_LAT_64,
    IDC_BTN_LAT_128,
    IDC_BTN_LAT_256,
    IDC_BTN_LAT_512,
    IDC_BTN_LAT_1024,
    IDC_STATUS_LATENCY,

    IDC_GRP_FORMAT,
    IDC_LBL_SAMPLE_RATE,
    IDC_CMB_SAMPLE_RATE,
    IDC_LBL_BIT_DEPTH,
    IDC_CMB_BIT_DEPTH,

    IDC_GRP_WASAPI_MODE,
    IDC_LBL_WASAPI_MODE,
    IDC_CMB_WASAPI_MODE,

    IDC_BTN_SAVE,
    IDC_BTN_CANCEL,
    IDC_BTN_DEFAULTS
};

// ============================================================================
// ENUMERAÇÃO DE DISPOSITIVOS DE ÁUDIO WASAPI
// ============================================================================

std::vector<AudioDeviceEntry> EnumerateAudioDevices(EDataFlow flow) {
    std::vector<AudioDeviceEntry> devices;
    IMMDeviceEnumerator* pEnum = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum)
    );
    if (FAILED(hr) || !pEnum) return devices;

    IMMDeviceCollection* pCollection = nullptr;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCollection)) && pCollection) {
        UINT count = 0;
        pCollection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
                LPWSTR pstrId = nullptr;
                pDev->GetId(&pstrId);

                std::wstring name = L"Dispositivo de Áudio";
                IPropertyStore* pProps = nullptr;
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                        name = varName.pwszVal;
                    }
                    PropVariantClear(&varName);
                    pProps->Release();
                }

                if (pstrId) {
                    devices.push_back({ pstrId, name });
                    CoTaskMemFree(pstrId);
                }
                pDev->Release();
            }
        }
        pCollection->Release();
    }
    pEnum->Release();
    return devices;
}

// ============================================================================
// PERSISTÊNCIA NO REGISTRO DO WINDOWS (HKCU)
// ============================================================================

DriverUserConfig LoadUserConfig() {
    DriverUserConfig cfg;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUserSettingsKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        RegOpenKeyExW(HKEY_CURRENT_USER, kLegacyUserSettingsKey, 0, KEY_READ, &hKey);
    }
    if (hKey != nullptr) {
        DWORD val = 0, sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"ExclusiveMode", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS)
            cfg.useExclusiveMode = (val != 0);

        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"EnableInput", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS)
            cfg.enableInputCapture = (val != 0);

        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"CustomBufferFrames", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS)
            cfg.customBufferFrames = static_cast<long>(val);

        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"SampleRate", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS && val > 0)
            cfg.preferredSampleRate = static_cast<long>(val);

        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"BitDepth", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS && val > 0)
            cfg.bitDepth = static_cast<long>(val);

        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"ChannelsOut", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS && val > 0)
            cfg.numChannelsOut = static_cast<long>(val);

        sz = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"ChannelsIn", nullptr, nullptr, reinterpret_cast<LPBYTE>(&val), &sz) == ERROR_SUCCESS && val > 0)
            cfg.numChannelsIn = static_cast<long>(val);

        sz = sizeof(cfg.outputDeviceId);
        if (RegQueryValueExW(hKey, L"OutputDeviceId", nullptr, nullptr, reinterpret_cast<LPBYTE>(cfg.outputDeviceId), &sz) != ERROR_SUCCESS || cfg.outputDeviceId[0] == L'\0') {
            sz = sizeof(cfg.outputDeviceId);
            RegQueryValueExW(hKey, L"OutputEndpointId", nullptr, nullptr, reinterpret_cast<LPBYTE>(cfg.outputDeviceId), &sz);
        }

        sz = sizeof(cfg.outputDeviceName);
        RegQueryValueExW(hKey, L"OutputDeviceName", nullptr, nullptr, reinterpret_cast<LPBYTE>(cfg.outputDeviceName), &sz);

        sz = sizeof(cfg.inputDeviceId);
        if (RegQueryValueExW(hKey, L"InputDeviceId", nullptr, nullptr, reinterpret_cast<LPBYTE>(cfg.inputDeviceId), &sz) != ERROR_SUCCESS || cfg.inputDeviceId[0] == L'\0') {
            sz = sizeof(cfg.inputDeviceId);
            RegQueryValueExW(hKey, L"InputEndpointId", nullptr, nullptr, reinterpret_cast<LPBYTE>(cfg.inputDeviceId), &sz);
        }

        sz = sizeof(cfg.inputDeviceName);
        RegQueryValueExW(hKey, L"InputDeviceName", nullptr, nullptr, reinterpret_cast<LPBYTE>(cfg.inputDeviceName), &sz);

        RegCloseKey(hKey);
    }
    return cfg;
}

void SaveUserConfig(const DriverUserConfig& cfg) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD excl = cfg.useExclusiveMode ? 1 : 0;
        DWORD inp  = cfg.enableInputCapture ? 1 : 0;
        DWORD buf  = static_cast<DWORD>(cfg.customBufferFrames);
        DWORD sr   = static_cast<DWORD>(cfg.preferredSampleRate);
        DWORD bits = static_cast<DWORD>(cfg.bitDepth);
        DWORD chO  = static_cast<DWORD>(cfg.numChannelsOut);
        DWORD chI  = static_cast<DWORD>(cfg.numChannelsIn);

        RegSetValueExW(hKey, L"ExclusiveMode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&excl), sizeof(DWORD));
        RegSetValueExW(hKey, L"EnableInput", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&inp), sizeof(DWORD));
        RegSetValueExW(hKey, L"CustomBufferFrames", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&buf), sizeof(DWORD));
        RegSetValueExW(hKey, L"SampleRate", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&sr), sizeof(DWORD));
        RegSetValueExW(hKey, L"BitDepth", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&bits), sizeof(DWORD));
        RegSetValueExW(hKey, L"ChannelsOut", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&chO), sizeof(DWORD));
        RegSetValueExW(hKey, L"ChannelsIn", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&chI), sizeof(DWORD));

        RegSetValueExW(hKey, L"OutputDeviceId", 0, REG_SZ, reinterpret_cast<const BYTE*>(cfg.outputDeviceId),
                       static_cast<DWORD>((std::wcslen(cfg.outputDeviceId) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"OutputEndpointId", 0, REG_SZ, reinterpret_cast<const BYTE*>(cfg.outputDeviceId),
                       static_cast<DWORD>((std::wcslen(cfg.outputDeviceId) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"OutputDeviceName", 0, REG_SZ, reinterpret_cast<const BYTE*>(cfg.outputDeviceName),
                       static_cast<DWORD>((std::wcslen(cfg.outputDeviceName) + 1) * sizeof(wchar_t)));

        RegSetValueExW(hKey, L"InputDeviceId", 0, REG_SZ, reinterpret_cast<const BYTE*>(cfg.inputDeviceId),
                       static_cast<DWORD>((std::wcslen(cfg.inputDeviceId) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"InputEndpointId", 0, REG_SZ, reinterpret_cast<const BYTE*>(cfg.inputDeviceId),
                       static_cast<DWORD>((std::wcslen(cfg.inputDeviceId) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"InputDeviceName", 0, REG_SZ, reinterpret_cast<const BYTE*>(cfg.inputDeviceName),
                       static_cast<DWORD>((std::wcslen(cfg.inputDeviceName) + 1) * sizeof(wchar_t)));

        RegCloseKey(hKey);
    }
}

// ============================================================================
// JANELA WIN32 DO PAINEL DE CONTROLE
// ============================================================================

struct DialogState {
    DriverUserConfig              workingConfig;
    DriverUserConfig*             targetConfig = nullptr;
    std::vector<AudioDeviceEntry> outputDevices;
    std::vector<AudioDeviceEntry> inputDevices;
    bool                          inDawMode    = false;
    double                        nativeRate   = 48000.0;
    long                          hwMin        = 64;
    long                          hwDef        = 256;
    long                          hwMax        = 2048;
    bool                          applied      = false;
    bool                          isOpen       = true;
    HFONT                         hFontNormal  = nullptr;
    HFONT                         hFontBold    = nullptr;
    HFONT                         hFontHeader  = nullptr;
    HFONT                         hFontBig     = nullptr;
    HBRUSH                        hBkgBrush    = nullptr;
};

static void UpdateLatencyDisplay(HWND hWnd, DialogState* pState) {
    HWND hCmbBuf = GetDlgItem(hWnd, IDC_CMB_BUFFER_SIZE);
    HWND hCmbSr  = GetDlgItem(hWnd, IDC_CMB_SAMPLE_RATE);
    HWND hCmbMod = GetDlgItem(hWnd, IDC_CMB_WASAPI_MODE);
    HWND hStatus = GetDlgItem(hWnd, IDC_STATUS_LATENCY);

    int bufIdx = (int)SendMessageW(hCmbBuf, CB_GETCURSEL, 0, 0);
    long bufFrames = (long)SendMessageW(hCmbBuf, CB_GETITEMDATA, bufIdx, 0);
    if (bufFrames <= 0) bufFrames = pState->hwDef;

    int srIdx = (int)SendMessageW(hCmbSr, CB_GETCURSEL, 0, 0);
    long sr = (long)SendMessageW(hCmbSr, CB_GETITEMDATA, srIdx, 0);
    if (sr <= 0) sr = 48000;

    int modIdx = (int)SendMessageW(hCmbMod, CB_GETCURSEL, 0, 0);
    long mode = (long)SendMessageW(hCmbMod, CB_GETITEMDATA, modIdx, 0);

    double oneWayMs = (static_cast<double>(bufFrames) * 1000.0) / static_cast<double>(sr);
    double roundTripMs = oneWayMs * 2.0;

    wchar_t text[256];
    std::swprintf(text, 256,
        L"⚡ Latência de Ida e Volta: ~%.1f ms (%ld samples @ %ld Hz) | Modo: %s",
        roundTripMs, bufFrames, sr,
        (mode == 1) ? L"Exclusivo (Acesso Direto ao Hardware)" : L"Compartilhado (Multi-app / YouTube / Spotify)");

    SetWindowTextW(hStatus, text);
}

static void SelectBufferByFrames(HWND hWnd, DialogState* pState, long frames) {
    HWND hCmbBuf = GetDlgItem(hWnd, IDC_CMB_BUFFER_SIZE);
    int count = (int)SendMessageW(hCmbBuf, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        long f = (long)SendMessageW(hCmbBuf, CB_GETITEMDATA, i, 0);
        if (f == frames) {
            SendMessageW(hCmbBuf, CB_SETCURSEL, i, 0);
            UpdateLatencyDisplay(hWnd, pState);
            break;
        }
    }
}

static LRESULT CALLBACK PanelWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    DialogState* pState = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pState = reinterpret_cast<DialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pState));
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    case WM_CREATE: {
        // Fontes modernas Segoe UI
        pState->hFontNormal = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        pState->hFontBold = CreateFontW(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        pState->hFontHeader = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        pState->hFontBig = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        pState->hBkgBrush = CreateSolidBrush(RGB(246, 247, 249));

        // 1. TÍTULO E CABEÇALHO
        HWND hHeader = CreateWindowExW(0, L"STATIC",
            L"nanoAudio Low-Latency ASIO Driver (v2.5)\n"
            L"Painel de Configurações de Áudio e Latência",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            16, 10, 480, 40, hWnd, (HMENU)IDC_STATIC_HEADER, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hHeader, WM_SETFONT, (WPARAM)pState->hFontHeader, TRUE);

        // 2. GRUPO: SAÍDA DE ÁUDIO (PLAYBACK)
        HWND hGrpOut = CreateWindowExW(0, L"BUTTON", L" 🎧 Saída de Áudio (Playback) ",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            16, 55, 480, 110, hWnd, (HMENU)IDC_GRP_OUTPUT, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hGrpOut, WM_SETFONT, (WPARAM)pState->hFontBold, TRUE);

        HWND hLblOut = CreateWindowExW(0, L"STATIC", L"Dispositivo de Saída (Caixas / Fones / Interface):",
            WS_CHILD | WS_VISIBLE, 30, 75, 350, 18, hWnd, (HMENU)IDC_LBL_OUT_DEV, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblOut, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbOut = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            30, 94, 452, 200, hWnd, (HMENU)IDC_CMB_OUT_DEV, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbOut, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hLblOutCh = CreateWindowExW(0, L"STATIC", L"Canais de Saída:",
            WS_CHILD | WS_VISIBLE, 30, 130, 120, 18, hWnd, (HMENU)IDC_LBL_OUT_CH, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblOutCh, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbOutCh = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            155, 126, 210, 120, hWnd, (HMENU)IDC_CMB_OUT_CH, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbOutCh, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        // 3. GRUPO: ENTRADA DE ÁUDIO (RECORDING)
        HWND hGrpIn = CreateWindowExW(0, L"BUTTON", L" 🎙️ Entrada de Áudio (Microfone / Interface) ",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            16, 172, 480, 130, hWnd, (HMENU)IDC_GRP_INPUT, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hGrpIn, WM_SETFONT, (WPARAM)pState->hFontBold, TRUE);

        HWND hChkIn = CreateWindowExW(0, L"BUTTON", L"Habilitar Entrada de Áudio (Full Duplex In / Out)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
            30, 194, 380, 20, hWnd, (HMENU)IDC_CHK_ENABLE_IN, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hChkIn, WM_SETFONT, (WPARAM)pState->hFontBold, TRUE);

        HWND hLblIn = CreateWindowExW(0, L"STATIC", L"Dispositivo de Entrada:",
            WS_CHILD | WS_VISIBLE, 30, 218, 200, 18, hWnd, (HMENU)IDC_LBL_IN_DEV, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblIn, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbIn = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            30, 236, 452, 200, hWnd, (HMENU)IDC_CMB_IN_DEV, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbIn, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hLblInCh = CreateWindowExW(0, L"STATIC", L"Canais de Entrada:",
            WS_CHILD | WS_VISIBLE, 30, 272, 120, 18, hWnd, (HMENU)IDC_LBL_IN_CH, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblInCh, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbInCh = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            155, 268, 210, 120, hWnd, (HMENU)IDC_CMB_IN_CH, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbInCh, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        // 4. GRUPO: BAIXAR LATÊNCIA & TAMANHO DO BUFFER
        HWND hGrpBuf = CreateWindowExW(0, L"BUTTON", L" ⚡ Ajustar e Baixar Latência (Buffer Size) ",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            16, 310, 480, 140, hWnd, (HMENU)IDC_GRP_BUFFER, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hGrpBuf, WM_SETFONT, (WPARAM)pState->hFontBold, TRUE);

        HWND hLblQuick = CreateWindowExW(0, L"STATIC", L"Atalhos Rápidos de Latência:",
            WS_CHILD | WS_VISIBLE, 30, 332, 200, 18, hWnd, (HMENU)150, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblQuick, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        // Botões de atalho rápido para baixar latência
        HWND hBtn64 = CreateWindowExW(0, L"BUTTON", L"64 (~1.3ms)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            30, 350, 85, 26, hWnd, (HMENU)IDC_BTN_LAT_64, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtn64, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hBtn128 = CreateWindowExW(0, L"BUTTON", L"128 (~2.7ms)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            120, 350, 85, 26, hWnd, (HMENU)IDC_BTN_LAT_128, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtn128, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hBtn256 = CreateWindowExW(0, L"BUTTON", L"256 (~5.3ms)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            210, 350, 85, 26, hWnd, (HMENU)IDC_BTN_LAT_256, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtn256, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hBtn512 = CreateWindowExW(0, L"BUTTON", L"512 (~10ms)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            300, 350, 85, 26, hWnd, (HMENU)IDC_BTN_LAT_512, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtn512, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hBtn1024 = CreateWindowExW(0, L"BUTTON", L"1024 (~21ms)",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            390, 350, 92, 26, hWnd, (HMENU)IDC_BTN_LAT_1024, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtn1024, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hLblBuf = CreateWindowExW(0, L"STATIC", L"Lista Completa de Tamanho do Buffer:",
            WS_CHILD | WS_VISIBLE, 30, 385, 250, 18, hWnd, (HMENU)IDC_LBL_BUFFER_SIZE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblBuf, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbBuf = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
            30, 403, 452, 200, hWnd, (HMENU)IDC_CMB_BUFFER_SIZE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbBuf, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        // Barra de cálculo de latência em tempo real
        HWND hStatusLat = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            30, 430, 452, 18, hWnd, (HMENU)IDC_STATUS_LATENCY, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hStatusLat, WM_SETFONT, (WPARAM)pState->hFontBig, TRUE);

        // 5. GRUPO: SAMPLE RATE & RESOLUÇÃO
        HWND hGrpFormat = CreateWindowExW(0, L"BUTTON", L" 🎛️ Sample Rate & Resolução de Áudio ",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            16, 458, 480, 80, hWnd, (HMENU)IDC_GRP_FORMAT, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hGrpFormat, WM_SETFONT, (WPARAM)pState->hFontBold, TRUE);

        HWND hLblSr = CreateWindowExW(0, L"STATIC", L"Sample Rate (Taxa de Amostragem):",
            WS_CHILD | WS_VISIBLE, 30, 480, 220, 18, hWnd, (HMENU)IDC_LBL_SAMPLE_RATE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblSr, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbSr = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            30, 498, 210, 180, hWnd, (HMENU)IDC_CMB_SAMPLE_RATE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbSr, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hLblBits = CreateWindowExW(0, L"STATIC", L"Resolução de Bits (Bit Depth):",
            WS_CHILD | WS_VISIBLE, 260, 480, 210, 18, hWnd, (HMENU)IDC_LBL_BIT_DEPTH, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblBits, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbBits = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            260, 498, 222, 180, hWnd, (HMENU)IDC_CMB_BIT_DEPTH, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbBits, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        // 6. GRUPO: MODO WASAPI
        HWND hGrpMode = CreateWindowExW(0, L"BUTTON", L" 🔒 Modo de Operação WASAPI ",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            16, 545, 480, 75, hWnd, (HMENU)IDC_GRP_WASAPI_MODE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hGrpMode, WM_SETFONT, (WPARAM)pState->hFontBold, TRUE);

        HWND hLblMode = CreateWindowExW(0, L"STATIC", L"Escolha como o driver compartilha o som com o Windows:",
            WS_CHILD | WS_VISIBLE, 30, 567, 350, 18, hWnd, (HMENU)IDC_LBL_WASAPI_MODE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hLblMode, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hCmbMode = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
            30, 585, 452, 120, hWnd, (HMENU)IDC_CMB_WASAPI_MODE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hCmbMode, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        // 7. BOTÕES DE AÇÃO
        HWND hBtnDef = CreateWindowExW(0, L"BUTTON", L"Restaurar Padrões",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            16, 630, 140, 32, hWnd, (HMENU)IDC_BTN_DEFAULTS, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtnDef, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        HWND hBtnSave = CreateWindowExW(0, L"BUTTON", L"✔ Salvar & Aplicar",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            240, 630, 150, 32, hWnd, (HMENU)IDC_BTN_SAVE, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtnSave, WM_SETFONT, (WPARAM)pState->hFontBold, TRUE);

        HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", L"Cancelar",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            400, 630, 96, 32, hWnd, (HMENU)IDC_BTN_CANCEL, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hBtnCancel, WM_SETFONT, (WPARAM)pState->hFontNormal, TRUE);

        // ====================================================================
        // POPULAR DADOS NOS CONTROLES
        // ====================================================================

        // Dispositivos de Saída
        int selOut = 0;
        int idx = (int)SendMessageW(hCmbOut, CB_ADDSTRING, 0, (LPARAM)L"[Padrão do Windows] Dispositivo Padrão");
        SendMessageW(hCmbOut, CB_SETITEMDATA, idx, (LPARAM)-1);

        for (size_t i = 0; i < pState->outputDevices.size(); ++i) {
            idx = (int)SendMessageW(hCmbOut, CB_ADDSTRING, 0, (LPARAM)pState->outputDevices[i].name.c_str());
            SendMessageW(hCmbOut, CB_SETITEMDATA, idx, (LPARAM)i);
            if (pState->workingConfig.outputDeviceId[0] != L'\0' &&
                pState->outputDevices[i].id == pState->workingConfig.outputDeviceId) {
                selOut = idx;
            }
        }
        SendMessageW(hCmbOut, CB_SETCURSEL, selOut, 0);

        // Canais de Saída
        const struct { const wchar_t* label; int ch; } outChans[] = {
            { L"2 Canais (Estéreo Master L / R)", 2 },
            { L"4 Canais (Quad / Saídas 1-2 e 3-4)", 4 },
            { L"6 Canais (5.1 Surround)", 6 },
            { L"8 Canais (7.1 Surround)", 8 }
        };
        int selOutCh = 0;
        for (int i = 0; i < 4; ++i) {
            idx = (int)SendMessageW(hCmbOutCh, CB_ADDSTRING, 0, (LPARAM)outChans[i].label);
            SendMessageW(hCmbOutCh, CB_SETITEMDATA, idx, (LPARAM)outChans[i].ch);
            if (pState->workingConfig.numChannelsOut == outChans[i].ch) selOutCh = idx;
        }
        SendMessageW(hCmbOutCh, CB_SETCURSEL, selOutCh, 0);

        // Habilitar Entrada Checkbox
        SendMessageW(hChkIn, BM_SETCHECK, pState->workingConfig.enableInputCapture ? BST_CHECKED : BST_UNCHECKED, 0);

        // Dispositivos de Entrada
        int selIn = 0;
        idx = (int)SendMessageW(hCmbIn, CB_ADDSTRING, 0, (LPARAM)L"[Padrão do Windows] Dispositivo Padrão");
        SendMessageW(hCmbIn, CB_SETITEMDATA, idx, (LPARAM)-1);

        for (size_t i = 0; i < pState->inputDevices.size(); ++i) {
            idx = (int)SendMessageW(hCmbIn, CB_ADDSTRING, 0, (LPARAM)pState->inputDevices[i].name.c_str());
            SendMessageW(hCmbIn, CB_SETITEMDATA, idx, (LPARAM)i);
            if (pState->workingConfig.inputDeviceId[0] != L'\0' &&
                pState->inputDevices[i].id == pState->workingConfig.inputDeviceId) {
                selIn = idx;
            }
        }
        SendMessageW(hCmbIn, CB_SETCURSEL, selIn, 0);

        // Canais de Entrada
        const struct { const wchar_t* label; int ch; } inChans[] = {
            { L"1 Canal (Mono In 1)", 1 },
            { L"2 Canais (Estéreo In 1 / In 2)", 2 }
        };
        int selInCh = 1;
        for (int i = 0; i < 2; ++i) {
            idx = (int)SendMessageW(hCmbInCh, CB_ADDSTRING, 0, (LPARAM)inChans[i].label);
            SendMessageW(hCmbInCh, CB_SETITEMDATA, idx, (LPARAM)inChans[i].ch);
            if (pState->workingConfig.numChannelsIn == inChans[i].ch) selInCh = idx;
        }
        SendMessageW(hCmbInCh, CB_SETCURSEL, selInCh, 0);

        EnableWindow(hCmbIn, pState->workingConfig.enableInputCapture ? TRUE : FALSE);
        EnableWindow(hCmbInCh, pState->workingConfig.enableInputCapture ? TRUE : FALSE);

        // Sample Rates
        const long sampleRates[] = { 44100, 48000, 88200, 96000, 192000 };
        const wchar_t* srLabels[] = {
            L"44.1 kHz (44100 Hz - CD / Streaming)",
            L"48.0 kHz (48000 Hz - Padrão Estúdio / DAW)",
            L"88.2 kHz (88200 Hz - Alta Resolução)",
            L"96.0 kHz (96000 Hz - Gravação Master Pro)",
            L"192.0 kHz (192000 Hz - Hi-Res Audio)"
        };
        int selSr = 1;
        for (int i = 0; i < 5; ++i) {
            idx = (int)SendMessageW(hCmbSr, CB_ADDSTRING, 0, (LPARAM)srLabels[i]);
            SendMessageW(hCmbSr, CB_SETITEMDATA, idx, (LPARAM)sampleRates[i]);
            if (pState->workingConfig.preferredSampleRate == sampleRates[i]) selSr = idx;
        }
        SendMessageW(hCmbSr, CB_SETCURSEL, selSr, 0);

        // Resolução de Bits
        const struct { const wchar_t* label; int bits; } bitDepths[] = {
            { L"32-bit Float (IEEE Float - Máxima Fidelidade / Padrão DAW)", 32 },
            { L"24-bit PCM (Alta Definição / Interfaces USB)", 24 },
            { L"16-bit PCM (Padrão CD / Compatibilidade)", 16 }
        };
        int selBits = 0;
        for (int i = 0; i < 3; ++i) {
            idx = (int)SendMessageW(hCmbBits, CB_ADDSTRING, 0, (LPARAM)bitDepths[i].label);
            SendMessageW(hCmbBits, CB_SETITEMDATA, idx, (LPARAM)bitDepths[i].bits);
            if (pState->workingConfig.bitDepth == bitDepths[i].bits) selBits = idx;
        }
        SendMessageW(hCmbBits, CB_SETCURSEL, selBits, 0);

        // Tamanhos de Buffer
        const struct { const wchar_t* label; long frames; } buffers[] = {
            { L"Automático (Detectado pelo Hardware WASAPI)", 0 },
            { L"32 samples (~0.7 ms - Latência Extrema)", 32 },
            { L"64 samples (~1.3 ms - Ultra Baixa Latência)", 64 },
            { L"128 samples (~2.7 ms - Muito Baixa Latência)", 128 },
            { L"256 samples (~5.3 ms - Balanceado / Gravação)", 256 },
            { L"512 samples (~10.7 ms - Mixagem Estável)", 512 },
            { L"1024 samples (~21.3 ms - Projetos Pesados)", 1024 },
            { L"2048 samples (~42.7 ms - Máxima Estabilidade)", 2048 }
        };
        int selBuf = 0;
        for (int i = 0; i < 8; ++i) {
            idx = (int)SendMessageW(hCmbBuf, CB_ADDSTRING, 0, (LPARAM)buffers[i].label);
            SendMessageW(hCmbBuf, CB_SETITEMDATA, idx, (LPARAM)buffers[i].frames);
            if (pState->workingConfig.customBufferFrames == buffers[i].frames) selBuf = idx;
        }
        SendMessageW(hCmbBuf, CB_SETCURSEL, selBuf, 0);

        // Modo WASAPI
        const wchar_t* modes[] = {
            L"Modo Compartilhado (Shared) - Áudio simultâneo com YouTube/Spotify/Discord",
            L"Modo Exclusivo (Exclusive) - Foco em latência mínima direta no hardware"
        };
        idx = (int)SendMessageW(hCmbMode, CB_ADDSTRING, 0, (LPARAM)modes[0]);
        SendMessageW(hCmbMode, CB_SETITEMDATA, idx, (LPARAM)0);
        idx = (int)SendMessageW(hCmbMode, CB_ADDSTRING, 0, (LPARAM)modes[1]);
        SendMessageW(hCmbMode, CB_SETITEMDATA, idx, (LPARAM)1);
        SendMessageW(hCmbMode, CB_SETCURSEL, pState->workingConfig.useExclusiveMode ? 1 : 0, 0);

        UpdateLatencyDisplay(hWnd, pState);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdcStatic = (HDC)wParam;
        SetBkColor(hdcStatic, RGB(246, 247, 249));
        SetTextColor(hdcStatic, RGB(30, 30, 30));
        return (LRESULT)pState->hBkgBrush;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        // Botões rápidos de latência
        if (id == IDC_BTN_LAT_64 && code == BN_CLICKED)   { SelectBufferByFrames(hWnd, pState, 64); return 0; }
        if (id == IDC_BTN_LAT_128 && code == BN_CLICKED)  { SelectBufferByFrames(hWnd, pState, 128); return 0; }
        if (id == IDC_BTN_LAT_256 && code == BN_CLICKED)  { SelectBufferByFrames(hWnd, pState, 256); return 0; }
        if (id == IDC_BTN_LAT_512 && code == BN_CLICKED)  { SelectBufferByFrames(hWnd, pState, 512); return 0; }
        if (id == IDC_BTN_LAT_1024 && code == BN_CLICKED) { SelectBufferByFrames(hWnd, pState, 1024); return 0; }

        if (id == IDC_CHK_ENABLE_IN && code == BN_CLICKED) {
            LRESULT chk = SendMessageW(GetDlgItem(hWnd, IDC_CHK_ENABLE_IN), BM_GETCHECK, 0, 0);
            BOOL enable = (chk == BST_CHECKED);
            EnableWindow(GetDlgItem(hWnd, IDC_CMB_IN_DEV), enable);
            EnableWindow(GetDlgItem(hWnd, IDC_CMB_IN_CH), enable);
        }

        if (code == CBN_SELCHANGE) {
            UpdateLatencyDisplay(hWnd, pState);
        }

        if (id == IDC_BTN_SAVE) {
            // Salvar escolhas
            HWND hCmbOut = GetDlgItem(hWnd, IDC_CMB_OUT_DEV);
            HWND hCmbOutCh = GetDlgItem(hWnd, IDC_CMB_OUT_CH);
            HWND hChkIn = GetDlgItem(hWnd, IDC_CHK_ENABLE_IN);
            HWND hCmbIn = GetDlgItem(hWnd, IDC_CMB_IN_DEV);
            HWND hCmbInCh = GetDlgItem(hWnd, IDC_CMB_IN_CH);
            HWND hCmbSr = GetDlgItem(hWnd, IDC_CMB_SAMPLE_RATE);
            HWND hCmbBits = GetDlgItem(hWnd, IDC_CMB_BIT_DEPTH);
            HWND hCmbBuf = GetDlgItem(hWnd, IDC_CMB_BUFFER_SIZE);
            HWND hCmbMode = GetDlgItem(hWnd, IDC_CMB_WASAPI_MODE);

            // Saída
            int selOut = (int)SendMessageW(hCmbOut, CB_GETCURSEL, 0, 0);
            int devIdx = (int)SendMessageW(hCmbOut, CB_GETITEMDATA, selOut, 0);
            if (devIdx >= 0 && devIdx < (int)pState->outputDevices.size()) {
                wcsncpy(pState->workingConfig.outputDeviceId, pState->outputDevices[devIdx].id.c_str(), 255);
                wcsncpy(pState->workingConfig.outputDeviceName, pState->outputDevices[devIdx].name.c_str(), 127);
            } else {
                pState->workingConfig.outputDeviceId[0] = L'\0';
                pState->workingConfig.outputDeviceName[0] = L'\0';
            }

            int selOutCh = (int)SendMessageW(hCmbOutCh, CB_GETCURSEL, 0, 0);
            pState->workingConfig.numChannelsOut = (long)SendMessageW(hCmbOutCh, CB_GETITEMDATA, selOutCh, 0);

            // Entrada
            pState->workingConfig.enableInputCapture = (SendMessageW(hChkIn, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (pState->workingConfig.enableInputCapture) {
                int selIn = (int)SendMessageW(hCmbIn, CB_GETCURSEL, 0, 0);
                int inIdx = (int)SendMessageW(hCmbIn, CB_GETITEMDATA, selIn, 0);
                if (inIdx >= 0 && inIdx < (int)pState->inputDevices.size()) {
                    wcsncpy(pState->workingConfig.inputDeviceId, pState->inputDevices[inIdx].id.c_str(), 255);
                    wcsncpy(pState->workingConfig.inputDeviceName, pState->inputDevices[inIdx].name.c_str(), 127);
                } else {
                    pState->workingConfig.inputDeviceId[0] = L'\0';
                    pState->workingConfig.inputDeviceName[0] = L'\0';
                }

                int selInCh = (int)SendMessageW(hCmbInCh, CB_GETCURSEL, 0, 0);
                pState->workingConfig.numChannelsIn = (long)SendMessageW(hCmbInCh, CB_GETITEMDATA, selInCh, 0);
            } else {
                pState->workingConfig.numChannelsIn = 0;
            }

            // Sample Rate & Bit Depth
            int selSr = (int)SendMessageW(hCmbSr, CB_GETCURSEL, 0, 0);
            pState->workingConfig.preferredSampleRate = (long)SendMessageW(hCmbSr, CB_GETITEMDATA, selSr, 0);

            int selBits = (int)SendMessageW(hCmbBits, CB_GETCURSEL, 0, 0);
            pState->workingConfig.bitDepth = (long)SendMessageW(hCmbBits, CB_GETITEMDATA, selBits, 0);

            // Buffer & Modo
            int selBuf = (int)SendMessageW(hCmbBuf, CB_GETCURSEL, 0, 0);
            pState->workingConfig.customBufferFrames = (long)SendMessageW(hCmbBuf, CB_GETITEMDATA, selBuf, 0);

            int selMode = (int)SendMessageW(hCmbMode, CB_GETCURSEL, 0, 0);
            pState->workingConfig.useExclusiveMode = (SendMessageW(hCmbMode, CB_GETITEMDATA, selMode, 0) == 1);

            // Gravar no Registro do Windows (HKCU)
            SaveUserConfig(pState->workingConfig);
            if (pState->targetConfig) {
                *pState->targetConfig = pState->workingConfig;
            }

            pState->applied = true;
            DestroyWindow(hWnd);
            return 0;
        }

        if (id == IDC_BTN_CANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }

        if (id == IDC_BTN_DEFAULTS) {
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_OUT_DEV), CB_SETCURSEL, 0, 0);
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_OUT_CH), CB_SETCURSEL, 0, 0);
            SendMessageW(GetDlgItem(hWnd, IDC_CHK_ENABLE_IN), BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_IN_DEV), CB_SETCURSEL, 0, 0);
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_IN_CH), CB_SETCURSEL, 1, 0);
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_SAMPLE_RATE), CB_SETCURSEL, 1, 0); // 48000
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_BIT_DEPTH), CB_SETCURSEL, 0, 0); // 32-bit Float
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_BUFFER_SIZE), CB_SETCURSEL, 0, 0); // Auto
            SendMessageW(GetDlgItem(hWnd, IDC_CMB_WASAPI_MODE), CB_SETCURSEL, 0, 0); // Shared
            EnableWindow(GetDlgItem(hWnd, IDC_CMB_IN_DEV), TRUE);
            EnableWindow(GetDlgItem(hWnd, IDC_CMB_IN_CH), TRUE);
            UpdateLatencyDisplay(hWnd, pState);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        if (pState->hFontNormal) DeleteObject(pState->hFontNormal);
        if (pState->hFontBold)   DeleteObject(pState->hFontBold);
        if (pState->hFontHeader) DeleteObject(pState->hFontHeader);
        if (pState->hFontBig)    DeleteObject(pState->hFontBig);
        if (pState->hBkgBrush)   DeleteObject(pState->hBkgBrush);
        pState->isOpen = false;
        PostMessageW(hWnd, WM_NULL, 0, 0);
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

bool ShowControlPanelWindow(
    HWND hWndParent,
    DriverUserConfig* inOutConfig,
    bool inDawMode,
    double nativeMixRate,
    long hwMinFrames,
    long hwDefFrames,
    long hwMaxFrames
) {
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    DialogState state;
    state.targetConfig  = inOutConfig;
    state.workingConfig = inOutConfig ? *inOutConfig : LoadUserConfig();
    state.outputDevices = EnumerateAudioDevices(eRender);
    state.inputDevices  = EnumerateAudioDevices(eCapture);
    state.inDawMode     = inDawMode;
    state.nativeRate    = nativeMixRate;
    state.hwMin         = hwMinFrames;
    state.hwDef         = hwDefFrames;
    state.hwMax         = hwMaxFrames;
    state.applied       = false;
    state.isOpen        = true;

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    const wchar_t* kClassName = L"UniversalWasapiAsioPanelClass";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = PanelWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    const int dlgWidth  = 526;
    const int dlgHeight = 718;
    int posX = (GetSystemMetrics(SM_CXSCREEN) - dlgWidth) / 2;
    int posY = (GetSystemMetrics(SM_CYSCREEN) - dlgHeight) / 2;

    if (hWndParent && IsWindow(hWndParent)) {
        RECT parentRect = {};
        GetWindowRect(hWndParent, &parentRect);
        posX = parentRect.left + ((parentRect.right - parentRect.left) - dlgWidth) / 2;
        posY = parentRect.top  + ((parentRect.bottom - parentRect.top) - dlgHeight) / 2;
        if (posX < 0) posX = 0;
        if (posY < 0) posY = 0;
    }

    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HWND hWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kClassName,
        L"nanoAudio ASIO Driver (v2.5) — Configurações",
        dwStyle,
        posX, posY, dlgWidth, dlgHeight,
        hWndParent,
        nullptr,
        hInstance,
        &state
    );

    if (!hWnd) return false;

    if (hWndParent && IsWindow(hWndParent)) {
        EnableWindow(hWndParent, FALSE);
    }

    ShowWindow(hWnd, SW_SHOW);
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    UpdateWindow(hWnd);

    // Modal message loop seguro (não usa PostQuitMessage que fecharia a DAW)
    MSG msg;
    while (state.isOpen && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hWndParent && IsWindow(hWndParent)) {
        EnableWindow(hWndParent, TRUE);
        SetForegroundWindow(hWndParent);
    }

    UnregisterClassW(kClassName, hInstance);
    return state.applied;
}
