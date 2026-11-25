#pragma once
// Comparação diferencial interpretador × código recompilado (usada por tlaverify e test_synth).

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "gba/gba.h"

namespace difftest {

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

template <typename F> Run guarded(gba::Gba& g, uint64_t budget, F&& body) {
    Run r;
    const uint64_t io0 = g.bus.io_accesses;
    g.cycle_limit = g.cycles + budget;
    try {
        body();
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

inline std::string compare(const gba::Gba& a, const gba::Gba& b) {
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

class Verifier {
public:
    Verifier(const core::Rom& rom, const gba::RecompEntry* table, size_t count, const gba::RecompOverlay* overlays,
             size_t overlay_count)
        : interp_(std::make_unique<gba::Gba>(rom)), recomp_(std::make_unique<gba::Gba>(rom)) {
        recomp_->set_recomp(table, count, overlays, overlay_count);
    }

    // Copia `base` para as duas instâncias, aplica `setup`, executa a função em `pc` até
    // r15 == ret nos dois modos e compara.
    template <typename Setup>
    Result evaluate(const gba::Gba& base, uint32_t pc, bool thumb, uint32_t ret, uint64_t budget, Setup&& setup) {
        interp_->copy_state_from(base);
        recomp_->copy_state_from(base);
        for (gba::Gba* g : {interp_.get(), recomp_.get()}) {
            setup(*g);
            g->cpu.r[15] = pc;
            g->cpu.t_ = thumb;
            g->cpu.i_ = true; // IRQs mascaradas
            g->cpu.halted = false;
        }
        const uint32_t sp0 = interp_->cpu.r[13];
        const Run ri = guarded(*interp_, budget, [&] { interp_->resume_until(ret); });
        if (!ri.ok)
            return {ri.timeout ? Verdict::Timeout : Verdict::Error, ri.timeout ? "interpretador não retornou" : ri.error};
        if (interp_->cpu.r[13] != sp0) return {Verdict::NotCall, "retornou com outra pilha (bl usado como salto)"};
        const Run rr = guarded(*recomp_, budget, [&] {
            recomp_->call_function(pc, thumb);
            if (recomp_->cpu.r[15] != ret) recomp_->resume_until(ret);
        });
        if (!rr.ok) {
            char where[200];
            std::snprintf(where, sizeof where,
                          " (último pc sincronizado 0x%08X; acessos a IO: interp %llu, recomp %llu)", recomp_->cpu.r[15],
                          static_cast<unsigned long long>(ri.io), static_cast<unsigned long long>(rr.io));
            const std::string d = (rr.timeout ? "recompilado não retornou" : "recompilado: " + rr.error) + where;
            return {(ri.io || rr.io) ? Verdict::IoTiming : Verdict::Diverge, d};
        }
        const std::string d = compare(*interp_, *recomp_);
        if (d.empty()) return {Verdict::Ok, {}};
        if (ri.io || rr.io) return {Verdict::IoTiming, d};
        return {Verdict::Diverge, d};
    }

private:
    std::unique_ptr<gba::Gba> interp_, recomp_;
};

struct Tally {
    std::map<Verdict, size_t> totals;
    std::vector<std::pair<uint32_t, Result>> failures, inconclusive;
    std::unordered_set<uint32_t> funcs;

    void add(uint32_t key, const Result& r) {
        ++totals[r.v];
        funcs.insert(key);
        if (r.v == Verdict::Diverge) failures.emplace_back(key, r);
        else if (r.v != Verdict::Ok) inconclusive.emplace_back(key, r);
    }

    size_t total() const {
        size_t n = 0;
        for (const auto& [v, c] : totals) n += c;
        return n;
    }

    void print(const char* title, size_t universe, double secs) const {
        auto get = [&](Verdict v) {
            auto it = totals.find(v);
            return it == totals.end() ? size_t(0) : it->second;
        };
        std::printf("\n%s: %zu execuções (%zu funções distintas de %zu) em %.1f s\n", title, total(), funcs.size(),
                    universe, secs);
        std::printf("  idênticas            : %zu\n", get(Verdict::Ok));
        std::printf("  DIVERGENTES          : %zu\n", get(Verdict::Diverge));
        std::printf("  inconclusivas (IO)   : %zu\n", get(Verdict::IoTiming));
        std::printf("  inconclusivas (tempo): %zu\n", get(Verdict::Timeout));
        std::printf("  inconclusivas (erro) : %zu\n", get(Verdict::Error));
        std::printf("  não eram chamadas    : %zu\n", get(Verdict::NotCall));
    }

    void dump(std::ofstream& rep, const char* title, size_t show) const {
        auto section = [&](const char* name, const std::vector<std::pair<uint32_t, Result>>& list, size_t n_show) {
            rep << "== " << title << " — " << name << " (" << list.size() << ")\n";
            size_t n = 0;
            for (const auto& [key, r] : list) {
                char head[64];
                std::snprintf(head, sizeof head, "0x%08X %s: ", key & ~1u, (key & 1) ? "thumb" : "arm");
                rep << head << r.detail << "\n";
                if (n++ < n_show) std::printf("  %s%s\n", head, r.detail.substr(0, 220).c_str());
            }
        };
        if (!failures.empty()) std::printf("Divergências (%s):\n", title);
        section("DIVERGENTES", failures, show);
        section("inconclusivas", inconclusive, 0);
    }
};

} // namespace difftest
