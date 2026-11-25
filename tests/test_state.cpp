// Testes de save state: ida e volta byte a byte e rejeição transacional de arquivos ruins.
//
//   test_state <rom.gba>

#include <cstdio>
#include <memory>
#include <stdexcept>

#include "core/rom.h"
#include "gba/gba.h"

extern const gba::RecompEntry tla_recomp_table[];
extern const size_t tla_recomp_count;
extern const gba::RecompOverlay tla_recomp_overlays[];
extern const size_t tla_recomp_overlay_count;

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "OK    " : "FALHOU", what);
    if (!ok) ++g_fail;
}

std::unique_ptr<gba::Gba> make(const core::Rom& rom) {
    auto g = std::make_unique<gba::Gba>(rom);
    g->set_recomp(tla_recomp_table, tla_recomp_count, tla_recomp_overlays, tla_recomp_overlay_count);
    return g;
}

void run_frames(gba::Gba& g, uint64_t until, uint16_t keys_at_900 = gba::KEY_START) {
    g.run([&] {
        const uint64_t f = g.frame_count;
        g.set_keys(f >= 900 && f < 906 ? keys_at_900 : 0);
        return f < until;
    });
}

bool throws(gba::Gba& g, const std::vector<uint8_t>& bytes) {
    try {
        g.load_state(bytes);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: test_state <rom.gba>\n");
        return 2;
    }
    auto rom = core::Rom::load(argv[1]);
    if (!rom || !rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM inválida ou não suportada\n");
        return 2;
    }

    // Estado no meio do jogo (título), com o código recompilado em execução.
    auto a = make(*rom);
    run_frames(*a, 1200);
    const std::vector<uint8_t> s1 = a->save_state();
    std::printf("estado: %zu bytes, quadro %llu\n", s1.size(), static_cast<unsigned long long>(a->frame_count));

    // Ida e volta: carregar numa instância nova e salvar de novo dá os mesmos bytes.
    auto b = make(*rom);
    b->load_state(s1);
    check(b->save_state() == s1, "ida e volta: salvar(carregar(s)) == s");
    check(b->frame_count == a->frame_count && b->cpu.r == a->cpu.r, "quadro e registradores restaurados");
    check(b->frame() == a->frame(), "tela restaurada");

    // Dois carregamentos do mesmo estado seguem idênticos (determinismo após carregar).
    auto c = make(*rom);
    c->load_state(s1);
    run_frames(*b, 1500);
    run_frames(*c, 1500);
    check(b->save_state() == c->save_state(), "300 quadros depois de carregar: dois carregamentos idênticos");

    // Carregar DENTRO de run(), com código recompilado em execução (desempilha via exceção):
    // precisa chegar ao mesmo estado que carregar antes de rodar.
    auto d = make(*rom);
    bool loaded = false;
    d->run([&] {
        const uint64_t f = d->frame_count;
        if (!loaded && f == 1000) {
            loaded = true;
            d->load_state_and_restart(s1); // não retorna
        }
        d->set_keys(f >= 900 && f < 906 ? gba::KEY_START : 0);
        return f < 1500;
    });
    check(loaded && d->save_state() == c->save_state(), "carregar no meio de run() == carregar antes de rodar");

    // Um arquivo ruim dentro de run() não interrompe a execução (o jogo segue de onde estava).
    auto e = make(*rom);
    bool rejected = false;
    e->run([&] {
        const uint64_t f = e->frame_count;
        if (f == 600) {
            try {
                e->load_state_and_restart({1, 2, 3});
            } catch (const std::runtime_error&) {
                rejected = true;
            }
        }
        return f < 700;
    });
    check(rejected && e->frame_count == 700, "estado inválido dentro de run() é rejeitado e o jogo continua");

    // Arquivos ruins são rejeitados e não alteram o estado atual (transacional).
    const std::vector<uint8_t> before = c->save_state();
    std::vector<uint8_t> truncated(s1.begin(), s1.begin() + static_cast<long>(s1.size() / 2));
    check(throws(*c, truncated) && c->save_state() == before, "estado truncado rejeitado sem efeito");
    std::vector<uint8_t> extra = s1;
    extra.push_back(0);
    check(throws(*c, extra) && c->save_state() == before, "bytes sobrando rejeitados sem efeito");
    std::vector<uint8_t> bad_magic = s1;
    bad_magic[0] = 'X';
    check(throws(*c, bad_magic) && c->save_state() == before, "assinatura inválida rejeitada");
    std::vector<uint8_t> bad_version = s1;
    bad_version[8] ^= 0xFF;
    check(throws(*c, bad_version) && c->save_state() == before, "versão diferente rejeitada");
    std::vector<uint8_t> bad_rom = s1;
    bad_rom[12] ^= 0x01;
    check(throws(*c, bad_rom) && c->save_state() == before, "estado de outra ROM rejeitado");
    check(throws(*c, {}) && c->save_state() == before, "arquivo vazio rejeitado");

    std::printf("%s\n", g_fail ? "HÁ FALHAS" : "todos os testes de save state passaram");
    return g_fail ? 1 : 0;
}
