// tlaverify — teste diferencial: código recompilado × interpretador, função a função.
//
//   tlaverify <rom> [--frames N] [--per-function K] [--press TECLA@ini-fim ...] [--report arq]
//
// O jogo roda no interpretador (com o roteiro de teclas). Nas primeiras K vezes em que
// cada função recompilada é chamada, o estado completo é copiado para duas instâncias de
// rascunho, que executam a função até ela retornar: uma pelo interpretador, outra pelo
// código recompilado (IRQs mascaradas, vigia de ciclos). Registradores, flags e memórias
// (EWRAM, IWRAM, VRAM, paleta, OAM) precisam ficar idênticos.
//
// Funções que acessam IO podem divergir só por timing (VCOUNT, timers): essas divergências
// são marcadas como inconclusivas, não como falha.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/rom.h"
#include "gba/gba.h"

extern const gba::RecompEntry tla_recomp_table[];
extern const size_t tla_recomp_count;
extern const gba::RecompOverlay tla_recomp_overlays[];
extern const size_t tla_recomp_overlay_count;

namespace {

constexpr uint64_t kBudgetCycles = 16 * 280896; // ~16 quadros por chamada

struct Press {
    uint16_t key;
    uint64_t from, to;
};

enum class Verdict { Ok, Diverge, IoTiming, Timeout, Error, NotCall };

struct Result {
    Verdict v;
    std::string detail;
};

struct Run {
    bool ok = false;
    bool timeout = false;
    std::string error;
    uint64_t io = 0;
};

Run run_interp(gba::Gba& g, uint32_t ret) {
    Run r;
    const uint64_t io0 = g.bus.io_accesses;
    g.cycle_limit = g.cycles + kBudgetCycles;
    try {
        g.resume_until(ret);
        r.ok = true;
    } catch (const gba::StopRequested&) {
        r.timeout = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    r.io = g.bus.io_accesses - io0;
    g.cycle_limit = UINT64_MAX;
    return r;
}

Run run_recomp(gba::Gba& g, uint32_t pc, bool thumb, uint32_t ret) {
    Run r;
    const uint64_t io0 = g.bus.io_accesses;
    g.cycle_limit = g.cycles + kBudgetCycles;
    try {
        g.call_function(pc, thumb);
        if (g.cpu.r[15] != ret) g.resume_until(ret);
        r.ok = true;
    } catch (const gba::StopRequested&) {
        r.timeout = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    r.io = g.bus.io_accesses - io0;
    g.cycle_limit = UINT64_MAX;
    return r;
}

template <size_t N>
std::string diff_mem(const char* name, uint32_t base, const std::array<uint8_t, N>& a, const std::array<uint8_t, N>& b) {
    if (std::memcmp(a.data(), b.data(), N) == 0) return {};
    size_t first = 0, count = 0;
    for (size_t i = 0; i < N; ++i)
        if (a[i] != b[i]) {
            if (!count) first = i;
            ++count;
        }
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s: %zu bytes diferentes, 1º em 0x%08zX (interp %02X, recomp %02X); ", name, count,
                  base + first, a[first], b[first]);
    return buf;
}

std::string compare(const gba::Gba& a, const gba::Gba& b) {
    std::string d;
    for (int i = 0; i < 16; ++i)
        if (a.cpu.r[i] != b.cpu.r[i]) {
            char buf[80];
            std::snprintf(buf, sizeof buf, "r%d: 0x%08X × 0x%08X; ", i, a.cpu.r[i], b.cpu.r[i]);
            d += buf;
        }
    if (a.cpu.cpsr() != b.cpu.cpsr()) {
        char buf[80];
        std::snprintf(buf, sizeof buf, "cpsr: 0x%08X × 0x%08X; ", a.cpu.cpsr(), b.cpu.cpsr());
        d += buf;
    }
    d += diff_mem("EWRAM", 0x02000000, a.bus.ewram, b.bus.ewram);
    d += diff_mem("IWRAM", 0x03000000, a.bus.iwram, b.bus.iwram);
    d += diff_mem("VRAM", 0x06000000, a.bus.vram, b.bus.vram);
    d += diff_mem("PAL", 0x05000000, a.bus.palette, b.bus.palette);
    d += diff_mem("OAM", 0x07000000, a.bus.oam, b.bus.oam);
    return d;
}

uint16_t key_from_name(const std::string& n) {
    static const std::pair<const char*, uint16_t> keys[] = {
        {"A", gba::KEY_A}, {"B", gba::KEY_B}, {"SELECT", gba::KEY_SELECT}, {"START", gba::KEY_START},
        {"RIGHT", gba::KEY_RIGHT}, {"LEFT", gba::KEY_LEFT}, {"UP", gba::KEY_UP}, {"DOWN", gba::KEY_DOWN},
        {"R", gba::KEY_R}, {"L", gba::KEY_L}};
    for (const auto& [name, k] : keys)
        if (n == name) return k;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: tlaverify <rom> [--frames N] [--per-function K] [--press TECLA@ini-fim] [--report arq]\n");
        return 2;
    }
    uint64_t frames = 3000;
    int per_function = 2;
    std::string report_path = "build/verify_report.txt";
    std::vector<Press> script;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::stoull(argv[++i]);
        else if (a == "--per-function" && i + 1 < argc) per_function = std::stoi(argv[++i]);
        else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            const std::string s = argv[++i];
            const auto at = s.find('@'), dash = s.find('-', at);
            Press p{key_from_name(s.substr(0, at)), std::stoull(s.substr(at + 1, dash - at - 1)), 0};
            p.to = dash == std::string::npos ? p.from + 5 : std::stoull(s.substr(dash + 1));
            script.push_back(p);
        } else {
            std::fprintf(stderr, "opção desconhecida: %s\n", a.c_str());
            return 2;
        }
    }

    std::string err;
    auto rom = core::Rom::load(argv[1], &err);
    if (!rom || !rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM inválida ou não suportada %s\n", err.c_str());
        return 1;
    }
    if (tla_recomp_count == 0) {
        std::fprintf(stderr, "erro: build sem código recompilado (rode tlarecomp emit e recompile)\n");
        return 1;
    }

    auto main_g = std::make_unique<gba::Gba>(*rom);     // jogo de referência (interpretador)
    auto interp_g = std::make_unique<gba::Gba>(*rom);   // rascunho: interpretador
    auto recomp_g = std::make_unique<gba::Gba>(*rom);   // rascunho: recompilado
    recomp_g->set_recomp(tla_recomp_table, tla_recomp_count, tla_recomp_overlays, tla_recomp_overlay_count);

    std::unordered_map<uint32_t, int> entries; // endereço|thumb → vezes testada
    for (size_t i = 0; i < tla_recomp_count; ++i) entries[tla_recomp_table[i].addr] = 0;

    std::map<Verdict, size_t> totals;
    std::vector<std::pair<uint32_t, Result>> failures, inconclusive;
    std::unordered_set<uint32_t> tested_funcs;
    bool testing = false;
    uint32_t last_pc = 0;

    main_g->cpu.on_exec = [&](uint32_t pc, bool thumb) {
        if (testing) return;
        const uint32_t prev = last_pc;
        last_pc = pc;
        // Só testa entradas recém-chamadas (bl, ou "mov lr, pc; bx"): lr aponta para
        // logo depois da instrução anterior. Entradas por salto têm lr sem relação.
        if ((main_g->cpu.r[14] & ~1u) != prev + 4) return;
        const uint32_t key = pc | (thumb ? 1u : 0u);
        auto it = entries.find(key);
        if (it == entries.end() || it->second >= per_function) return;
        ++it->second;
        testing = true;
        tested_funcs.insert(key);

        // Estado no início da função (o PC ainda aponta para ela).
        const uint32_t ret = main_g->cpu.r[14] & ~1u;
        interp_g->copy_state_from(*main_g);
        recomp_g->copy_state_from(*main_g);
        interp_g->cpu.r[15] = recomp_g->cpu.r[15] = pc;
        interp_g->cpu.i_ = recomp_g->cpu.i_ = true; // IRQs mascaradas nos dois

        const uint32_t sp0 = main_g->cpu.r[13];
        const Run ri = run_interp(*interp_g, ret);
        Result res;
        if (ri.ok && interp_g->cpu.r[13] != sp0) {
            // "Retornou" com outra pilha: era um bl usado como salto, não uma chamada.
            res = {Verdict::NotCall, "bl usado como salto (pilha diferente no retorno)"};
        } else if (!ri.ok) {
            res = {ri.timeout ? Verdict::Timeout : Verdict::Error, ri.timeout ? "interpretador não retornou" : ri.error};
        } else {
            const Run rr = run_recomp(*recomp_g, pc, thumb, ret);
            if (!rr.ok) {
                res = {Verdict::Diverge, rr.timeout ? "recompilado não retornou" : "recompilado: " + rr.error};
            } else {
                const std::string d = compare(*interp_g, *recomp_g);
                if (d.empty()) res = {Verdict::Ok, {}};
                else if (ri.io || rr.io) res = {Verdict::IoTiming, d};
                else res = {Verdict::Diverge, d};
            }
        }
        ++totals[res.v];
        if (res.v == Verdict::Diverge) failures.emplace_back(key, res);
        else if (res.v != Verdict::Ok) inconclusive.emplace_back(key, res);
        testing = false;
    };

    const auto t0 = std::chrono::steady_clock::now();
    auto keys_for = [&](uint64_t f) {
        uint16_t k = 0;
        for (const auto& p : script)
            if (f >= p.from && f <= p.to) k |= p.key;
        return k;
    };
    main_g->set_keys(keys_for(0));
    main_g->run([&] {
        const uint64_t f = main_g->frame_count;
        if (f % 500 == 0)
            std::printf("  quadro %llu: %zu funções testadas, %zu divergências\n", static_cast<unsigned long long>(f),
                        tested_funcs.size(), failures.size());
        main_g->set_keys(keys_for(f));
        return f < frames;
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const size_t total = totals[Verdict::Ok] + totals[Verdict::Diverge] + totals[Verdict::IoTiming] +
                         totals[Verdict::Timeout] + totals[Verdict::Error] + totals[Verdict::NotCall];
    std::printf("\n%zu execuções testadas (%zu funções distintas de %zu) em %.1f s\n", total, tested_funcs.size(),
                tla_recomp_count, secs);
    std::printf("  idênticas            : %zu\n", totals[Verdict::Ok]);
    std::printf("  DIVERGENTES          : %zu\n", totals[Verdict::Diverge]);
    std::printf("  inconclusivas (IO)   : %zu\n", totals[Verdict::IoTiming]);
    std::printf("  inconclusivas (tempo): %zu\n", totals[Verdict::Timeout]);
    std::printf("  inconclusivas (erro) : %zu\n", totals[Verdict::Error]);
    std::printf("  não eram chamadas    : %zu\n", totals[Verdict::NotCall]);

    std::ofstream rep(report_path);
    auto dump = [&](const char* title, const std::vector<std::pair<uint32_t, Result>>& list, size_t show) {
        rep << "== " << title << " (" << list.size() << ")\n";
        size_t n = 0;
        for (const auto& [key, r] : list) {
            char head[64];
            std::snprintf(head, sizeof head, "0x%08X %s: ", key & ~1u, (key & 1) ? "thumb" : "arm");
            rep << head << r.detail << "\n";
            if (n++ < show) std::printf("  %s%s\n", head, r.detail.substr(0, 200).c_str());
        }
    };
    if (!failures.empty()) std::printf("\nDivergências:\n");
    dump("DIVERGENTES", failures, 15);
    dump("inconclusivas", inconclusive, 0);
    std::printf("Relatório: %s\n", report_path.c_str());
    return failures.empty() ? 0 : 1;
}
