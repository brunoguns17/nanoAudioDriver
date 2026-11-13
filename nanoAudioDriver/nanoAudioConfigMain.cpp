// ============================================================================
// nanoAudioConfigMain.cpp
// Executável Autônomo do Painel de Controle (Standalone App) — nanoAudio (v2.5)
// Universal WASAPI Low-Latency ASIO Driver
// ============================================================================

#include "nanoAudioConfig.h"

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // Renderização nítida em telas com escala (125%, 150%...). Só no app autônomo:
    // dentro da DAW quem decide o modo de DPI é o processo hospedeiro.
    SetProcessDPIAware();
    CoInitialize(nullptr);
    DriverUserConfig cfg = LoadUserConfig();
    ShowControlPanelWindow(nullptr, &cfg, false);
    CoUninitialize();
    return 0;
}
