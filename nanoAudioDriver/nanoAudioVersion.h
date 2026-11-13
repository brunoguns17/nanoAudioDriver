// ============================================================================
// nanoAudioVersion.h
// Fonte única da versão do nanoAudio e do repositório usado pelo atualizador.
//
// NÃO edite a versão à mão para publicar: use Publicar_Release.bat, que
// atualiza esta linha, compila, gera o instalador e cria a release no GitHub.
// ============================================================================

#pragma once

#define NANOAUDIO_VERSION_STR "2.6.0"

// Repositório público de onde o atualizador baixa as releases.
#define NANOAUDIO_GITHUB_REPO "brunoguns17/nanoAudioDriver"

// Nome fixo do manifesto anexado a cada release. Instalações antigas sempre
// procuram exatamente este nome em releases/latest/download/ — não renomeie.
#define NANOAUDIO_UPDATE_MANIFEST "update-manifest.txt"

#define NANOAUDIO_WIDEN2(x) L##x
#define NANOAUDIO_WIDEN(x)  NANOAUDIO_WIDEN2(x)

#ifndef NANOAUDIO_VERSION_WSTR
#define NANOAUDIO_VERSION_WSTR NANOAUDIO_WIDEN(NANOAUDIO_VERSION_STR)
#endif
