# Universal WASAPI Low-Latency ASIO Driver

Driver ASIO universal em C++ (COM In-Process Server 64-bit/32-bit) projetado para Windows 10 e Windows 11, construído sobre a API nativa [**IAudioClient3 (WASAPI Low Latency)**](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/low-latency-audio) e compatível com a especificação binária do [**Steinberg ASIO SDK 2.3**](https://www.steinberg.net/developers/asiosdk-open/).

## O Que Este Driver Resolve em Relação ao ASIO4ALL

| Problema no ASIO4ALL | Solução Implementada no Código | 
| ----- | ----- | 
| **1. Bloqueio do Áudio do Sistema (Monopólio)** | Inicializa o endpoint via `IAudioClient3::InitializeSharedAudioStream` ([WASAPI](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi) Shared Mode), permitindo usar a DAW simultaneamente com YouTube, Spotify e Discord | 
| **2. Instabilidade ("X" Vermelho)** | Evita o sequestro exclusivo WDM-KS e gerencia os buffers de captura e renderização sem travar quando outros aplicativos abrem a placa de som | 
| **3. Buffer Dinâmico conforme a Placa do Usuário** | Consulta `IAudioClient3::GetSharedModeEnginePeriod` em `ProbeHardwareCapabilities()` para detectar `minPeriodInFrames`, `defaultPeriodInFrames` e `maxPeriodInFrames` da placa de som do usuário e repassa via `IASIO::getBufferSize()` | 
| **4. Consumo Elevado de CPU** | Loop *Event-Driven* (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`) sem alocações na thread de tempo real e registrado no agendador de kernel [**MMCSS**](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service) com perfil `"Pro Audio"` (`AVRT_PRIORITY_CRITICAL`) | 
| **5. Conflito de Sample Rate (44.1 kHz vs 48 kHz)** | Ativa automaticamente as flags [**`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`**](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants) e `AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY` quando a taxa da DAW difere da taxa nativa do Windows | 

## Estrutura de Arquivos do Projeto

* `nanoAudioDriver.h`: Declaração da interface binária COM `IASIO` (compatível com a VTable oficial da Steinberg), estruturas de buffer/tempo e definição da classe `UniversalWasapiAsioDriver`.

* `nanoAudioDriver.cpp`: Implementação completa do motor WASAPI (`IAudioClient3`), detecção dinâmica de buffer por hardware, thread de áudio *Pro Audio*, painel de controle Win32 (`IASIO::controlPanel`) e auto-registro no Registro do Windows (`DllRegisterServer`).

* `CMakeLists.txt`: Configuração de build CMake para Visual Studio 2019/2022.

## Como Compilar no Windows

### Usando CMake (Recomendado)

Abra o terminal (PowerShell ou Prompt de Comando com Visual Studio Build Tools instalado) na pasta dos arquivos e execute:

```
mkdir build
cd build
cmake .. -A x64
cmake --build . --config Release

```

O arquivo `nanoAudioDriver.dll` será gerado em `build/Release/nanoAudioDriver.dll`.

### Usando Diretamente o Compilador MSVC (`cl.exe`)

No **x64 Native Tools Command Prompt for VS**, execute:

```
cl /LD /O2 /EHsc /DUNICODE /D_UNICODE nanoAudioDriver.cpp ole32.lib uuid.lib avrt.lib user32.lib gdi32.lib advapi32.lib

```

## Como Instalar o Driver

### 1. Instalador Gráfico Oficial (.exe) — Recomendado para Usuários Finais
Basta executar o instalador executável gerado:
* **`Output\nanoAudio_Setup_v2.5.0.exe`**

O instalador:
* Instala os drivers nativos **64-bit** e **32-bit** em `C:\Program Files\nanoAudio`.
* Registra automaticamente o servidor COM e as entradas ASIO para todas as DAWs (64-bit e 32-bit).
* Adiciona a opção de desinstalação padrão no menu "Configurações > Aplicativos > Aplicativos Instalados" (ou Painel de Controle) do Windows.

---

### 2. Gerar Novo Instalador Executável com 1 Clique (Para Desenvolvedores)
Dê um duplo clique no script:
* **`Compilar_e_Gerar_Instalador.bat`**

Ele compila as versões estáticas (64-bit e 32-bit) e gera automaticamente o arquivo `UniversalWasapiAsio_Setup_v2.3.0.exe` dentro da pasta `Output\`.

---

### 3. Instalação Rápida Direta via Linha de Comando (Sem Instalador)
Se preferir instalar diretamente a partir dos arquivos locais:
* Execute como Administrador: **`Instalar_e_Compilar_Driver.bat`**
* Para remover: **`Desinstalar_Driver.bat`**

---

## Painel de Controle Gráfico Integrado

O driver inclui um Painel de Controle nativo em Win32 que pode ser aberto tanto de **dentro de qualquer DAW** (ao clicar em *Control Panel* / *Driver Settings*) quanto diretamente pelo **Menu Iniciar** ou área de trabalho via **`nanoAudioControlPanel.exe`**.

### O que pode ser configurado:
1. **🎧 Saída de Áudio (Playback):** Escolha qual placa ou interface reproduz o som (ex: *BEHRINGER USB Audio*, *Alto-falantes Realtek*, etc.) ou mantenha no *Padrão do Windows*.
2. **Canais de Saída:** Selecione entre **2 Canais (Estéreo)**, **4 Canais (Quad)**, **6 Canais (5.1)** ou **8 Canais (7.1)**.
3. **🎙️ Entrada de Áudio (Recording):** Escolha o microfone ou interface de captura (ex: *BEHRINGER USB In*, *Microfone Realtek*), com opção de ativar/desativar a captura (Full Duplex).
4. **Canais de Entrada:** Selecione **1 Canal (Mono)** ou **2 Canais (Estéreo In 1 / In 2)**.
5. **🎛️ Sample Rate (Taxa de Amostragem):** 44.1 kHz (44100 Hz), 48.0 kHz (48000 Hz), 88.2 kHz, 96.0 kHz ou 192.0 kHz.
6. **Resolução de Bits (Bit Depth):**
   * **32-bit Float:** Máxima faixa dinâmica sem risco de clipping digital interno (Recomendado para DAWs).
   * **24-bit PCM:** Alta definição para gravação e interfaces profissionais.
   * **16-bit PCM:** Compatibilidade máxima (Padrão CD).
7. **⏱️ Tamanho do Buffer & Latência:** 
   * Automático (Detectado pelo Hardware WASAPI)
   * 64 samples (~1.3 ms a 48kHz — Ultra Baixa Latência)
   * 128 samples (~2.7 ms a 48kHz — Muito Baixa Latência)
   * 256 samples (~5.3 ms a 48kHz — Balanceado / Gravação)
   * 512 samples (~10.7 ms a 48kHz — Mixagem Estável)
   * 1024 samples (~21.3 ms a 48kHz — Projetos Pesados)
   * 2048 samples (~42.7 ms a 48kHz — Máxima Estabilidade)
8. **Modo WASAPI:**
   * **Modo Compartilhado (Shared):** Não bloqueia o Windows. Permite tocar na DAW e ouvir YouTube, Spotify e Discord simultaneamente.
   * **Modo Exclusivo (Exclusive):** Monopoliza a interface para garantir a menor latência possível sem interferência do sistema operacional.

---

## Como Configurar nas Principais DAWs

* **Reaper, FL Studio, Cubase e Studio One:** Abra as preferências de áudio da DAW, escolha **ASIO** como tipo de driver e selecione **Universal WASAPI Low-Latency ASIO**. Clique no botão **Control Panel / Show ASIO Panel** para abrir o Painel de Controle gráfico e configurar entrada, saída, sample rate, resolução e latência. Ao clicar em **Salvar & Aplicar**, o driver reconfigura a DAW automaticamente via `kAsioResetRequest`.

* **Ableton Live e Pro Tools:** Desinstale o ASIO4ALL antes de abrir o Ableton Live ou o Pro Tools para evitar que o ASIO4ALL trave o dispositivo de áudio ao ser sondado em paralelo. O driver já opera com formato unificado `ASIOSTFloat32LSB` na entrada e na saída, exigido pelo Pro Tools.