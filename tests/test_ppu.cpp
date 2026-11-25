// Teste de equivalência da composição da PPU.
//
// Compara Ppu::compose (otimizada) com a implementação de referência abaixo — a versão
// original, pixel a pixel, da primeira PPU do projeto — em milhares de configurações
// aleatórias: modos, prioridades, janelas (WIN0/WIN1/OBJ), efeitos de cor, sprites
// semitransparentes e conteúdo das camadas. As capturas do jogo não exercitam janelas nem
// sprites semitransparentes; este teste cobre esses caminhos.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>

#include "core/rom.h"
#include "gba/gba.h"

namespace gba {

struct PpuTestAccess {
    static auto& bg(Ppu& p) { return p.bg_line_; }
    static auto& obj(Ppu& p) { return p.obj_line_; }
    static auto& prio(Ppu& p) { return p.obj_prio_; }
    static auto& semi(Ppu& p) { return p.obj_semi_; }
    static auto& win(Ppu& p) { return p.obj_window_; }
    static uint8_t& prio_used(Ppu& p) { return p.obj_prio_used_; }
    static void compose(Ppu& p, int line) { p.compose(line); }
};

} // namespace gba

namespace {

using gba::PpuTestAccess;
constexpr uint16_t kOpaque = 0x8000;
constexpr int W = gba::kScreenW;

uint32_t to_argb(uint16_t c) {
    const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

// Composição de referência: cópia fiel da primeira implementação (por pixel).
void reference_compose(gba::Gba& g, int line, uint32_t* out) {
    gba::Io& io = g.io;
    gba::Ppu& ppu = g.ppu;
    const uint16_t dispcnt = io.reg(0);
    uint16_t backdrop;
    std::memcpy(&backdrop, g.bus.palette.data(), 2);
    backdrop &= 0x7FFF;
    const int mode = dispcnt & 7;
    uint8_t bg_mask = (dispcnt >> 8) & 0xF;
    if (mode == 1) bg_mask &= 0x7;
    else if (mode == 2) bg_mask &= 0xC;
    else if (mode >= 3) bg_mask &= 0x4;
    const bool obj_on = dispcnt & 0x1000;
    int bg_prio[4];
    for (int b = 0; b < 4; ++b) bg_prio[b] = io.reg(0x08 + b * 2) & 3;
    const bool win0 = dispcnt & 0x2000, win1 = dispcnt & 0x4000, winobj = dispcnt & 0x8000;
    const bool any_win = win0 || win1 || winobj;
    const uint16_t winin = io.reg(0x48), winout = io.reg(0x4A);
    auto in_win = [&](int which, int x) {
        const uint16_t hreg = io.reg(0x40 + which * 2), vreg = io.reg(0x44 + which * 2);
        const int x1 = hreg >> 8, x2 = hreg & 0xFF, y1 = vreg >> 8, y2 = vreg & 0xFF;
        const bool inx = x1 <= x2 ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
        const bool iny = y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
        return inx && iny;
    };
    const uint16_t bldcnt = io.reg(0x50), bldalpha = io.reg(0x52);
    const int blend_mode = (bldcnt >> 6) & 3;
    const int eva = std::min(16, bldalpha & 31), evb = std::min(16, (bldalpha >> 8) & 31);
    const int evy = std::min(16, io.reg(0x54) & 31);
    auto& bgl = PpuTestAccess::bg(ppu);
    auto& objl = PpuTestAccess::obj(ppu);
    auto& objp = PpuTestAccess::prio(ppu);
    auto& objs = PpuTestAccess::semi(ppu);
    auto& objw = PpuTestAccess::win(ppu);

    for (int x = 0; x < W; ++x) {
        uint8_t mask = 0x3F;
        if (any_win) {
            if (win0 && in_win(0, x)) mask = winin & 0x3F;
            else if (win1 && in_win(1, x)) mask = (winin >> 8) & 0x3F;
            else if (winobj && objw[x]) mask = (winout >> 8) & 0x3F;
            else mask = winout & 0x3F;
        }
        int top_id = 5, bot_id = 5;
        uint16_t top = backdrop, bot = backdrop;
        int found = 0;
        for (int p = 0; p < 4 && found < 2; ++p) {
            if (obj_on && (mask & 0x10) && (objl[x] & kOpaque) && objp[x] == p) {
                if (found == 0) top = objl[x], top_id = 4;
                else bot = objl[x], bot_id = 4;
                ++found;
            }
            for (int b = 0; b < 4 && found < 2; ++b) {
                if (!((bg_mask >> b) & 1) || !((mask >> b) & 1) || bg_prio[b] != p) continue;
                const uint16_t c = bgl[b][x];
                if (!(c & kOpaque)) continue;
                if (found == 0) top = c, top_id = b;
                else bot = c, bot_id = b;
                ++found;
            }
        }
        top &= 0x7FFF;
        bot &= 0x7FFF;
        uint16_t color = top;
        const bool fx = mask & 0x20;
        const bool top_first = (bldcnt >> top_id) & 1;
        const bool bot_second = (bldcnt >> (8 + bot_id)) & 1;
        auto alpha = [&](uint16_t a, uint16_t b) {
            uint16_t r = 0;
            for (int s = 0; s < 15; s += 5) {
                const int ca = (a >> s) & 31, cb = (b >> s) & 31;
                r |= static_cast<uint16_t>(std::min(31, (ca * eva + cb * evb) >> 4) << s);
            }
            return r;
        };
        if (top_id == 4 && objs[x] && bot_second) {
            color = alpha(top, bot);
        } else if (fx) {
            if (blend_mode == 1 && top_first && bot_second) {
                color = alpha(top, bot);
            } else if ((blend_mode == 2 || blend_mode == 3) && top_first) {
                uint16_t r = 0;
                for (int s = 0; s < 15; s += 5) {
                    int c = (top >> s) & 31;
                    c = blend_mode == 2 ? c + (((31 - c) * evy) >> 4) : c - ((c * evy) >> 4);
                    r |= static_cast<uint16_t>(c << s);
                }
                color = r;
            }
        }
        out[x] = to_argb(color);
    }
}

} // namespace

int main() {
    // ROM mínima só para construir o Gba (a composição não lê a ROM).
    std::vector<uint8_t> blank(0x200, 0);
    const char* tmp = "test_ppu_blank.gba";
    std::FILE* f = std::fopen(tmp, "wb");
    std::fwrite(blank.data(), 1, blank.size(), f);
    std::fclose(f);
    auto rom = core::Rom::load(tmp);
    std::remove(tmp);
    auto g = std::make_unique<gba::Gba>(*rom);
    gba::Ppu& ppu = g->ppu;

    std::mt19937 rng(12345);
    auto r16 = [&] { return static_cast<uint16_t>(rng()); };
    const int kCases = 20000;
    int failures = 0;
    std::vector<uint32_t> expected(W);

    for (int t = 0; t < kCases; ++t) {
        gba::Io& io = g->io;
        // Registradores relevantes, com viés para casos interessantes.
        io.reg(0x00) = static_cast<uint16_t>((r16() & 0xFF07) & ~0x80); // modo, camadas, janelas (sem forced blank)
        for (int b = 0; b < 4; ++b) io.reg(0x08 + b * 2) = r16();
        for (uint32_t off = 0x40; off <= 0x4A; off += 2) io.reg(off) = r16();
        if (rng() % 3 == 0) { // janelas "normais" (x1 <= x2)
            io.reg(0x40) = static_cast<uint16_t>(((rng() % 120) << 8) | (120 + rng() % 120));
            io.reg(0x44) = static_cast<uint16_t>(((rng() % 80) << 8) | (80 + rng() % 80));
        }
        io.reg(0x50) = r16();
        io.reg(0x52) = static_cast<uint16_t>((rng() % 20) | ((rng() % 20) << 8));
        io.reg(0x54) = static_cast<uint16_t>(rng() % 20);
        const uint16_t bd = r16();
        std::memcpy(g->bus.palette.data(), &bd, 2);

        // Conteúdo das camadas: densidade variável de pixels opacos.
        const int density = static_cast<int>(rng() % 4);
        auto pix = [&] {
            const bool opaque = density == 3 || (rng() % 4) < static_cast<uint32_t>(density + 1);
            return static_cast<uint16_t>(opaque ? (r16() | kOpaque) : 0);
        };
        auto& bgl = PpuTestAccess::bg(ppu);
        for (auto& l : bgl)
            for (auto& p : l) p = pix();
        uint8_t used = 0;
        for (int x = 0; x < W; ++x) {
            PpuTestAccess::obj(ppu)[x] = rng() % 3 ? 0 : pix();
            const uint8_t pr = static_cast<uint8_t>(rng() % 4);
            PpuTestAccess::prio(ppu)[x] = (PpuTestAccess::obj(ppu)[x] & kOpaque) ? pr : 4;
            if (PpuTestAccess::obj(ppu)[x] & kOpaque) used |= static_cast<uint8_t>(1 << pr);
            PpuTestAccess::semi(ppu)[x] = rng() % 4 == 0;
            PpuTestAccess::win(ppu)[x] = rng() % 2 == 0;
        }
        PpuTestAccess::prio_used(ppu) = used;

        const int line = static_cast<int>(rng() % gba::kScreenH);
        reference_compose(*g, line, expected.data());
        PpuTestAccess::compose(ppu, line);
        const uint32_t* got = &g->frame()[line * W];
        for (int x = 0; x < W; ++x) {
            if (got[x] != expected[x]) {
                if (++failures <= 10)
                    std::printf("FALHOU caso %d linha %d x=%d: esperado %08X, obtido %08X (DISPCNT %04X BLDCNT %04X)\n",
                                t, line, x, expected[x], got[x], io.reg(0), io.reg(0x50));
                break;
            }
        }
    }
    std::printf("%d/%d casos de composição idênticos à referência\n", kCases - failures, kCases);
    return failures ? 1 : 0;
}
