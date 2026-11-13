// ============================================================================
// nanoAudioUpdater.cpp
// Atualizador automático do nanoAudio (executável separado, nunca roda dentro
// da DAW).
//
// Fluxo:
//   1. Baixa  https://github.com/<repo>/releases/latest/download/update-manifest.txt
//      (o GitHub redireciona "latest" para a release publicada mais recente).
//   2. Compara a versão do manifesto com a versão compilada neste executável.
//   3. Se for mais nova: baixa o instalador da release, confere o SHA-256,
//      verifica se nenhum programa (DAW, painel) está usando o driver e executa
//      o instalador do Inno Setup em modo silencioso.
//
// Modos:
//   --silent       Tarefa agendada (SYSTEM). Sem nenhuma janela; registra em
//                  %ProgramData%\nanoAudio\update.log. Se o driver estiver em
//                  uso, adia para a próxima execução.
//   --interactive  (padrão) Aberto pelo painel: pergunta antes de instalar.
//   --dry-run      Faz tudo (consulta, download, hash) mas não instala.
//   --log <arq>    Caminho alternativo para o log.
//
// Códigos de saída: 0 = já atualizado, 1 = erro, 2 = atualização iniciada
// (ou disponível, em --dry-run), 3 = adiada (driver em uso), 4 = recusada.
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <restartmanager.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cwchar>

#include "nanoAudioVersion.h"

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

// Permite compilar uma build de teste que "finge" ser uma versão antiga.
#ifndef NANOAUDIO_UPDATER_CURRENT
#define NANOAUDIO_UPDATER_CURRENT NANOAUDIO_VERSION_STR
#endif

namespace {

enum ExitCode { kUpToDate = 0, kError = 1, kUpdateStarted = 2, kBusy = 3, kDeclined = 4 };

const wchar_t* kTitle           = L"nanoAudio — Atualização";
const wchar_t* kPanelClassName  = L"UniversalWasapiAsioPanelClass";
const size_t   kMaxManifestSize = 16 * 1024;
const size_t   kMaxInstallerSize = 200u * 1024u * 1024u;

struct Options {
    bool         silent  = false;
    bool         dryRun  = false;
    std::wstring logPath;
};

std::wstring g_logPath;

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vswprintf(buf, 2048, fmt, ap);
    va_end(ap);
    return buf;
}

void Log(const std::wstring& msg) {
    if (g_logPath.empty()) return;

    // Rotação simples: acima de 256 KB o log recomeça.
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad) &&
        (fad.nFileSizeHigh > 0 || fad.nFileSizeLow > 256 * 1024)) {
        DeleteFileW(g_logPath.c_str());
    }

    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string line = Narrow(Format(L"%04u-%02u-%02u %02u:%02u:%02u  ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond) + msg) + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}

std::wstring EnvPath(const wchar_t* var) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? std::wstring(buf, n) : std::wstring();
}

bool EnsureDir(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring Win32Error(DWORD code) {
    wchar_t* text = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE hWinHttp = GetModuleHandleW(L"winhttp.dll");
    if (code >= 12000 && code <= 12200 && hWinHttp) flags = (flags & ~FORMAT_MESSAGE_FROM_SYSTEM) | FORMAT_MESSAGE_FROM_HMODULE;
    FormatMessageW(flags, hWinHttp, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring msg = text ? text : L"";
    if (text) LocalFree(text);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
    return Format(L"erro %lu%ls%ls", code, msg.empty() ? L"" : L": ", msg.c_str());
}

// ---------------------------------------------------------------------------
// Versões e manifesto
// ---------------------------------------------------------------------------

bool ParseVersion(const std::string& s, long out[3]) {
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

int CompareVersions(const long a[3], const long b[3]) {
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    return 0;
}

struct Manifest {
    std::string version;
    std::string file;
    std::string sha256;
};

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Formato (chaves desconhecidas são ignoradas para permitir evoluir o arquivo):
//   version=2.6.1
//   file=nanoAudio_Setup_v2.6.1.exe
//   sha256=<64 hex>
bool ParseManifest(std::string text, Manifest& m, std::wstring& err) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        text.erase(0, 3);

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = Trim(text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos));
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        for (auto& c : key) c = (char)tolower((unsigned char)c);
        if (key == "version")     m.version = val;
        else if (key == "file")   m.file = val;
        else if (key == "sha256") { for (auto& c : val) c = (char)tolower((unsigned char)c); m.sha256 = val; }
    }

    long v[3];
    if (!ParseVersion(m.version, v)) { err = L"manifesto com versão inválida"; return false; }

    bool fileOk = m.file.size() >= 5 && m.file.size() <= 100;
    for (char c : m.file) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            fileOk = false;
    }
    if (fileOk) {
        std::string ext = m.file.substr(m.file.size() - 4);
        for (auto& c : ext) c = (char)tolower((unsigned char)c);
        fileOk = (ext == ".exe") && m.file[0] != '.';
    }
    if (!fileOk) { err = L"manifesto com nome de arquivo inválido"; return false; }

    bool hashOk = m.sha256.size() == 64;
    for (char c : m.sha256) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hashOk = false;
    }
    if (!hashOk) { err = L"manifesto com SHA-256 inválido"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// HTTPS (WinHTTP)
// ---------------------------------------------------------------------------

struct InternetHandle {
    HINTERNET h = nullptr;
    explicit InternetHandle(HINTERNET v = nullptr) : h(v) {}
    ~InternetHandle() { if (h) WinHttpCloseHandle(h); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

// Baixa uma URL HTTPS para a memória (memOut) ou para um arquivo (fileOut).
bool HttpsGet(const std::wstring& url, size_t maxBytes, std::string* memOut, HANDLE fileOut, std::wstring& err) {
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    URL_COMPONENTS uc = {};
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath      = path;
    uc.dwUrlPathLength  = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = L"URL inválida (somente HTTPS é aceito)";
        return false;
    }

    const std::wstring ua = std::wstring(L"nanoAudioUpdater/") + NANOAUDIO_WIDEN(NANOAUDIO_UPDATER_CURRENT);
    InternetHandle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        session.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) { err = Win32Error(GetLastError()); return false; }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);

    InternetHandle connect(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connect.h) { err = Win32Error(GetLastError()); return false; }

    InternetHandle request(WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH));
    if (!request.h) { err = Win32Error(GetLastError()); return false; }

    // Redirecionamentos HTTPS -> HTTPS são seguidos automaticamente (o padrão do
    // WinHTTP proíbe HTTPS -> HTTP).
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        err = Win32Error(GetLastError());
        return false;
    }

    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        err = (status == 404) ? L"arquivo não encontrado no GitHub (HTTP 404)" : Format(L"resposta HTTP %lu", status);
        return false;
    }

    std::vector<char> buf(64 * 1024);
    size_t total = 0;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buf.data(), (DWORD)buf.size(), &read)) {
            err = Win32Error(GetLastError());
            return false;
        }
        if (read == 0) break;
        total += read;
        if (total > maxBytes) { err = L"arquivo maior que o limite permitido"; return false; }
        if (memOut) memOut->append(buf.data(), read);
        if (fileOut) {
            DWORD written = 0;
            if (!WriteFile(fileOut, buf.data(), read, &written, nullptr) || written != read) {
                err = Win32Error(GetLastError());
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SHA-256 (BCrypt)
// ---------------------------------------------------------------------------

bool Sha256OfHandle(HANDLE hFile, std::string& hexOut) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if ((LONG)BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if ((LONG)BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        LARGE_INTEGER zero = {};
        SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN);
        std::vector<unsigned char> buf(64 * 1024);
        ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(hFile, buf.data(), (DWORD)buf.size(), &read, nullptr)) { ok = false; break; }
            if (read == 0) break;
            if ((LONG)BCryptHashData(hash, buf.data(), read, 0) < 0) { ok = false; break; }
        }
        unsigned char digest[32];
        if (ok && (LONG)BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
            static const char* hex = "0123456789abcdef";
            hexOut.clear();
            for (unsigned char b : digest) { hexOut += hex[b >> 4]; hexOut += hex[b & 15]; }
        } else {
            ok = false;
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---------------------------------------------------------------------------
// Arquivos em uso (Restart Manager)
// ---------------------------------------------------------------------------

// Retorna os nomes dos programas que estão usando algum dos arquivos.
std::vector<std::wstring> ProgramsUsingFiles(const std::vector<std::wstring>& files) {
    std::vector<std::wstring> names;
    std::vector<LPCWSTR> existing;
    for (const auto& f : files) {
        if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) existing.push_back(f.c_str());
    }
    if (existing.empty()) return names;

    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) == ERROR_SUCCESS) {
        if (RmRegisterResources(session, (UINT)existing.size(), existing.data(), 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
            UINT needed = 0, count = 0;
            DWORD reasons = 0;
            DWORD r = RmGetList(session, &needed, &count, nullptr, &reasons);
            if (r == ERROR_MORE_DATA && needed > 0) {
                std::vector<RM_PROCESS_INFO> info(needed);
                count = needed;
                if (RmGetList(session, &needed, &count, info.data(), &reasons) == ERROR_SUCCESS) {
                    for (UINT i = 0; i < count; ++i) names.push_back(info[i].strAppName);
                }
            }
        }
        RmEndSession(session);
    }

    // Reforço: um arquivo mapeado/aberto sem compartilhamento recusa escrita.
    if (names.empty()) {
        for (LPCWSTR f : existing) {
            HANDLE h = CreateFileW(f, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); continue; }
            DWORD e = GetLastError();
            if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_USER_MAPPED_FILE) {
                const wchar_t* base = wcsrchr(f, L'\\');
                names.push_back(base ? base + 1 : f);
            }
        }
    }
    return names;
}

void ClosePanelWindows() {
    std::vector<DWORD> pids;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, kPanelClassName, nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        pids.push_back(pid);
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
    // Aguarda até 5 s para o painel autônomo encerrar e liberar o .exe.
    for (DWORD pid : pids) {
        HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (hp) { WaitForSingleObject(hp, 5000); CloseHandle(hp); }
    }
}

int Message(const Options& opt, const std::wstring& text, UINT flags) {
    if (opt.silent) return 0;
    return MessageBoxW(nullptr, text.c_str(), kTitle, flags | MB_SETFOREGROUND | MB_TOPMOST);
}

// ---------------------------------------------------------------------------
// Fluxo principal
// ---------------------------------------------------------------------------

int RunUpdate(const Options& opt) {
    const std::wstring repo    = NANOAUDIO_WIDEN(NANOAUDIO_GITHUB_REPO);
    const std::wstring current = NANOAUDIO_WIDEN(NANOAUDIO_UPDATER_CURRENT);
    Log(Format(L"Verificando atualizações (versão instalada %ls, modo %ls%ls)",
               current.c_str(), opt.silent ? L"silencioso" : L"interativo", opt.dryRun ? L", dry-run" : L""));

    // 1. Manifesto da última release
    const std::wstring manifestUrl = L"https://github.com/" + repo + L"/releases/latest/download/" +
                                     NANOAUDIO_WIDEN(NANOAUDIO_UPDATE_MANIFEST);
    std::string manifestText;
    std::wstring err;
    if (!HttpsGet(manifestUrl, kMaxManifestSize, &manifestText, nullptr, err)) {
        Log(L"Falha ao baixar o manifesto: " + err);
        Message(opt, L"Não foi possível verificar atualizações.\n\nDetalhes: " + err +
                     L"\n\nVerifique sua conexão com a internet e tente novamente.", MB_ICONWARNING | MB_OK);
        return kError;
    }

    Manifest m;
    if (!ParseManifest(manifestText, m, err)) {
        Log(L"Manifesto rejeitado: " + err);
        Message(opt, L"O servidor de atualizações respondeu com dados inválidos.\n\nDetalhes: " + err, MB_ICONWARNING | MB_OK);
        return kError;
    }

    long vCur[3], vNew[3];
    ParseVersion(NANOAUDIO_UPDATER_CURRENT, vCur);
    ParseVersion(m.version, vNew);
    const std::wstring newVer = Widen(m.version);
    if (CompareVersions(vNew, vCur) <= 0) {
        Log(L"Nenhuma atualização: versão publicada " + newVer);
        Message(opt, L"Você já está usando a versão mais recente do nanoAudio (" + current + L").", MB_ICONINFORMATION | MB_OK);
        return kUpToDate;
    }
    Log(L"Nova versão disponível: " + newVer);

    if (!opt.silent) {
        int answer = Message(opt,
            L"Uma nova versão do nanoAudio está disponível.\n\n"
            L"Versão instalada:  " + current + L"\n"
            L"Nova versão:  " + newVer + L"\n\n"
            L"Baixar e instalar agora?\nO painel de controle será fechado durante a instalação (alterações não salvas serão perdidas).",
            MB_ICONQUESTION | MB_YESNO);
        if (answer != IDYES) { Log(L"Usuário recusou a atualização."); return kDeclined; }
    }

    // 2. Pasta de download: no modo silencioso fica ao lado do atualizador
    //    (Program Files, gravável só por administradores); no interativo, no
    //    perfil do próprio usuário.
    std::wstring dlDir;
    if (opt.silent) {
        dlDir = ExeDir() + L"\\Updates";
    } else {
        std::wstring base = EnvPath(L"LOCALAPPDATA") + L"\\nanoAudio";
        EnsureDir(base);
        dlDir = base + L"\\Updates";
    }
    if (!EnsureDir(dlDir)) {
        err = Win32Error(GetLastError());
        Log(L"Não foi possível criar a pasta de download " + dlDir + L": " + err);
        Message(opt, L"Não foi possível preparar o download.\n\nDetalhes: " + err, MB_ICONERROR | MB_OK);
        return kError;
    }

    // Remove instaladores de tentativas anteriores
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((dlDir + L"\\*.exe").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do { DeleteFileW((dlDir + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    // 3. Download do instalador
    const std::wstring fileName  = Widen(m.file);
    const std::wstring localPath = dlDir + L"\\" + fileName;
    const std::wstring setupUrl  = L"https://github.com/" + repo + L"/releases/download/v" + newVer + L"/" + fileName;
    Log(L"Baixando " + setupUrl);
    {
        HANDLE hOut = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hOut == INVALID_HANDLE_VALUE) {
            err = Win32Error(GetLastError());
            Log(L"Não foi possível criar " + localPath + L": " + err);
            Message(opt, L"Não foi possível salvar o instalador.\n\nDetalhes: " + err, MB_ICONERROR | MB_OK);
            return kError;
        }
        bool ok = HttpsGet(setupUrl, kMaxInstallerSize, nullptr, hOut, err);
        CloseHandle(hOut);
        if (!ok) {
            DeleteFileW(localPath.c_str());
            Log(L"Falha no download do instalador: " + err);
            Message(opt, L"Não foi possível baixar a atualização.\n\nDetalhes: " + err, MB_ICONWARNING | MB_OK);
            return kError;
        }
    }

    // 4. Verificação de integridade. O arquivo fica aberto (sem permitir escrita
    //    por terceiros) desde o hash até o instalador ser iniciado.
    HANDLE hSetup = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string actualHash;
    if (hSetup == INVALID_HANDLE_VALUE || !Sha256OfHandle(hSetup, actualHash) || actualHash != m.sha256) {
        Log(L"SHA-256 não confere. Esperado " + Widen(m.sha256) + L", obtido " + Widen(actualHash));
        if (hSetup != INVALID_HANDLE_VALUE) CloseHandle(hSetup);
        DeleteFileW(localPath.c_str());
        Message(opt, L"O arquivo baixado não passou na verificação de integridade (SHA-256).\n"
                     L"A atualização foi cancelada por segurança.", MB_ICONERROR | MB_OK);
        return kError;
    }
    Log(L"SHA-256 conferido: " + Widen(actualHash));

    if (opt.dryRun) {
        CloseHandle(hSetup);
        Log(L"Dry-run: instalador verificado em " + localPath + L" (não executado).");
        Message(opt, L"[dry-run] Versão " + newVer + L" baixada e verificada em:\n" + localPath, MB_ICONINFORMATION | MB_OK);
        return kUpdateStarted;
    }

    // 5. O driver não pode estar em uso (DAW aberta) nem o painel aberto.
    if (!opt.silent) ClosePanelWindows();
    const std::wstring dir = ExeDir();
    const std::vector<std::wstring> watched = {
        dir + L"\\nanoAudioDriver.dll",
        dir + L"\\nanoAudioDriver32.dll",
        dir + L"\\nanoAudioControlPanel.exe",
    };
    for (;;) {
        std::vector<std::wstring> busy = ProgramsUsingFiles(watched);
        if (busy.empty()) break;
        std::wstring list;
        for (const auto& n : busy) list += L"   •  " + n + L"\n";
        Log(L"Driver em uso por: " + list);
        if (opt.silent) {
            CloseHandle(hSetup);
            Log(L"Atualização adiada para a próxima verificação.");
            return kBusy;
        }
        int answer = Message(opt, L"O nanoAudio está em uso por:\n\n" + list +
                                  L"\nFeche esses programas e clique em Tentar novamente.", MB_ICONWARNING | MB_RETRYCANCEL);
        if (answer != IDRETRY) { CloseHandle(hSetup); return kDeclined; }
    }

    // 6. Executa o instalador. /NOCLOSEAPPLICATIONS garante que o Inno Setup
    //    nunca feche a DAW do usuário.
    std::wstring setupLog = (opt.silent ? EnvPath(L"ProgramData") : EnvPath(L"LOCALAPPDATA")) + L"\\nanoAudio\\install.log";
    std::wstring args = L"/SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /SP- /LOG=\"" + setupLog + L"\"";
    bool started = false;
    DWORD launchError = 0;
    if (opt.silent) {
        std::wstring cmd = L"\"" + localPath + L"\" /VERYSILENT " + args;
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        // Sai do job da tarefa agendada, para o instalador sobreviver ao fim deste processo.
        started = CreateProcessW(localPath.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                                 CREATE_BREAKAWAY_FROM_JOB, nullptr, dlDir.c_str(), &si, &pi) != FALSE;
        if (!started) {
            started = CreateProcessW(localPath.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                                     0, nullptr, dlDir.c_str(), &si, &pi) != FALSE;
        }
        if (started) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        else launchError = GetLastError();
    } else {
        std::wstring params = L"/SILENT " + args;
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb       = L"runas";
        sei.lpFile       = localPath.c_str();
        sei.lpParameters = params.c_str();
        sei.lpDirectory  = dlDir.c_str();
        sei.nShow        = SW_SHOWNORMAL;
        started = ShellExecuteExW(&sei) != FALSE;
        if (started) { if (sei.hProcess) CloseHandle(sei.hProcess); }
        else launchError = GetLastError();
    }
    CloseHandle(hSetup);

    if (!started) {
        err = Win32Error(launchError);
        Log(L"Falha ao iniciar o instalador: " + err);
        if (launchError != ERROR_CANCELLED) {  // UAC recusado pelo usuário: sem mensagem extra
            Message(opt, L"Não foi possível iniciar o instalador.\n\nDetalhes: " + err, MB_ICONERROR | MB_OK);
        }
        return kError;
    }
    Log(L"Instalador da versão " + newVer + L" iniciado.");
    // Encerra logo em seguida: o instalador precisa substituir este executável.
    return kUpdateStarted;
}

Options ParseArgs() {
    Options opt;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--silent")                     opt.silent = true;
        else if (a == L"--interactive")           opt.silent = false;
        else if (a == L"--dry-run")               opt.dryRun = true;
        else if (a == L"--log" && i + 1 < argc)   opt.logPath = argv[++i];
    }
    if (argv) LocalFree(argv);
    return opt;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    Options opt = ParseArgs();

    if (!opt.logPath.empty()) {
        g_logPath = opt.logPath;
    } else {
        std::wstring base = EnvPath(opt.silent ? L"ProgramData" : L"LOCALAPPDATA");
        if (!base.empty()) {
            base += L"\\nanoAudio";
            if (EnsureDir(base)) g_logPath = base + L"\\update.log";
        }
    }

    // Uma execução por vez (inicialização + verificação diária podem coincidir).
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Global\\nanoAudioUpdater");
    if (!hMutex) hMutex = CreateMutexW(nullptr, FALSE, L"Local\\nanoAudioUpdater");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"Outra verificação já está em andamento.");
        Message(opt, L"Já existe uma verificação de atualização em andamento.", MB_ICONINFORMATION | MB_OK);
        CloseHandle(hMutex);
        return kBusy;
    }

    int code = RunUpdate(opt);
    if (hMutex) CloseHandle(hMutex);
    return code;
}
