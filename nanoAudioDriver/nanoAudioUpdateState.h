// ============================================================================
// nanoAudioUpdateState.h
// Estado "há atualização disponível", compartilhado entre o atualizador e o
// painel de controle.
//
// A verificação em segundo plano (nanoAudioUpdater.exe --silent, tarefa
// agendada) grava a versão nova em <pasta de instalação>\update-available.txt.
// O painel (inclusive dentro da DAW) apenas LÊ esse arquivo local ao abrir;
// nenhum acesso à rede acontece dentro do processo da DAW.
// ============================================================================

#pragma once

#include <windows.h>
#include <string>

namespace nanoAudioUpdate {

// "x.y.z" com até 5 dígitos por parte.
inline bool ParseVersion(const std::string& s, long out[3]) {
    int part = 0;
    size_t digits = 0;
    out[0] = out[1] = out[2] = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            if (++digits > 5) return false;
            out[part] = out[part] * 10 + (c - '0');
        } else if (c == '.' && digits > 0 && part < 2) {
            ++part;
            digits = 0;
        } else {
            return false;
        }
    }
    return part == 2 && digits > 0;
}

inline int CompareVersions(const long a[3], const long b[3]) {
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

// true se "candidate" for uma versão válida e estritamente mais nova que "current".
inline bool IsNewer(const std::string& candidate, const std::string& current) {
    long a[3], b[3];
    return ParseVersion(candidate, a) && ParseVersion(current, b) && CompareVersions(a, b) > 0;
}

// Pasta de instalação = pasta do módulo que contém este código (o .exe do
// atualizador/painel ou a DLL do driver carregada pela DAW). Em Program Files
// só administradores gravam e todos leem: o SYSTEM grava o aviso ali sem que
// um usuário comum possa adulterá-lo (ao contrário de %ProgramData%).
inline std::wstring InstallDir() {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallDir), &hMod);
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(hMod, path, MAX_PATH * 2);
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : dir.substr(0, slash);
}

inline std::wstring StateFilePath() {
    std::wstring dir = InstallDir();
    return dir.empty() ? std::wstring() : dir + L"\\update-available.txt";
}

// Versão anunciada pela última verificação, se for mais nova que "current".
// Retorna "" quando não há atualização pendente.
inline std::string ReadPendingUpdate(const std::string& current) {
    const std::wstring path = StateFilePath();
    if (path.empty()) return {};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[257] = {};
    DWORD read = 0;
    ReadFile(h, buf, 256, &read, nullptr);
    CloseHandle(h);

    std::string text(buf, read);
    size_t pos = text.find("version=");
    if (pos == std::string::npos) return {};
    pos += 8;
    size_t end = text.find_first_of("\r\n", pos);
    std::string version = text.substr(pos, (end == std::string::npos ? text.size() : end) - pos);
    return IsNewer(version, current) ? version : std::string();
}

// Grava de forma atômica (arquivo temporário + MoveFileEx).
inline bool WritePendingUpdate(const std::string& version) {
    const std::wstring path = StateFilePath();
    if (path.empty()) return false;
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::string text = "version=" + version + "\r\n";
    DWORD written = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

inline void ClearPendingUpdate() {
    const std::wstring path = StateFilePath();
    if (!path.empty()) DeleteFileW(path.c_str());
}

} // namespace nanoAudioUpdate
