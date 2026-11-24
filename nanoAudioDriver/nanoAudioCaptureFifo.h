// ============================================================================
// nanoAudioCaptureFifo.h
// FIFO de captura com reamostrador (sinc + Kaiser) e controle automático de nível.
//
// Por que existe: a entrada e a saída quase nunca andam no mesmo passo. Os pacotes
// chegam em rajadas (a Behringer USB, por exemplo, entrega saltos de ~512 frames) e,
// quando os dispositivos são diferentes, os relógios também são. O FIFO:
//
//   1. Começa com folga e, depois de observar um segundo, descarta (com crossfade)
//      o que sobrou além da margem. O nível de referência é sempre o MÍNIMO da janela:
//      medir no pico da rajada deixava o FIFO sem margem e causava um ciclo de
//      "falta -> silêncio -> excesso -> corte".
//   2. Se ainda assim faltar áudio, aumenta a própria margem (aprende o jitter real).
//   3. Compensa a diferença de relógio variando a taxa de leitura em no máximo 0,05%.
//
// Quando a taxa é a mesma e não há correção ativa, a leitura é cópia direta (bit a bit).
// Não aloca memória depois de Configure(): seguro para a thread de tempo real.
// ============================================================================

#pragma once

#include <emmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

class CaptureFifo {
public:
    // burstFrames: frames consumidos de uma vez a cada despertar da saída (taxa de saída).
    // packetFrames: tamanho típico dos pacotes de captura (taxa de saída).
    // marginFrames: margem já aprendida em execuções anteriores (0 = usar a inicial).
    void Configure(long channels, double inRate, double outRate, long blockFrames,
                   long burstFrames, long packetFrames, long marginFrames) {
        m_channels = std::max<long>(1, channels);
        m_block    = std::max<long>(1, blockFrames);
        m_inRate   = inRate;
        m_ratio    = inRate / outRate;

        BuildTable();

        m_cap = std::max<size_t>(65536, static_cast<size_t>(m_block * m_ratio) * 8 + 4096);
        m_buf.assign(m_cap * static_cast<size_t>(m_channels), 0.0f);
        m_tmp.assign(static_cast<size_t>(m_block) * static_cast<size_t>(m_channels), 0.0f);
        m_tmpPtr.resize(static_cast<size_t>(m_channels));
        for (long c = 0; c < m_channels; ++c) m_tmpPtr[c] = &m_tmp[static_cast<size_t>(c) * m_block];
        m_last.assign(static_cast<size_t>(m_channels), 0.0f);

        m_maxMargin = inRate * 0.025;
        m_margin    = std::clamp(marginFrames * m_ratio, inRate * 0.004, m_maxMargin);
        m_deadband  = std::max(m_block * m_ratio, inRate * 0.002);
        m_hardLimit = std::max(m_block * m_ratio * 2.0, inRate * 0.030);
        m_headroom  = (burstFrames + m_block + packetFrames * 1.5) * m_ratio;
        m_window    = std::max<long>(1, static_cast<long>(outRate * 0.5 / m_block));
        Clear();
    }

    long Channels() const { return m_channels; }

    // Maior pacote aceito por PrepareWrite (pacotes maiores devem ser divididos).
    size_t MaxWrite() const { return m_cap / 4; }

    // Garante espaço para "frames" e devolve, via WritePtr(), onde gravar cada canal.
    void PrepareWrite(size_t frames) {
        if (m_w + frames <= m_cap) return;
        const size_t base = m_r - (m_half - 1);
        const size_t keep = m_w - base;
        if (keep + frames > m_cap) {   // ninguém está lendo (host travado): recomeça
            m_skipped += Level();
            Clear();
            return;
        }
        for (long c = 0; c < m_channels; ++c) {
            float* p = &m_buf[static_cast<size_t>(c) * m_cap];
            std::memmove(p, p + base, keep * sizeof(float));
        }
        m_w = keep;
        m_r = m_half - 1;
    }
    float* WritePtr(long ch) { return &m_buf[static_cast<size_t>(ch) * m_cap + m_w]; }
    void   CommitWrite(size_t frames) { m_w += frames; }

    // true se o próximo ReadBlock() vai faltar áudio. O chamador pode esperar um pouco pelo pacote
    // atrasado em vez de aceitar a falta.
    bool Starving() const {
        return m_running && Level() < static_cast<size_t>(m_frac + (m_block - 1) * m_ratio * (1.0 + m_corr)) + 1;
    }

    // Entrega um bloco (planar). Retorna false se entregou silêncio (pré-carga ou falta).
    bool ReadBlock(float* const* out) {
        if (!m_running) {
            if (static_cast<double>(Level()) < m_margin + m_headroom) { EmitFadeOut(out); return false; }
            m_running  = true;
            m_fadeIn   = true;
            m_settling = kSettleWindows;
            m_corr     = 0.0;
            m_skip     = 0;
            BeginWindow();
        }

        const double step = m_ratio * (1.0 + m_corr);
        const size_t need = static_cast<size_t>(m_frac + (m_block - 1) * step) + 1;
        if (Level() < need) {
            ++m_underruns;
            m_margin  = std::min(m_margin + m_inRate * 0.003, m_maxMargin);
            m_running = false;
            EmitFadeOut(out);
            return false;
        }

        if (m_skip > 0 && Level() >= need + m_skip) {
            // Descarta o excesso misturando a posição antiga com a nova (sem estalo).
            const size_t r0   = m_r;
            const double f0   = m_frac;
            Pull(out, step);
            m_r    = r0 + m_skip;
            m_frac = f0;
            Pull(m_tmpPtr.data(), step);
            const float inc = 1.0f / static_cast<float>(m_block);
            for (long c = 0; c < m_channels; ++c) {
                float w = 0.0f;
                for (long f = 0; f < m_block; ++f, w += inc) out[c][f] += (m_tmpPtr[c][f] - out[c][f]) * w;
            }
            m_skipped += m_skip;
            m_skip = 0;
            BeginWindow();
        } else {
            Pull(out, step);
        }

        if (m_fadeIn) {
            const long n = std::min<long>(m_block, 64);
            for (long c = 0; c < m_channels; ++c)
                for (long f = 0; f < n; ++f) out[c][f] *= static_cast<float>(f) / static_cast<float>(n);
            m_fadeIn = false;
        }
        for (long c = 0; c < m_channels; ++c) m_last[c] = out[c][m_block - 1];

        m_winMin = std::min(m_winMin, Level());
        if (--m_winLeft <= 0) EndWindow();
        return true;
    }

    uint64_t Underruns() const     { return m_underruns; }
    uint64_t SkippedFrames() const { return m_skipped; }
    double   Correction() const    { return m_corr; }
    bool     Running() const       { return m_running; }

    // Frames de entrada já recebidos que ainda serão lidos (inclui o que o filtro olha à frente).
    long PendingFrames() const { return static_cast<long>(m_w - m_r); }

    // Margem atual em frames na taxa de saída (reaproveitada no próximo start()).
    long MarginFrames() const { return static_cast<long>(m_margin / m_ratio + 0.5); }

private:
    static constexpr long kPhases = 256;
    static constexpr long kMaxTaps = 192;
    static constexpr int  kSettleWindows = 3;
    static constexpr double kMaxCorrection = 0.0005;

    size_t Level() const {
        const size_t floor = m_r + m_half;
        return (m_w > floor) ? m_w - floor : 0;
    }

    void Clear() {
        // Histórico de zeros para o filtro poder olhar para trás desde a 1ª amostra
        std::fill(m_buf.begin(), m_buf.end(), 0.0f);
        m_w = m_r = m_half - 1;
        m_frac    = 0.0;
        m_running = false;
        m_fadeIn  = false;
        m_settling = kSettleWindows;
        m_corr    = 0.0;
        m_skip    = 0;
        BeginWindow();
    }

    void BeginWindow() {
        m_winMin  = SIZE_MAX;
        m_winLeft = (m_settling > 0) ? m_window * 2 : m_window;   // observa mais nos primeiros ajustes
    }

    void EndWindow() {
        const double excess = static_cast<double>(m_winMin) - m_margin;
        double target = 0.0;
        if (m_settling > 0) {
            // Primeiros segundos: o arranque dos drivers é irregular, então o ajuste se repete
            --m_settling;
            if (excess > m_deadband * 0.5) m_skip = static_cast<size_t>(excess);
        } else if (excess > m_hardLimit) {
            m_skip = static_cast<size_t>(excess);
        } else if (excess > m_deadband) {
            target = std::min((excess - m_deadband) / (m_inRate * 4.0), kMaxCorrection);
        } else if (excess < -m_margin * 0.25) {
            target = std::max((excess + m_margin * 0.25) / (m_inRate * 4.0), -kMaxCorrection);
        }
        // Transição suave: a variação de afinação fica muito abaixo do audível
        m_corr += (target - m_corr) * 0.3;
        if (target == 0.0 && std::fabs(m_corr) < 1e-6) m_corr = 0.0;
        BeginWindow();
    }

    void EmitFadeOut(float* const* out) {
        const long n = std::min<long>(m_block, 64);
        for (long c = 0; c < m_channels; ++c) {
            for (long f = 0; f < n; ++f) out[c][f] = m_last[c] * (1.0f - static_cast<float>(f + 1) / static_cast<float>(n));
            if (m_block > n) std::memset(out[c] + n, 0, static_cast<size_t>(m_block - n) * sizeof(float));
            m_last[c] = 0.0f;
        }
    }

    void Pull(float* const* out, double step) {
        if (m_frac == 0.0 && step == 1.0) {
            for (long c = 0; c < m_channels; ++c)
                std::memcpy(out[c], &m_buf[static_cast<size_t>(c) * m_cap + m_r], static_cast<size_t>(m_block) * sizeof(float));
            m_r += static_cast<size_t>(m_block);
            return;
        }

        alignas(16) float coef[kMaxTaps];
        double pos = m_frac;
        for (long f = 0; f < m_block; ++f, pos += step) {
            const size_t ip    = static_cast<size_t>(pos);
            const double phase = (pos - static_cast<double>(ip)) * kPhases;
            const long   p     = std::min<long>(static_cast<long>(phase), kPhases - 1);
            const float* a     = &m_table[static_cast<size_t>(p) * m_taps];
            const float* b     = a + m_taps;
            const __m128 t     = _mm_set1_ps(static_cast<float>(phase - p));
            for (size_t k = 0; k < m_taps; k += 4) {
                const __m128 va = _mm_loadu_ps(a + k);
                _mm_storeu_ps(coef + k, _mm_add_ps(va, _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(b + k), va), t)));
            }
            const size_t first = m_r + ip - (m_half - 1);
            for (long c = 0; c < m_channels; ++c) {
                const float* x = &m_buf[static_cast<size_t>(c) * m_cap + first];
                __m128 acc = _mm_setzero_ps();
                for (size_t k = 0; k < m_taps; k += 4)
                    acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(x + k), _mm_loadu_ps(coef + k)));
                acc = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));
                acc = _mm_add_ss(acc, _mm_shuffle_ps(acc, acc, 1));
                out[c][f] = _mm_cvtss_f32(acc);
            }
        }
        const size_t adv = static_cast<size_t>(pos);
        m_r   += adv;
        m_frac = pos - static_cast<double>(adv);
    }

    // I0 modificada de Bessel (janela de Kaiser)
    static double BesselI0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k) {
            term *= (x * 0.5 / k) * (x * 0.5 / k);
            sum  += term;
            if (term < sum * 1e-14) break;
        }
        return sum;
    }

    void BuildTable() {
        const double pi   = 3.14159265358979323846;
        const double down = std::max(1.0, m_ratio);
        m_taps = static_cast<size_t>(std::min<long>(kMaxTaps, static_cast<long>(std::ceil(12.0 * down)) * 4));
        m_half = m_taps / 2;
        // Mesma taxa nominal: corte em Nyquist (atraso fracionário puro). Conversão real: um
        // pouco abaixo, para atenuar imagens e aliasing.
        const double fc   = (std::fabs(m_ratio - 1.0) < 1e-9) ? 1.0 : 0.95 / down;
        const double beta = 9.0;
        const double i0b  = BesselI0(beta);

        m_table.assign(static_cast<size_t>(kPhases + 1) * m_taps, 0.0f);
        std::vector<double> row(m_taps);
        for (long p = 0; p <= kPhases; ++p) {
            const double frac = static_cast<double>(p) / kPhases;
            double sum = 0.0;
            for (size_t k = 0; k < m_taps; ++k) {
                const double d = static_cast<double>(k) - static_cast<double>(m_half - 1) - frac;
                const double u = d / static_cast<double>(m_half);
                const double w = (std::fabs(u) < 1.0) ? BesselI0(beta * std::sqrt(1.0 - u * u)) / i0b : 0.0;
                const double s = (std::fabs(d) < 1e-12) ? 1.0 : std::sin(pi * fc * d) / (pi * fc * d);
                row[k] = s * w;
                sum   += row[k];
            }
            for (size_t k = 0; k < m_taps; ++k)
                m_table[static_cast<size_t>(p) * m_taps + k] = static_cast<float>(row[k] / sum);
        }
    }

    long   m_channels = 1;
    long   m_block    = 64;
    double m_inRate   = 48000.0;
    double m_ratio    = 1.0;      // frames de entrada por frame de saída

    std::vector<float> m_table;   // (kPhases + 1) x m_taps
    size_t m_taps = 48, m_half = 24;

    std::vector<float>  m_buf;    // planar: canais x m_cap
    size_t m_cap = 0, m_w = 0, m_r = 0;
    double m_frac = 0.0;
    std::vector<float>  m_tmp;
    std::vector<float*> m_tmpPtr;
    std::vector<float>  m_last;

    double m_margin = 0.0, m_maxMargin = 0.0, m_deadband = 0.0, m_hardLimit = 0.0, m_headroom = 0.0;
    double m_corr = 0.0;
    size_t m_skip = 0, m_winMin = SIZE_MAX;
    long   m_window = 1, m_winLeft = 1;
    int    m_settling = 0;
    bool   m_running = false, m_fadeIn = false;

    uint64_t m_underruns = 0, m_skipped = 0;
};
