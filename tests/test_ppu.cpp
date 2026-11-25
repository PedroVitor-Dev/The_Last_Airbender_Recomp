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
    static void sprites(Ppu& p, int line) { p.render_sprites(line); }
    static void text_bg(Ppu& p, int bg, int line) { p.render_text_bg(bg, line); }
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

struct SpriteLine {
    std::array<uint16_t, W> color{};
    std::array<uint8_t, W> prio{};
    std::array<bool, W> semi{}, window{};
};

// Sprites de referência: cópia fiel do laço geral por pixel (antes do caminho rápido).
void reference_sprites(gba::Gba& g, int line, SpriteLine& s) {
    static constexpr uint8_t kObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
    static constexpr uint8_t kObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};
    auto rd = [](const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; };
    const uint8_t* vram = g.bus.vram.data();
    const uint8_t* pal = g.bus.palette.data();
    const uint8_t* oam = g.bus.oam.data();
    const uint16_t dispcnt = g.io.reg(0);
    const bool map1d = dispcnt & 0x40;
    const bool bitmap_mode = (dispcnt & 7) >= 3;
    s.color.fill(0);
    s.prio.fill(4);
    s.semi.fill(false);
    s.window.fill(false);
    for (int n = 0; n < 128; ++n) {
        const uint16_t a0 = rd(oam + n * 8), a1 = rd(oam + n * 8 + 2), a2 = rd(oam + n * 8 + 4);
        const bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        const int shape = a0 >> 14;
        if (shape == 3) continue;
        const int sz = a1 >> 14;
        const int w = kObjW[shape][sz], h = kObjH[shape][sz];
        const bool dbl = affine && (a0 & 0x200);
        const int bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int sy = a0 & 0xFF;
        if (sy >= 160) sy -= 256;
        if (line < sy || line >= sy + bh) continue;
        int sx = a1 & 0x1FF;
        if (sx >= 240) sx -= 512;
        const int mode = (a0 >> 10) & 3;
        const bool c256 = a0 & 0x2000;
        const uint32_t tile = a2 & 0x3FF;
        if (bitmap_mode && tile < 512) continue;
        const int prio = (a2 >> 10) & 3;
        const uint32_t palbank = a2 >> 12;
        int16_t pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const uint32_t gi = ((a1 >> 9) & 31) * 32;
            pa = static_cast<int16_t>(rd(oam + gi + 6));
            pb = static_cast<int16_t>(rd(oam + gi + 14));
            pc = static_cast<int16_t>(rd(oam + gi + 22));
            pd = static_cast<int16_t>(rd(oam + gi + 30));
        }
        const int iy = line - sy - bh / 2;
        for (int ix = -bw / 2; ix < bw / 2; ++ix) {
            const int x = sx + ix + bw / 2;
            if (x < 0 || x >= W) continue;
            int tx, ty;
            if (affine) {
                tx = ((pa * ix + pb * iy) >> 8) + w / 2;
                ty = ((pc * ix + pd * iy) >> 8) + h / 2;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            } else {
                tx = ix + w / 2;
                ty = iy + h / 2;
                if (a1 & 0x1000) tx = w - 1 - tx;
                if (a1 & 0x2000) ty = h - 1 - ty;
            }
            uint32_t tile_n;
            const uint32_t step = c256 ? 2 : 1;
            if (map1d) tile_n = tile + (ty / 8) * (w / 8) * step + (tx / 8) * step;
            else tile_n = (c256 ? (tile & ~1u) : tile) + (ty / 8) * 32 + (tx / 8) * step;
            uint32_t idx, color_off;
            if (c256) {
                idx = vram[(0x10000 + (tile_n & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7)) % 0x18000];
                color_off = 0x200 + idx * 2;
            } else {
                idx = (vram[(0x10000 + (tile_n & 0x3FF) * 32 + (ty & 7) * 4 + (tx & 7) / 2) % 0x18000] >>
                       ((tx & 1) * 4)) & 15;
                color_off = 0x200 + (palbank * 16 + idx) * 2;
            }
            if (!idx) continue;
            if (mode == 2) {
                s.window[x] = true;
                continue;
            }
            if (prio < s.prio[x]) {
                s.color[x] = static_cast<uint16_t>((rd(pal + color_off) & 0x7FFF) | kOpaque);
                s.prio[x] = static_cast<uint8_t>(prio);
                s.semi[x] = mode == 1;
            }
        }
    }
}

int test_sprites(gba::Gba& g, std::mt19937& rng) {
    gba::Ppu& ppu = g.ppu;
    const int kCases = 5000;
    int failures = 0;
    SpriteLine ref;
    for (int t = 0; t < kCases; ++t) {
        // VRAM de sprites e paleta aleatórias; OAM com viés para sprites visíveis.
        for (size_t i = 0x10000; i < 0x18000; ++i) g.bus.vram[i] = static_cast<uint8_t>(rng() % 3 ? rng() : 0);
        for (auto& b : g.bus.palette) b = static_cast<uint8_t>(rng());
        for (int n = 0; n < 128; ++n) {
            uint16_t a0 = static_cast<uint16_t>(rng()), a1 = static_cast<uint16_t>(rng()), a2 = static_cast<uint16_t>(rng());
            if (rng() % 2) a0 = static_cast<uint16_t>((a0 & 0xFF00) | (rng() % 180));       // y perto da tela
            if (rng() % 2) a1 = static_cast<uint16_t>((a1 & 0xFE00) | ((rng() % 300) - 30) & 0x1FF); // x perto da tela
            if (rng() % 3) a0 = static_cast<uint16_t>(a0 & ~0x0300);                       // maioria não-afim, visível
            std::memcpy(&g.bus.oam[n * 8], &a0, 2);
            std::memcpy(&g.bus.oam[n * 8 + 2], &a1, 2);
            std::memcpy(&g.bus.oam[n * 8 + 4], &a2, 2);
            const uint16_t aff = static_cast<uint16_t>(rng());
            std::memcpy(&g.bus.oam[n * 8 + 6], &aff, 2);
        }
        g.io.reg(0) = static_cast<uint16_t>(rng() & 0xFF47);
        const int line = static_cast<int>(rng() % gba::kScreenH);
        reference_sprites(g, line, ref);
        PpuTestAccess::sprites(ppu, line);
        uint8_t used = 0;
        for (int x = 0; x < W; ++x)
            if (ref.color[x] & kOpaque) used |= static_cast<uint8_t>(1 << ref.prio[x]);
        // obj_prio_used_ pode ser um superconjunto (marca também pixels sobrescritos depois):
        // custa no máximo um passe extra na composição. Faltar um bit seria erro.
        bool ok = (PpuTestAccess::prio_used(ppu) & used) == used;
        if (!ok && failures < 10)
            std::printf("FALHOU sprites caso %d: prioridades usadas %X, esperado incluir %X\n", t,
                        PpuTestAccess::prio_used(ppu), used);
        for (int x = 0; x < W && ok; ++x) {
            ok = PpuTestAccess::obj(ppu)[x] == ref.color[x] && PpuTestAccess::prio(ppu)[x] == ref.prio[x] &&
                 PpuTestAccess::semi(ppu)[x] == ref.semi[x] && PpuTestAccess::win(ppu)[x] == ref.window[x];
            if (!ok && failures < 10)
                std::printf("FALHOU sprites caso %d linha %d x=%d: cor %04X x %04X, prio %u x %u\n", t, line, x,
                            ref.color[x], PpuTestAccess::obj(ppu)[x], ref.prio[x], PpuTestAccess::prio(ppu)[x]);
        }
        if (!ok) ++failures;
    }
    std::printf("%d/%d casos de sprites idênticos à referência\n", kCases - failures, kCases);
    return failures;
}

// BG de texto de referência: a primeira implementação, pixel a pixel.
void reference_text_bg(gba::Gba& g, int bg, int line, uint16_t* out) {
    auto rd = [](const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; };
    const uint8_t* vram = g.bus.vram.data();
    const uint8_t* pal = g.bus.palette.data();
    const uint16_t cnt = g.io.reg(0x08 + bg * 2);
    const uint32_t char_base = ((cnt >> 2) & 3) * 0x4000;
    const uint32_t screen_base = ((cnt >> 8) & 31) * 0x800;
    const bool c256 = cnt & 0x80;
    const int size = cnt >> 14;
    const int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    const int hofs = g.io.reg(0x10 + bg * 4) & 0x1FF, vofs = g.io.reg(0x12 + bg * 4) & 0x1FF;
    const int ty = (line + vofs) & (h - 1);
    for (int x = 0; x < W; ++x) {
        const int tx = (x + hofs) & (w - 1);
        const int block = (tx >> 8) + (ty >> 8) * (w == 512 ? 2 : 1);
        const uint32_t map_addr = screen_base + block * 0x800 + (((ty & 255) >> 3) * 32 + ((tx & 255) >> 3)) * 2;
        const uint16_t e = rd(vram + (map_addr & 0xFFFF));
        const uint32_t tile = e & 0x3FF;
        int px = tx & 7, py = ty & 7;
        if (e & 0x400) px = 7 - px;
        if (e & 0x800) py = 7 - py;
        uint32_t idx, color_off;
        if (c256) {
            const uint32_t a = char_base + tile * 64 + py * 8 + px;
            if (a >= 0x10000) { out[x] = 0; continue; }
            idx = vram[a];
            color_off = idx * 2;
        } else {
            const uint32_t a = char_base + tile * 32 + py * 4 + px / 2;
            if (a >= 0x10000) { out[x] = 0; continue; }
            idx = (vram[a] >> ((px & 1) * 4)) & 15;
            color_off = ((e >> 12) * 16 + idx) * 2;
        }
        out[x] = idx ? static_cast<uint16_t>((rd(pal + color_off) & 0x7FFF) | kOpaque) : 0;
    }
}

int test_text_bg(gba::Gba& g, std::mt19937& rng) {
    gba::Ppu& ppu = g.ppu;
    const int kCases = 20000;
    int failures = 0;
    std::array<uint16_t, W> ref{};
    for (int t = 0; t < kCases; ++t) {
        // A cada 50 casos, VRAM/paleta novas (com muitos zeros: tiles transparentes).
        if (t % 50 == 0) {
            for (size_t i = 0; i < 0x10000; ++i) g.bus.vram[i] = static_cast<uint8_t>(rng() % 4 ? rng() : 0);
            for (auto& b : g.bus.palette) b = static_cast<uint8_t>(rng());
        }
        // Às vezes muda só uma cor da paleta (a cache de paleta da PPU precisa perceber).
        if (rng() % 4 == 0) g.bus.palette[rng() % 512] = static_cast<uint8_t>(rng());
        const int bg = static_cast<int>(rng() % 4);
        g.io.reg(0x08 + bg * 2) = static_cast<uint16_t>(rng());
        g.io.reg(0x10 + bg * 4) = static_cast<uint16_t>(rng());
        g.io.reg(0x12 + bg * 4) = static_cast<uint16_t>(rng());
        const int line = static_cast<int>(rng() % gba::kScreenH);
        reference_text_bg(g, bg, line, ref.data());
        PpuTestAccess::text_bg(ppu, bg, line);
        const auto& got = PpuTestAccess::bg(ppu)[bg];
        for (int x = 0; x < W; ++x)
            if (got[x] != ref[x]) {
                if (++failures <= 10)
                    std::printf("FALHOU BG texto caso %d bg%d linha %d x=%d: esperado %04X, obtido %04X (CNT %04X)\n", t,
                                bg, line, x, ref[x], got[x], g.io.reg(0x08 + bg * 2));
                break;
            }
    }
    std::printf("%d/%d casos de BG de texto idênticos à referência\n", kCases - failures, kCases);
    return failures;
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
    failures += test_sprites(*g, rng);
    failures += test_text_bg(*g, rng);
    return failures ? 1 : 0;
}
