// ============================================================================
// nanoAudioConfig.cpp
// Menu Gráfico de Configuração de Áudio ASIO (Win32)
// Universal WASAPI Low-Latency ASIO Driver
// ============================================================================

#include "nanoAudioConfig.h"
#include "nanoAudioVersion.h"
#include "nanoAudioUpdateState.h"
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
    IDC_BTN_DEFAULTS,

    IDC_LBL_QUICK,
    IDC_BTN_UPDATES
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
//
// Layout (em unidades de 96 DPI, escaladas em tempo de execução):
//
//   +--------------------------------------------------------------+
//   | nanoAudio                                   10.7 ms          |  <- cabeçalho escuro
//   | Low-Latency ASIO Driver · v2.6.0    latência ida e volta     |
//   +------------------------------+-------------------------------+
//   | [Saída de áudio]             | [Latência e buffer]           |
//   | [Entrada de áudio]           | [Formato e modo WASAPI]       |
//   +------------------------------+-------------------------------+
//   | [Restaurar] [Atualizações]   [Salvar e aplicar] [Cancelar]   |  <- rodapé
//   +--------------------------------------------------------------+

namespace Layout {
    constexpr int ClientW   = 760;
    constexpr int HeaderH   = 76;
    constexpr int Margin    = 20;
    constexpr int ColW      = 350;
    constexpr int ColL      = Margin;                      // 20
    constexpr int ColR      = ClientW - Margin - ColW;     // 390
    constexpr int Pad       = 16;
    constexpr int InnerW    = ColW - 2 * Pad;              // 318
    constexpr int Top       = HeaderH + 16;                // 92
    constexpr int Bottom    = 404;                         // base das colunas
    constexpr int FooterTop = Bottom + Margin;             // 424
    constexpr int FooterH   = 56;
    constexpr int ClientH   = FooterTop + FooterH;         // 480
    constexpr int LblH      = 18;
    constexpr int BtnH      = 30;
    constexpr int ComboDrop = 300;                         // altura da lista aberta
}

namespace Palette {
    constexpr COLORREF HeaderBg   = RGB(24, 28, 36);
    constexpr COLORREF HeaderText = RGB(255, 255, 255);
    constexpr COLORREF HeaderSub  = RGB(150, 160, 176);
    constexpr COLORREF BodyBg     = RGB(255, 255, 255);
    constexpr COLORREF BodyText   = RGB(32, 32, 32);
    constexpr COLORREF MutedText  = RGB(104, 104, 104);
    constexpr COLORREF FooterBg   = RGB(243, 243, 243);
    constexpr COLORREF FooterLine = RGB(223, 223, 223);
    constexpr COLORREF LatGood    = RGB(72, 207, 133);   // <= 10 ms
    constexpr COLORREF LatOk      = RGB(255, 191, 71);   // <= 25 ms
    constexpr COLORREF LatHigh    = RGB(255, 112, 92);   //  > 25 ms
}

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
    int                           dpi          = 96;
    HFONT                         hFontNormal  = nullptr;
    HFONT                         hFontBold    = nullptr;
    HFONT                         hFontSmall   = nullptr;
    HFONT                         hFontTitle   = nullptr;
    HFONT                         hFontReadout = nullptr;
    HBRUSH                        hBodyBrush   = nullptr;
    HBRUSH                        hFooterBrush = nullptr;
    wchar_t                       readoutText[32]     = {};
    wchar_t                       readoutCaption[128] = {};
    COLORREF                      readoutColor = Palette::LatGood;

    int S(int v) const { return MulDiv(v, dpi, 96); }
};

static const struct { int id; long frames; } kQuickBuffers[] = {
    { IDC_BTN_LAT_64,   64 },
    { IDC_BTN_LAT_128,  128 },
    { IDC_BTN_LAT_256,  256 },
    { IDC_BTN_LAT_512,  512 },
    { IDC_BTN_LAT_1024, 1024 },
};

static HFONT MakeUiFont(int dpi, int points, int weight) {
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

// Cria um controle filho com coordenadas em unidades de 96 DPI.
static HWND AddControl(HWND hParent, const DialogState* st, const wchar_t* cls, const wchar_t* text,
                       DWORD style, int x, int y, int w, int h, int id, HFONT font) {
    HWND hCtl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
        st->S(x), st->S(y), st->S(w), st->S(h),
        hParent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (hCtl) SendMessageW(hCtl, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    return hCtl;
}

static HWND AddLabel(HWND hParent, const DialogState* st, const wchar_t* text,
                     int x, int y, int w, int id, HFONT font = nullptr) {
    return AddControl(hParent, st, L"STATIC", text, SS_LEFT | SS_NOPREFIX,
                      x, y, w, Layout::LblH, id, font ? font : st->hFontNormal);
}

static HWND AddCombo(HWND hParent, const DialogState* st, int x, int y, int w, int id) {
    return AddControl(hParent, st, L"COMBOBOX", nullptr,
                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                      x, y, w, Layout::ComboDrop, id, st->hFontNormal);
}

static void FormatKHz(wchar_t* out, size_t cap, long sr) {
    if (sr % 1000 == 0) std::swprintf(out, cap, L"%ld kHz", sr / 1000);
    else                std::swprintf(out, cap, L"%.1f kHz", static_cast<double>(sr) / 1000.0);
}

static void SetInputControlsEnabled(HWND hWnd, bool enable) {
    const int ids[] = { IDC_LBL_IN_DEV, IDC_CMB_IN_DEV, IDC_LBL_IN_CH, IDC_CMB_IN_CH };
    for (int id : ids) EnableWindow(GetDlgItem(hWnd, id), enable ? TRUE : FALSE);
}

static RECT HeaderRect(HWND hWnd, const DialogState* st) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    rc.bottom = st->S(Layout::HeaderH);
    return rc;
}

static void UpdateLatencyDisplay(HWND hWnd, DialogState* pState) {
    HWND hCmbBuf = GetDlgItem(hWnd, IDC_CMB_BUFFER_SIZE);
    HWND hCmbSr  = GetDlgItem(hWnd, IDC_CMB_SAMPLE_RATE);
    HWND hCmbMod = GetDlgItem(hWnd, IDC_CMB_WASAPI_MODE);

    int bufIdx = (int)SendMessageW(hCmbBuf, CB_GETCURSEL, 0, 0);
    long selFrames = (long)SendMessageW(hCmbBuf, CB_GETITEMDATA, bufIdx, 0);
    const bool isAuto = (selFrames <= 0);
    long bufFrames = isAuto ? pState->hwDef : selFrames;
    if (bufFrames <= 0) bufFrames = 256;

    int srIdx = (int)SendMessageW(hCmbSr, CB_GETCURSEL, 0, 0);
    long sr = (long)SendMessageW(hCmbSr, CB_GETITEMDATA, srIdx, 0);
    if (sr <= 0) sr = 48000;

    int modIdx = (int)SendMessageW(hCmbMod, CB_GETCURSEL, 0, 0);
    long mode = (long)SendMessageW(hCmbMod, CB_GETITEMDATA, modIdx, 0);

    const double oneWayMs    = (static_cast<double>(bufFrames) * 1000.0) / static_cast<double>(sr);
    const double roundTripMs = oneWayMs * 2.0;

    // Leitura grande no cabeçalho
    std::swprintf(pState->readoutText, 32, L"%.1f ms", roundTripMs);
    std::swprintf(pState->readoutCaption, 128, L"latência ida e volta  ·  modo %ls",
                  (mode == 1) ? L"exclusivo" : L"compartilhado");
    pState->readoutColor = (roundTripMs <= 10.0) ? Palette::LatGood
                         : (roundTripMs <= 25.0) ? Palette::LatOk
                                                 : Palette::LatHigh;
    RECT rcHeader = HeaderRect(hWnd, pState);
    InvalidateRect(hWnd, &rcHeader, TRUE);

    // Linha de detalhe abaixo do seletor de buffer
    wchar_t khz[24];
    FormatKHz(khz, 24, sr);
    wchar_t detail[192];
    std::swprintf(detail, 192, L"%ls%ld samples a %ls = %.1f ms por buffer",
                  isAuto ? L"Automático: " : L"", bufFrames, khz, oneWayMs);
    SetWindowTextW(GetDlgItem(hWnd, IDC_STATUS_LATENCY), detail);

    // Destaca o atalho rápido correspondente (nenhum quando em Automático)
    for (const auto& q : kQuickBuffers) {
        SendMessageW(GetDlgItem(hWnd, q.id), BM_SETCHECK,
                     (!isAuto && q.frames == selFrames) ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

static void SelectBufferByFrames(HWND hWnd, DialogState* pState, long frames) {
    HWND hCmbBuf = GetDlgItem(hWnd, IDC_CMB_BUFFER_SIZE);
    int count = (int)SendMessageW(hCmbBuf, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        long f = (long)SendMessageW(hCmbBuf, CB_GETITEMDATA, i, 0);
        if (f == frames) {
            SendMessageW(hCmbBuf, CB_SETCURSEL, i, 0);
            break;
        }
    }
    UpdateLatencyDisplay(hWnd, pState);
}

static void FillRectColor(HDC hdc, const RECT& rc, COLORREF color) {
    SetDCBrushColor(hdc, color);
    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// Pinta cabeçalho, corpo e rodapé. Usado tanto em WM_ERASEBKGND quanto em
// WM_PRINTCLIENT (controles com tema pedem o fundo do pai por esta via).
static void PaintBackground(HWND hWnd, HDC hdc, const DialogState* st) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    const int headerH   = st->S(Layout::HeaderH);
    const int footerTop = st->S(Layout::FooterTop);

    FillRectColor(hdc, RECT{ 0, 0, rc.right, headerH }, Palette::HeaderBg);
    FillRectColor(hdc, RECT{ 0, headerH, rc.right, footerTop }, Palette::BodyBg);
    FillRectColor(hdc, RECT{ 0, footerTop, rc.right, rc.bottom }, Palette::FooterBg);
    FillRectColor(hdc, RECT{ 0, footerTop, rc.right, footerTop + (std::max)(1, st->S(1)) }, Palette::FooterLine);

    const int saved = SaveDC(hdc);
    SetBkMode(hdc, TRANSPARENT);
    const UINT fmtL = DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM;
    const UINT fmtR = DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM;
    const int left  = st->S(Layout::Margin);
    const int right = rc.right - st->S(Layout::Margin);
    const int mid   = rc.right / 2;

    RECT rTitle = { left, st->S(10), mid, st->S(42) };
    SelectObject(hdc, st->hFontTitle);
    SetTextColor(hdc, Palette::HeaderText);
    DrawTextW(hdc, L"nanoAudio", -1, &rTitle, fmtL);

    RECT rSub = { left, st->S(44), mid, st->S(64) };
    SelectObject(hdc, st->hFontNormal);
    SetTextColor(hdc, Palette::HeaderSub);
    DrawTextW(hdc, L"Low-Latency ASIO Driver  ·  v" NANOAUDIO_VERSION_WSTR, -1, &rSub, fmtL);

    RECT rReadout = { mid, st->S(4), right, st->S(44) };
    SelectObject(hdc, st->hFontReadout);
    SetTextColor(hdc, st->readoutColor);
    DrawTextW(hdc, st->readoutText, -1, &rReadout, fmtR);

    RECT rCaption = { mid, st->S(44), right, st->S(64) };
    SelectObject(hdc, st->hFontSmall);
    SetTextColor(hdc, Palette::HeaderSub);
    DrawTextW(hdc, st->readoutCaption, -1, &rCaption, fmtR);

    RestoreDC(hdc, saved);
}

// Abre o nanoAudioUpdater.exe que fica na mesma pasta deste módulo.
static void LaunchUpdater(HWND hWnd, const wchar_t* args) {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LaunchUpdater), &hMod);
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(hMod, path, MAX_PATH * 2);
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L"\\/");
    dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);

    const std::wstring exe = dir + L"\\nanoAudioUpdater.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hWnd, L"O atualizador (nanoAudioUpdater.exe) não foi encontrado na pasta do nanoAudio.\n\n"
                          L"Instale o nanoAudio pelo instalador oficial para receber atualizações.",
                    L"nanoAudio", MB_ICONWARNING | MB_OK);
        return;
    }

    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(hWnd, L"Não foi possível abrir o atualizador.", L"nanoAudio", MB_ICONERROR | MB_OK);
    }
}

static void CreatePanelControls(HWND hWnd, DialogState* st) {
    using namespace Layout;
    const int xL = ColL + Pad;   // conteúdo da coluna esquerda
    const int xR = ColR + Pad;   // conteúdo da coluna direita

    // ---- COLUNA ESQUERDA: SAÍDA ----------------------------------------
    int y = Top;
    const int outH = 137;
    AddControl(hWnd, st, L"BUTTON", L"Saída de áudio", BS_GROUPBOX,
               ColL, y, ColW, outH, IDC_GRP_OUTPUT, st->hFontBold);
    AddLabel(hWnd, st, L"Dispositivo de saída", xL, y + 24, InnerW, IDC_LBL_OUT_DEV);
    AddCombo(hWnd, st, xL, y + 43, InnerW, IDC_CMB_OUT_DEV);
    AddLabel(hWnd, st, L"Canais de saída", xL, y + 76, InnerW, IDC_LBL_OUT_CH);
    AddCombo(hWnd, st, xL, y + 95, 210, IDC_CMB_OUT_CH);

    // ---- COLUNA ESQUERDA: ENTRADA --------------------------------------
    y = Top + outH + 12;
    AddControl(hWnd, st, L"BUTTON", L"Entrada de áudio", BS_GROUPBOX,
               ColL, y, ColW, Bottom - y, IDC_GRP_INPUT, st->hFontBold);
    AddControl(hWnd, st, L"BUTTON", L"Habilitar entrada (full duplex)",
               BS_AUTOCHECKBOX | WS_TABSTOP, xL, y + 24, InnerW, 20, IDC_CHK_ENABLE_IN, st->hFontNormal);
    AddLabel(hWnd, st, L"Dispositivo de entrada", xL, y + 52, InnerW, IDC_LBL_IN_DEV);
    AddCombo(hWnd, st, xL, y + 71, InnerW, IDC_CMB_IN_DEV);
    AddLabel(hWnd, st, L"Canais de entrada", xL, y + 104, InnerW, IDC_LBL_IN_CH);
    AddCombo(hWnd, st, xL, y + 123, 210, IDC_CMB_IN_CH);

    // ---- COLUNA DIREITA: LATÊNCIA E BUFFER -----------------------------
    y = Top;
    const int bufH = 160;
    AddControl(hWnd, st, L"BUTTON", L"Latência e buffer", BS_GROUPBOX,
               ColR, y, ColW, bufH, IDC_GRP_BUFFER, st->hFontBold);
    AddLabel(hWnd, st, L"Atalhos rápidos (samples)", xR, y + 24, InnerW, IDC_LBL_QUICK);

    const int gap = 6;
    const int n   = static_cast<int>(sizeof(kQuickBuffers) / sizeof(kQuickBuffers[0]));
    for (int i = 0; i < n; ++i) {
        const int x0 = xR + (i * (InnerW + gap)) / n;
        const int x1 = xR + ((i + 1) * (InnerW + gap)) / n - gap;
        wchar_t label[16];
        std::swprintf(label, 16, L"%ld", kQuickBuffers[i].frames);
        // Checkbox "pushlike" não-automático: o destaque é controlado só por
        // UpdateLatencyDisplay (um rádio automático se marcaria sozinho ao receber
        // foco pelo Tab e trocaria o buffer sem o usuário pedir).
        AddControl(hWnd, st, L"BUTTON", label, BS_CHECKBOX | BS_PUSHLIKE | WS_TABSTOP, x0, y + 43, x1 - x0, 28,
                   kQuickBuffers[i].id, st->hFontNormal);
    }

    AddLabel(hWnd, st, L"Tamanho do buffer", xR, y + 80, InnerW, IDC_LBL_BUFFER_SIZE);
    AddCombo(hWnd, st, xR, y + 99, InnerW, IDC_CMB_BUFFER_SIZE);
    AddLabel(hWnd, st, L"", xR, y + 130, InnerW, IDC_STATUS_LATENCY, st->hFontSmall);

    // ---- COLUNA DIREITA: FORMATO E MODO --------------------------------
    y = Top + bufH + 12;
    AddControl(hWnd, st, L"BUTTON", L"Formato e modo WASAPI", BS_GROUPBOX,
               ColR, y, ColW, Bottom - y, IDC_GRP_FORMAT, st->hFontBold);
    const int halfW = (InnerW - 14) / 2;
    AddLabel(hWnd, st, L"Sample rate", xR, y + 24, halfW, IDC_LBL_SAMPLE_RATE);
    AddLabel(hWnd, st, L"Resolução", xR + halfW + 14, y + 24, halfW, IDC_LBL_BIT_DEPTH);
    AddCombo(hWnd, st, xR, y + 43, halfW, IDC_CMB_SAMPLE_RATE);
    AddCombo(hWnd, st, xR + halfW + 14, y + 43, halfW, IDC_CMB_BIT_DEPTH);
    AddLabel(hWnd, st, L"Modo de operação", xR, y + 76, InnerW, IDC_LBL_WASAPI_MODE);
    AddCombo(hWnd, st, xR, y + 95, InnerW, IDC_CMB_WASAPI_MODE);

    // ---- RODAPÉ ---------------------------------------------------------
    const int by = FooterTop + (FooterH - BtnH) / 2;
    AddControl(hWnd, st, L"BUTTON", L"Restaurar padrões", BS_PUSHBUTTON | WS_TABSTOP,
               Margin, by, 150, BtnH, IDC_BTN_DEFAULTS, st->hFontNormal);
    if (!st->inDawMode) {
        // Dentro da DAW a DLL está carregada, então a atualização não poderia ser
        // aplicada; o botão só aparece no painel autônomo.
        AddControl(hWnd, st, L"BUTTON", L"Verificar atualizações", BS_PUSHBUTTON | WS_TABSTOP,
                   Margin + 150 + 8, by, 170, BtnH, IDC_BTN_UPDATES, st->hFontNormal);
    }
    AddControl(hWnd, st, L"BUTTON", L"Salvar e aplicar", BS_DEFPUSHBUTTON | WS_TABSTOP,
               ClientW - Margin - 100 - 8 - 140, by, 140, BtnH, IDC_BTN_SAVE, st->hFontBold);
    AddControl(hWnd, st, L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP,
               ClientW - Margin - 100, by, 100, BtnH, IDC_BTN_CANCEL, st->hFontNormal);
}

static void PopulatePanelControls(HWND hWnd, DialogState* st) {
    HWND hCmbOut   = GetDlgItem(hWnd, IDC_CMB_OUT_DEV);
    HWND hCmbOutCh = GetDlgItem(hWnd, IDC_CMB_OUT_CH);
    HWND hChkIn    = GetDlgItem(hWnd, IDC_CHK_ENABLE_IN);
    HWND hCmbIn    = GetDlgItem(hWnd, IDC_CMB_IN_DEV);
    HWND hCmbInCh  = GetDlgItem(hWnd, IDC_CMB_IN_CH);
    HWND hCmbSr    = GetDlgItem(hWnd, IDC_CMB_SAMPLE_RATE);
    HWND hCmbBits  = GetDlgItem(hWnd, IDC_CMB_BIT_DEPTH);
    HWND hCmbBuf   = GetDlgItem(hWnd, IDC_CMB_BUFFER_SIZE);
    HWND hCmbMode  = GetDlgItem(hWnd, IDC_CMB_WASAPI_MODE);
    const DriverUserConfig& cfg = st->workingConfig;
    int idx;

    // Nomes de dispositivos podem ser longos: a lista aberta fica mais larga que o campo
    SendMessageW(hCmbOut, CB_SETDROPPEDWIDTH, st->S(460), 0);
    SendMessageW(hCmbIn,  CB_SETDROPPEDWIDTH, st->S(460), 0);

    // Dispositivos de Saída (item data: índice no vetor, -1 = padrão do Windows)
    int selOut = 0;
    idx = (int)SendMessageW(hCmbOut, CB_ADDSTRING, 0, (LPARAM)L"Padrão do Windows");
    SendMessageW(hCmbOut, CB_SETITEMDATA, idx, (LPARAM)-1);
    for (size_t i = 0; i < st->outputDevices.size(); ++i) {
        idx = (int)SendMessageW(hCmbOut, CB_ADDSTRING, 0, (LPARAM)st->outputDevices[i].name.c_str());
        SendMessageW(hCmbOut, CB_SETITEMDATA, idx, (LPARAM)i);
        if (cfg.outputDeviceId[0] != L'\0' && st->outputDevices[i].id == cfg.outputDeviceId) selOut = idx;
    }
    SendMessageW(hCmbOut, CB_SETCURSEL, selOut, 0);

    // Canais de Saída
    const struct { const wchar_t* label; int ch; } outChans[] = {
        { L"2 canais (estéreo)",      2 },
        { L"4 canais (quad)",         4 },
        { L"6 canais (5.1 surround)", 6 },
        { L"8 canais (7.1 surround)", 8 }
    };
    int selOutCh = 0;
    for (int i = 0; i < 4; ++i) {
        idx = (int)SendMessageW(hCmbOutCh, CB_ADDSTRING, 0, (LPARAM)outChans[i].label);
        SendMessageW(hCmbOutCh, CB_SETITEMDATA, idx, (LPARAM)outChans[i].ch);
        if (cfg.numChannelsOut == outChans[i].ch) selOutCh = idx;
    }
    SendMessageW(hCmbOutCh, CB_SETCURSEL, selOutCh, 0);

    // Habilitar Entrada
    SendMessageW(hChkIn, BM_SETCHECK, cfg.enableInputCapture ? BST_CHECKED : BST_UNCHECKED, 0);

    // Dispositivos de Entrada
    int selIn = 0;
    idx = (int)SendMessageW(hCmbIn, CB_ADDSTRING, 0, (LPARAM)L"Padrão do Windows");
    SendMessageW(hCmbIn, CB_SETITEMDATA, idx, (LPARAM)-1);
    for (size_t i = 0; i < st->inputDevices.size(); ++i) {
        idx = (int)SendMessageW(hCmbIn, CB_ADDSTRING, 0, (LPARAM)st->inputDevices[i].name.c_str());
        SendMessageW(hCmbIn, CB_SETITEMDATA, idx, (LPARAM)i);
        if (cfg.inputDeviceId[0] != L'\0' && st->inputDevices[i].id == cfg.inputDeviceId) selIn = idx;
    }
    SendMessageW(hCmbIn, CB_SETCURSEL, selIn, 0);

    // Canais de Entrada
    const struct { const wchar_t* label; int ch; } inChans[] = {
        { L"1 canal (mono)",     1 },
        { L"2 canais (estéreo)", 2 }
    };
    int selInCh = 1;
    for (int i = 0; i < 2; ++i) {
        idx = (int)SendMessageW(hCmbInCh, CB_ADDSTRING, 0, (LPARAM)inChans[i].label);
        SendMessageW(hCmbInCh, CB_SETITEMDATA, idx, (LPARAM)inChans[i].ch);
        if (cfg.numChannelsIn == inChans[i].ch) selInCh = idx;
    }
    SendMessageW(hCmbInCh, CB_SETCURSEL, selInCh, 0);

    SetInputControlsEnabled(hWnd, cfg.enableInputCapture);

    // Sample Rates (a ordem importa: "Restaurar padrões" seleciona o índice 1 = 48 kHz)
    const long sampleRates[] = { 44100, 48000, 88200, 96000, 192000 };
    int selSr = 1;
    for (int i = 0; i < 5; ++i) {
        wchar_t khz[24];
        FormatKHz(khz, 24, sampleRates[i]);
        idx = (int)SendMessageW(hCmbSr, CB_ADDSTRING, 0, (LPARAM)khz);
        SendMessageW(hCmbSr, CB_SETITEMDATA, idx, (LPARAM)sampleRates[i]);
        if (cfg.preferredSampleRate == sampleRates[i]) selSr = idx;
    }
    SendMessageW(hCmbSr, CB_SETCURSEL, selSr, 0);

    // Resolução de Bits (índice 0 = 32-bit float é o padrão)
    const struct { const wchar_t* label; int bits; } bitDepths[] = {
        { L"32-bit float", 32 },
        { L"24-bit PCM",   24 },
        { L"16-bit PCM",   16 }
    };
    int selBits = 0;
    for (int i = 0; i < 3; ++i) {
        idx = (int)SendMessageW(hCmbBits, CB_ADDSTRING, 0, (LPARAM)bitDepths[i].label);
        SendMessageW(hCmbBits, CB_SETITEMDATA, idx, (LPARAM)bitDepths[i].bits);
        if (cfg.bitDepth == bitDepths[i].bits) selBits = idx;
    }
    SendMessageW(hCmbBits, CB_SETCURSEL, selBits, 0);

    // Tamanhos de Buffer (índice 0 = Automático é o padrão). O tempo em ms é
    // mostrado ao vivo na linha de detalhe, pois depende do sample rate.
    const struct { const wchar_t* label; long frames; } buffers[] = {
        { L"Automático (definido pelo hardware)", 0 },
        { L"32 samples",   32 },
        { L"64 samples",   64 },
        { L"128 samples",  128 },
        { L"256 samples",  256 },
        { L"512 samples",  512 },
        { L"1024 samples", 1024 },
        { L"2048 samples", 2048 }
    };
    int selBuf = 0;
    for (int i = 0; i < 8; ++i) {
        idx = (int)SendMessageW(hCmbBuf, CB_ADDSTRING, 0, (LPARAM)buffers[i].label);
        SendMessageW(hCmbBuf, CB_SETITEMDATA, idx, (LPARAM)buffers[i].frames);
        if (cfg.customBufferFrames == buffers[i].frames) selBuf = idx;
    }
    SendMessageW(hCmbBuf, CB_SETCURSEL, selBuf, 0);

    // Modo WASAPI (item data 0 = compartilhado, 1 = exclusivo)
    idx = (int)SendMessageW(hCmbMode, CB_ADDSTRING, 0, (LPARAM)L"Compartilhado — junto com outros apps");
    SendMessageW(hCmbMode, CB_SETITEMDATA, idx, (LPARAM)0);
    idx = (int)SendMessageW(hCmbMode, CB_ADDSTRING, 0, (LPARAM)L"Exclusivo — acesso direto ao hardware");
    SendMessageW(hCmbMode, CB_SETITEMDATA, idx, (LPARAM)1);
    SendMessageW(hCmbMode, CB_SETCURSEL, cfg.useExclusiveMode ? 1 : 0, 0);

    UpdateLatencyDisplay(hWnd, st);
}

// Mensagem postada no fim do WM_CREATE: com o painel já visível, mostra o aviso
// de atualização se a verificação em segundo plano encontrou uma versão nova.
static const UINT kMsgCheckPendingUpdate = WM_APP + 1;

static bool ShowUpdateNotice(HWND hPanel, const DialogState* st, const std::string& newVersion);

static LRESULT CALLBACK PanelWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    DialogState* pState = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (uMsg) {
    case kMsgCheckPendingUpdate: {
        // Só lê um arquivo local: nenhum acesso à rede acontece dentro da DAW.
        const std::string pending = nanoAudioUpdate::ReadPendingUpdate(NANOAUDIO_VERSION_STR);
        if (!pending.empty() && ShowUpdateNotice(hWnd, pState, pending)) {
            LaunchUpdater(hWnd, L"--interactive --yes");
        }
        return 0;
    }

    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pState = reinterpret_cast<DialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pState));
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    case WM_CREATE: {
        const int dpi = pState->dpi;
        pState->hFontNormal  = MakeUiFont(dpi, 9,  FW_NORMAL);
        pState->hFontBold    = MakeUiFont(dpi, 9,  FW_SEMIBOLD);
        pState->hFontSmall   = MakeUiFont(dpi, 8,  FW_NORMAL);
        pState->hFontTitle   = MakeUiFont(dpi, 16, FW_SEMIBOLD);
        pState->hFontReadout = MakeUiFont(dpi, 20, FW_SEMIBOLD);
        pState->hBodyBrush   = CreateSolidBrush(Palette::BodyBg);
        pState->hFooterBrush = CreateSolidBrush(Palette::FooterBg);

        CreatePanelControls(hWnd, pState);
        PopulatePanelControls(hWnd, pState);
        PostMessageW(hWnd, kMsgCheckPendingUpdate, 0, 0);
        return 0;
    }

    case WM_ERASEBKGND:
        PaintBackground(hWnd, reinterpret_cast<HDC>(wParam), pState);
        return 1;

    case WM_PRINTCLIENT:
        PaintBackground(hWnd, reinterpret_cast<HDC>(wParam), pState);
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        // Só labels, checkbox, group boxes e botões; combos ficam com as cores do sistema.
        HWND hCtl = reinterpret_cast<HWND>(lParam);
        wchar_t cls[32] = {};
        GetClassNameW(hCtl, cls, 32);
        if (lstrcmpiW(cls, L"Static") != 0 && lstrcmpiW(cls, L"Button") != 0) break;

        RECT rc;
        GetWindowRect(hCtl, &rc);
        MapWindowPoints(nullptr, hWnd, reinterpret_cast<POINT*>(&rc), 2);
        const bool inFooter = rc.top >= pState->S(Layout::FooterTop);

        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, inFooter ? Palette::FooterBg : Palette::BodyBg);
        SetTextColor(hdc, (GetDlgCtrlID(hCtl) == IDC_STATUS_LATENCY) ? Palette::MutedText : Palette::BodyText);
        return reinterpret_cast<LRESULT>(inFooter ? pState->hFooterBrush : pState->hBodyBrush);
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        // Botões rápidos de latência
        if (code == BN_CLICKED) {
            for (const auto& q : kQuickBuffers) {
                if (id == q.id) { SelectBufferByFrames(hWnd, pState, q.frames); return 0; }
            }
        }

        if (id == IDC_CHK_ENABLE_IN && code == BN_CLICKED) {
            LRESULT chk = SendMessageW(GetDlgItem(hWnd, IDC_CHK_ENABLE_IN), BM_GETCHECK, 0, 0);
            SetInputControlsEnabled(hWnd, chk == BST_CHECKED);
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

        if (id == IDC_BTN_UPDATES && code == BN_CLICKED) {
            LaunchUpdater(hWnd, L"--interactive");
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
            SetInputControlsEnabled(hWnd, true);
            UpdateLatencyDisplay(hWnd, pState);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY: {
        HFONT fonts[] = { pState->hFontNormal, pState->hFontBold, pState->hFontSmall,
                          pState->hFontTitle, pState->hFontReadout };
        for (HFONT f : fonts) if (f) DeleteObject(f);
        if (pState->hBodyBrush)   DeleteObject(pState->hBodyBrush);
        if (pState->hFooterBrush) DeleteObject(pState->hFooterBrush);
        pState->isOpen = false;
        PostMessageW(hWnd, WM_NULL, 0, 0);
        return 0;
    }
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Ativa os Common Controls v6 (visual moderno do Windows) sem precisar de
// manifesto embutido: usa o manifesto que o próprio shell32.dll já contém.
// Funciona igual no .exe autônomo e na DLL carregada dentro da DAW.
static HANDLE ActivateVisualStyles(ULONG_PTR* cookie) {
    *cookie = 0;
    wchar_t sysDir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return INVALID_HANDLE_VALUE;

    wchar_t shell32[MAX_PATH] = {};
    std::swprintf(shell32, MAX_PATH, L"%ls\\shell32.dll", sysDir);

    ACTCTXW act = {};
    act.cbSize              = sizeof(act);
    act.dwFlags             = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
    act.lpSource            = shell32;
    act.lpAssemblyDirectory = sysDir;
    act.lpResourceName      = MAKEINTRESOURCEW(124);

    HANDLE hCtx = CreateActCtxW(&act);
    if (hCtx == INVALID_HANDLE_VALUE) return hCtx;
    if (!ActivateActCtx(hCtx, cookie)) {
        ReleaseActCtx(hCtx);
        return INVALID_HANDLE_VALUE;
    }
    return hCtx;
}

static void DeactivateVisualStyles(HANDLE hCtx, ULONG_PTR cookie) {
    if (hCtx == INVALID_HANDLE_VALUE) return;
    DeactivateActCtx(0, cookie);
    ReleaseActCtx(hCtx);
}

// ============================================================================
// TELA "ATUALIZAÇÃO DISPONÍVEL" (aparece ao abrir o painel)
// ============================================================================
//
//   +----------------------------------------------+
//   | Atualização disponível               v2.6.1  |  <- cabeçalho escuro
//   | nanoAudio ASIO Driver                         |
//   +----------------------------------------------+
//   | Uma nova versão do nanoAudio está disponível. |
//   | Versão instalada   2.6.0                      |
//   | Nova versão        2.6.1                      |
//   | (dica)                                        |
//   +----------------------------------------------+
//   | [Ver novidades]   [Mais tarde] [Atualizar agora] |
//   +----------------------------------------------+

namespace NoticeLayout {
    constexpr int ClientW   = 460;
    constexpr int HeaderH   = 64;
    constexpr int Margin    = 20;
    constexpr int FooterTop = 214;
    constexpr int FooterH   = 56;
    constexpr int ClientH   = FooterTop + FooterH;   // 270
}

enum NoticeControlIDs {
    IDC_N_UPDATE = 400,
    IDC_N_LATER,
    IDC_N_NOTES,
    IDC_N_TEXT,
    IDC_N_LBL_CUR,
    IDC_N_CUR,
    IDC_N_LBL_NEW,
    IDC_N_NEW,
    IDC_N_HINT
};

struct NoticeState {
    const DialogState* panel = nullptr;   // fontes, pincéis e DPI do painel
    std::wstring       newVersion;
    bool               accepted = false;
    bool               isOpen   = true;
};

static void OpenReleasePage(const std::wstring& version) {
    // shell32 é carregado sob demanda para não adicionar dependências de link à DLL.
    typedef HINSTANCE (WINAPI *ShellExecuteWFn)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    HMODULE hShell = LoadLibraryW(L"shell32.dll");
    if (!hShell) return;
    auto fn = reinterpret_cast<ShellExecuteWFn>(reinterpret_cast<void*>(GetProcAddress(hShell, "ShellExecuteW")));
    const std::wstring url = std::wstring(L"https://github.com/") + NANOAUDIO_WIDEN(NANOAUDIO_GITHUB_REPO) +
                             L"/releases/tag/v" + version;
    if (fn) fn(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void PaintNotice(HWND hWnd, HDC hdc, const NoticeState* ns) {
    const DialogState* st = ns->panel;
    RECT rc;
    GetClientRect(hWnd, &rc);
    const int headerH   = st->S(NoticeLayout::HeaderH);
    const int footerTop = st->S(NoticeLayout::FooterTop);

    FillRectColor(hdc, RECT{ 0, 0, rc.right, headerH }, Palette::HeaderBg);
    FillRectColor(hdc, RECT{ 0, headerH, rc.right, footerTop }, Palette::BodyBg);
    FillRectColor(hdc, RECT{ 0, footerTop, rc.right, rc.bottom }, Palette::FooterBg);
    FillRectColor(hdc, RECT{ 0, footerTop, rc.right, footerTop + (std::max)(1, st->S(1)) }, Palette::FooterLine);

    const int saved = SaveDC(hdc);
    SetBkMode(hdc, TRANSPARENT);
    const UINT fmtL = DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM;
    const UINT fmtR = DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX | DT_BOTTOM;
    const int left  = st->S(NoticeLayout::Margin);
    const int right = rc.right - st->S(NoticeLayout::Margin);

    RECT rTitle = { left, st->S(8), right, st->S(38) };
    SelectObject(hdc, st->hFontTitle);
    SetTextColor(hdc, Palette::HeaderText);
    DrawTextW(hdc, L"Atualização disponível", -1, &rTitle, fmtL);

    RECT rSub = { left, st->S(40), right, st->S(58) };
    SelectObject(hdc, st->hFontSmall);
    SetTextColor(hdc, Palette::HeaderSub);
    DrawTextW(hdc, L"nanoAudio ASIO Driver", -1, &rSub, fmtL);

    const std::wstring tag = L"v" + ns->newVersion;
    RECT rTag = { left, st->S(8), right, st->S(40) };
    SelectObject(hdc, st->hFontReadout);
    SetTextColor(hdc, Palette::LatGood);
    DrawTextW(hdc, tag.c_str(), -1, &rTag, fmtR);

    RestoreDC(hdc, saved);
}

static LRESULT CALLBACK NoticeWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    NoticeState* ns = reinterpret_cast<NoticeState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (uMsg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ns = reinterpret_cast<NoticeState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ns));
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    case WM_CREATE: {
        using namespace NoticeLayout;
        const DialogState* st = ns->panel;
        const int w = ClientW - 2 * Margin;
        AddLabel(hWnd, st, L"Uma nova versão do nanoAudio está disponível.", Margin, 82, w, IDC_N_TEXT, st->hFontBold);

        AddLabel(hWnd, st, L"Versão instalada", Margin, 112, 130, IDC_N_LBL_CUR);
        AddLabel(hWnd, st, NANOAUDIO_VERSION_WSTR, Margin + 130, 112, 120, IDC_N_CUR);
        AddLabel(hWnd, st, L"Nova versão", Margin, 134, 130, IDC_N_LBL_NEW);
        AddLabel(hWnd, st, ns->newVersion.c_str(), Margin + 130, 134, 120, IDC_N_NEW, st->hFontBold);

        const wchar_t* hint = st->inDawMode
            ? L"Para instalar, a DAW precisa ser fechada: o atualizador avisa quando for a hora. "
              L"Suas configurações de áudio são mantidas."
            : L"A instalação leva menos de um minuto e mantém suas configurações de áudio.";
        AddControl(hWnd, st, L"STATIC", hint, SS_LEFT | SS_NOPREFIX, Margin, 166, w, 34, IDC_N_HINT, st->hFontSmall);

        const int by = FooterTop + (FooterH - 30) / 2;
        AddControl(hWnd, st, L"BUTTON", L"Ver novidades", BS_PUSHBUTTON | WS_TABSTOP,
                   Margin, by, 120, 30, IDC_N_NOTES, st->hFontNormal);
        AddControl(hWnd, st, L"BUTTON", L"Mais tarde", BS_PUSHBUTTON | WS_TABSTOP,
                   ClientW - Margin - 150 - 8 - 100, by, 100, 30, IDC_N_LATER, st->hFontNormal);
        HWND hUpdate = AddControl(hWnd, st, L"BUTTON", L"Atualizar agora", BS_DEFPUSHBUTTON | WS_TABSTOP,
                   ClientW - Margin - 150, by, 150, 30, IDC_N_UPDATE, st->hFontBold);
        SetFocus(hUpdate);
        return 0;
    }

    case WM_ERASEBKGND:
        PaintNotice(hWnd, reinterpret_cast<HDC>(wParam), ns);
        return 1;

    case WM_PRINTCLIENT:
        PaintNotice(hWnd, reinterpret_cast<HDC>(wParam), ns);
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HWND hCtl = reinterpret_cast<HWND>(lParam);
        RECT rc;
        GetWindowRect(hCtl, &rc);
        MapWindowPoints(nullptr, hWnd, reinterpret_cast<POINT*>(&rc), 2);
        const bool inFooter = rc.top >= ns->panel->S(NoticeLayout::FooterTop);
        const int id = GetDlgCtrlID(hCtl);
        const bool muted = (id == IDC_N_HINT || id == IDC_N_LBL_CUR || id == IDC_N_LBL_NEW);

        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, inFooter ? Palette::FooterBg : Palette::BodyBg);
        SetTextColor(hdc, muted ? Palette::MutedText : Palette::BodyText);
        return reinterpret_cast<LRESULT>(inFooter ? ns->panel->hFooterBrush : ns->panel->hBodyBrush);
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_N_UPDATE || id == IDOK) {
            ns->accepted = true;
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDC_N_LATER || id == IDCANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDC_N_NOTES) {
            OpenReleasePage(ns->newVersion);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        ns->isOpen = false;
        PostMessageW(hWnd, WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Mostra a tela de aviso (modal sobre o painel). Retorna true se o usuário
// clicou em "Atualizar agora".
static bool ShowUpdateNotice(HWND hPanel, const DialogState* st, const std::string& newVersion) {
    NoticeState ns;
    ns.panel = st;
    ns.newVersion.assign(newVersion.begin(), newVersion.end());   // validado: só dígitos e pontos

    ULONG_PTR actCookie = 0;
    HANDLE hActCtx = ActivateVisualStyles(&actCookie);

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    const wchar_t* kNoticeClass = L"nanoAudioUpdateNoticeClass";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = NoticeWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kNoticeClass;
    RegisterClassExW(&wc);

    const DWORD dwStyle   = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD dwExStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    RECT wr = { 0, 0, st->S(NoticeLayout::ClientW), st->S(NoticeLayout::ClientH) };
    AdjustWindowRectEx(&wr, dwStyle, FALSE, dwExStyle);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    RECT pr = {};
    GetWindowRect(hPanel, &pr);
    const int x = pr.left + ((pr.right - pr.left) - w) / 2;
    const int y = pr.top + ((pr.bottom - pr.top) - h) / 2;

    HWND hNotice = CreateWindowExW(dwExStyle, kNoticeClass, L"nanoAudio — Atualização disponível", dwStyle,
                                   x, y, w, h, hPanel, nullptr, hInstance, &ns);
    DeactivateVisualStyles(hActCtx, actCookie);
    if (!hNotice) {
        UnregisterClassW(kNoticeClass, hInstance);
        return false;
    }

    EnableWindow(hPanel, FALSE);
    ShowWindow(hNotice, SW_SHOW);
    SetForegroundWindow(hNotice);
    SetFocus(GetDlgItem(hNotice, IDC_N_UPDATE));
    UpdateWindow(hNotice);

    MSG msg = {};
    BOOL got = TRUE;
    while (ns.isOpen && (got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(hNotice, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));   // devolve o WM_QUIT ao loop externo

    EnableWindow(hPanel, TRUE);
    SetForegroundWindow(hPanel);
    if (IsWindow(hNotice)) DestroyWindow(hNotice);
    UnregisterClassW(kNoticeClass, hInstance);
    return ns.accepted;
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

    // DPI do sistema (se o processo não for DPI-aware, o Windows retorna 96 e escala sozinho)
    HDC hdcScreen = GetDC(nullptr);
    state.dpi = hdcScreen ? GetDeviceCaps(hdcScreen, LOGPIXELSY) : 96;
    if (hdcScreen) ReleaseDC(nullptr, hdcScreen);
    if (state.dpi <= 0) state.dpi = 96;

    // O contexto de ativação fica ativo apenas durante a criação da janela e dos
    // controles, para não interferir nas mensagens da DAW durante o loop modal.
    ULONG_PTR actCookie = 0;
    HANDLE hActCtx = ActivateVisualStyles(&actCookie);

    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    const wchar_t* kClassName = L"UniversalWasapiAsioPanelClass";

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = PanelWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // fundo pintado em WM_ERASEBKGND
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    const DWORD dwStyle   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    const DWORD dwExStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;

    RECT wr = { 0, 0, state.S(Layout::ClientW), state.S(Layout::ClientH) };
    AdjustWindowRectEx(&wr, dwStyle, FALSE, dwExStyle);
    const int dlgWidth  = wr.right - wr.left;
    const int dlgHeight = wr.bottom - wr.top;

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int posX = work.left + ((work.right - work.left) - dlgWidth) / 2;
    int posY = work.top  + ((work.bottom - work.top) - dlgHeight) / 2;

    if (hWndParent && IsWindow(hWndParent)) {
        RECT parentRect = {};
        GetWindowRect(hWndParent, &parentRect);
        posX = parentRect.left + ((parentRect.right - parentRect.left) - dlgWidth) / 2;
        posY = parentRect.top  + ((parentRect.bottom - parentRect.top) - dlgHeight) / 2;
        if (posX < 0) posX = 0;
        if (posY < 0) posY = 0;
    }

    HWND hWnd = CreateWindowExW(
        dwExStyle,
        kClassName,
        L"nanoAudio ASIO Driver (v" NANOAUDIO_VERSION_WSTR L") — Configurações",
        dwStyle,
        posX, posY, dlgWidth, dlgHeight,
        hWndParent,
        nullptr,
        hInstance,
        &state
    );

    DeactivateVisualStyles(hActCtx, actCookie);

    if (!hWnd) {
        UnregisterClassW(kClassName, hInstance);
        return false;
    }

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
