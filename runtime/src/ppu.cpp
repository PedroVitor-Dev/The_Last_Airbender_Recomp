// Renderizador por scanline: BGs de texto (modo 0/1), afins (1/2), bitmap (3/4/5),
// sprites normais e afins, janelas (WIN0/WIN1/OBJ) e efeitos de cor (alpha/brilho).
//
// Desempenho: BGs de texto são decodificados por tile (uma leitura de mapa a cada 8 px),
// a composição percorre só as camadas ativas, pré-ordenadas por prioridade, e a conversão
// BGR555 → ARGB usa uma tabela. A semântica é a mesma da versão por pixel (as capturas de
// referência em build/golden conferem pixel a pixel).

#include <algorithm>
#include <cstring>

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "gba/gba.h"
#include "gba/profile.h"

namespace gba {

namespace {

constexpr uint16_t kOpaque = 0x8000;

inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

struct ArgbLut {
    std::array<uint32_t, 32768> v{};
    ArgbLut() {
        for (uint32_t c = 0; c < 32768; ++c) {
            const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            v[c] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
        }
    }
};
const ArgbLut kArgb;

inline int32_t ref28(uint16_t lo, uint16_t hi) {
    const uint32_t v = (uint32_t(hi) << 16) | lo;
    return static_cast<int32_t>(v << 4) >> 4;
}

constexpr uint8_t kObjW[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
constexpr uint8_t kObjH[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

} // namespace

Ppu::Ppu(Gba& gba) : gba_(gba) {}

void Ppu::latch_affine() {
    Io& io = gba_.io;
    for (int i = 0; i < 2; ++i) {
        const uint32_t base = 0x28 + i * 0x10;
        affine_x_[i] = ref28(io.reg(base), io.reg(base + 2));
        affine_y_[i] = ref28(io.reg(base + 4), io.reg(base + 6));
    }
}

void Ppu::on_affine_ref_write(int bg) {
    Io& io = gba_.io;
    const int i = bg - 2;
    const uint32_t base = 0x28 + i * 0x10;
    affine_x_[i] = ref28(io.reg(base), io.reg(base + 2));
    affine_y_[i] = ref28(io.reg(base + 4), io.reg(base + 6));
}

void Ppu::refresh_bg_palette() {
    const uint8_t* pal = gba_.bus.palette.data();
    if (bg_pal_valid_ && std::memcmp(bg_pal_cache_.data(), pal, 512) == 0) return;
    std::memcpy(bg_pal_cache_.data(), pal, 512);
    for (int i = 0; i < 256; ++i) {
        const uint16_t c = static_cast<uint16_t>((rd16(pal + i * 2) & 0x7FFF) | kOpaque);
        bg_pal4_[i] = (i & 15) ? c : 0;
        bg_pal8_[i] = i ? c : 0;
    }
    bg_pal_valid_ = true;
}

namespace {
// Inverte a ordem dos 8 nibbles (pixels de 4 bpp) de uma linha de tile: flip horizontal.
inline uint32_t reverse_nibbles(uint32_t b) {
    b = ((b & 0x0F0F0F0Fu) << 4) | ((b >> 4) & 0x0F0F0F0Fu);
    return (b >> 24) | ((b >> 8) & 0xFF00u) | ((b << 8) & 0xFF0000u) | (b << 24);
}
} // namespace

void Ppu::render_text_bg(int bg, int line) {
    Io& io = gba_.io;
    const uint8_t* vram = gba_.bus.vram.data();
    refresh_bg_palette();
    const uint16_t* pal4 = bg_pal4_.data();
    const uint16_t* pal8 = bg_pal8_.data();
    const uint16_t cnt = io.reg(0x08 + bg * 2);
    const uint32_t char_base = ((cnt >> 2) & 3) * 0x4000;
    const uint32_t screen_base = ((cnt >> 8) & 31) * 0x800;
    const bool c256 = cnt & 0x80;
    const int size = cnt >> 14;
    const int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    const int hofs = io.reg(0x10 + bg * 4) & 0x1FF, vofs = io.reg(0x12 + bg * 4) & 0x1FF;
    const int ty = (line + vofs) & (h - 1);
    const uint32_t row_in_map = screen_base + ((ty & 255) >> 3) * 64; // 32 entradas × 2 bytes
    const int block_row = (ty >> 8) * (w == 512 ? 2 : 1);
    uint16_t* out = bg_line_[bg].data();

    int x = 0;
    while (x < kScreenW) {
        const int tx = (x + hofs) & (w - 1);
        const int block = (tx >> 8) + block_row;
        const uint32_t map_addr = row_in_map + block * 0x800 + ((tx & 255) >> 3) * 2;
        const uint16_t e = rd16(vram + (map_addr & 0xFFFF));
        const uint32_t tile = e & 0x3FF;
        const bool hflip = e & 0x400;
        const int py = (e & 0x800) ? 7 - (ty & 7) : (ty & 7);
        int px = tx & 7;
        const int run = std::min(8 - px, kScreenW - x); // pixels deste tile nesta linha

        uint16_t* dst = out + x;
        if (c256) {
            const uint32_t row = char_base + tile * 64 + py * 8;
            uint64_t row_bits = 0;
            if (row < 0x10000) std::memcpy(&row_bits, vram + row, 8);
            if (row_bits == 0) { // fora da área de BG ou toda transparente
                std::fill_n(dst, run, uint16_t(0));
            } else {
                // Bytes na ordem de exibição (flip resolvido uma vez); índice 0 → 0 pela tabela.
                const uint8_t* p = vram + row;
                if (run == 8) {
                    if (hflip) {
                        for (int i = 0; i < 8; ++i) dst[i] = pal8[p[7 - i]];
                    } else {
                        for (int i = 0; i < 8; ++i) dst[i] = pal8[p[i]];
                    }
                } else {
                    for (int i = 0; i < run; ++i, ++px) dst[i] = pal8[p[hflip ? 7 - px : px]];
                }
            }
        } else {
            const uint32_t row = char_base + tile * 32 + py * 4;
            uint32_t bits = 0;
            if (row < 0x10000) std::memcpy(&bits, vram + row, 4); // 8 pixels de 4 bits
            if (bits == 0) { // fora da área de BG ou toda transparente (comum em HUD/céu)
                std::fill_n(dst, run, uint16_t(0));
            } else {
                if (hflip) bits = reverse_nibbles(bits);
                const uint16_t* bank = pal4 + (e >> 12) * 16;
                if (run == 8) {
                    dst[0] = bank[bits & 15];
                    dst[1] = bank[(bits >> 4) & 15];
                    dst[2] = bank[(bits >> 8) & 15];
                    dst[3] = bank[(bits >> 12) & 15];
                    dst[4] = bank[(bits >> 16) & 15];
                    dst[5] = bank[(bits >> 20) & 15];
                    dst[6] = bank[(bits >> 24) & 15];
                    dst[7] = bank[bits >> 28];
                } else {
                    bits >>= px * 4;
                    for (int i = 0; i < run; ++i, bits >>= 4) dst[i] = bank[bits & 15];
                }
            }
        }
        x += run;
    }
}

void Ppu::render_affine_bg(int bg) {
    Io& io = gba_.io;
    const uint8_t* vram = gba_.bus.vram.data();
    const uint8_t* pal = gba_.bus.palette.data();
    const uint16_t cnt = io.reg(0x08 + bg * 2);
    const uint32_t char_base = ((cnt >> 2) & 3) * 0x4000;
    const uint32_t screen_base = ((cnt >> 8) & 31) * 0x800;
    const bool wrap = cnt & 0x2000;
    const int size = 128 << (cnt >> 14);
    const int i = bg - 2;
    const uint32_t base = 0x20 + i * 0x10;
    const int32_t pa = static_cast<int16_t>(io.reg(base)), pc = static_cast<int16_t>(io.reg(base + 4));
    int32_t px = affine_x_[i], py = affine_y_[i];
    auto& out = bg_line_[bg];
    for (int x = 0; x < kScreenW; ++x, px += pa, py += pc) {
        int tx = px >> 8, ty = py >> 8;
        if (wrap) {
            tx &= size - 1;
            ty &= size - 1;
        } else if (tx < 0 || ty < 0 || tx >= size || ty >= size) {
            out[x] = 0;
            continue;
        }
        const uint32_t map = screen_base + (ty >> 3) * (size >> 3) + (tx >> 3);
        const uint32_t tile = vram[map & 0xFFFF];
        const uint32_t idx = vram[(char_base + tile * 64 + (ty & 7) * 8 + (tx & 7)) & 0xFFFF];
        out[x] = idx ? static_cast<uint16_t>((rd16(pal + idx * 2) & 0x7FFF) | kOpaque) : 0;
    }
}

void Ppu::render_bitmap(int mode, int line) {
    Io& io = gba_.io;
    const uint8_t* vram = gba_.bus.vram.data();
    const uint8_t* pal = gba_.bus.palette.data();
    const uint32_t frame = (io.reg(0) & 0x10) ? 0xA000 : 0;
    auto& out = bg_line_[2];
    for (int x = 0; x < kScreenW; ++x) {
        switch (mode) {
        case 3:
            out[x] = static_cast<uint16_t>((rd16(vram + (line * 240 + x) * 2) & 0x7FFF) | kOpaque);
            break;
        case 4: {
            const uint8_t idx = vram[frame + line * 240 + x];
            out[x] = idx ? static_cast<uint16_t>((rd16(pal + idx * 2) & 0x7FFF) | kOpaque) : 0;
            break;
        }
        default: // 5: 160×128
            out[x] = (x < 160 && line < 128)
                         ? static_cast<uint16_t>((rd16(vram + frame + (line * 160 + x) * 2) & 0x7FFF) | kOpaque)
                         : 0;
            break;
        }
    }
}

void Ppu::render_sprites(int line) {
    Io& io = gba_.io;
    const uint8_t* vram = gba_.bus.vram.data();
    const uint8_t* pal = gba_.bus.palette.data();
    const uint8_t* oam = gba_.bus.oam.data();
    const uint16_t dispcnt = io.reg(0);
    const bool map1d = dispcnt & 0x40;
    const bool bitmap_mode = (dispcnt & 7) >= 3;

    obj_line_.fill(0);
    obj_prio_.fill(4);
    obj_semi_.fill(false);
    obj_window_.fill(false);
    obj_prio_used_ = 0;

    for (int n = 0; n < 128; ++n) {
        const uint16_t a0 = rd16(oam + n * 8), a1 = rd16(oam + n * 8 + 2), a2 = rd16(oam + n * 8 + 4);
        const bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue; // desabilitado
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
            const uint32_t g = ((a1 >> 9) & 31) * 32;
            pa = static_cast<int16_t>(rd16(oam + g + 6));
            pb = static_cast<int16_t>(rd16(oam + g + 14));
            pc = static_cast<int16_t>(rd16(oam + g + 22));
            pd = static_cast<int16_t>(rd16(oam + g + 30));
        }
        const int iy = line - sy - bh / 2;

        if (!affine) {
            // Caminho rápido (a grande maioria): linha de tiles fixa, faixa visível recortada
            // uma vez. Mesmo resultado do laço geral abaixo.
            int ty = line - sy;
            if (a1 & 0x2000) ty = h - 1 - ty;
            const uint32_t step = c256 ? 2 : 1;
            const uint32_t row_base =
                map1d ? tile + (ty / 8) * (w / 8) * step : (c256 ? (tile & ~1u) : tile) + (ty / 8) * 32;
            const bool hflip = a1 & 0x1000;
            const int j0 = std::max(0, -sx), j1 = std::min(w, kScreenW - sx);
            for (int j = j0; j < j1; ++j) {
                const int x = sx + j;
                const int tx = hflip ? w - 1 - j : j;
                const uint32_t tile_n = row_base + (tx / 8) * step;
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
                    obj_window_[x] = true;
                    continue;
                }
                if (prio < obj_prio_[x]) {
                    obj_line_[x] = static_cast<uint16_t>((rd16(pal + color_off) & 0x7FFF) | kOpaque);
                    obj_prio_[x] = static_cast<uint8_t>(prio);
                    obj_semi_[x] = mode == 1;
                    obj_prio_used_ |= static_cast<uint8_t>(1 << prio);
                }
            }
            continue;
        }

        for (int ix = -bw / 2; ix < bw / 2; ++ix) {
            const int x = sx + ix + bw / 2;
            if (x < 0 || x >= kScreenW) continue;
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
                const uint32_t addr = 0x10000 + (tile_n & 0x3FF) * 32 + (ty & 7) * 8 + (tx & 7);
                idx = vram[addr % 0x18000];
                color_off = 0x200 + idx * 2;
            } else {
                const uint32_t addr = 0x10000 + (tile_n & 0x3FF) * 32 + (ty & 7) * 4 + (tx & 7) / 2;
                idx = (vram[addr % 0x18000] >> ((tx & 1) * 4)) & 15;
                color_off = 0x200 + (palbank * 16 + idx) * 2;
            }
            if (!idx) continue;
            if (mode == 2) {
                obj_window_[x] = true;
                continue;
            }
            if (prio < obj_prio_[x]) {
                obj_line_[x] = static_cast<uint16_t>((rd16(pal + color_off) & 0x7FFF) | kOpaque);
                obj_prio_[x] = static_cast<uint8_t>(prio);
                obj_semi_[x] = mode == 1;
                obj_prio_used_ |= static_cast<uint8_t>(1 << prio);
            }
        }
    }
}

void Ppu::compose(int line) {
    Io& io = gba_.io;
    const uint16_t dispcnt = io.reg(0);
    const uint8_t* pal = gba_.bus.palette.data();
    const uint16_t backdrop = rd16(pal) & 0x7FFF;
    const int mode = dispcnt & 7;

    // Quais BGs existem neste modo
    uint8_t bg_mask = (dispcnt >> 8) & 0xF;
    if (mode == 1) bg_mask &= 0x7;
    else if (mode == 2) bg_mask &= 0xC;
    else if (mode >= 3) bg_mask &= 0x4;
    const bool obj_on = dispcnt & 0x1000;

    // BGs ativos em ordem de desenho: prioridade, depois índice.
    std::array<int, 4> bg_prio{};
    std::array<int, 4> order{};
    int nbg = 0;
    for (int b = 0; b < 4; ++b) bg_prio[b] = io.reg(0x08 + b * 2) & 3;
    for (int p = 0; p < 4; ++p)
        for (int b = 0; b < 4; ++b)
            if (((bg_mask >> b) & 1) && bg_prio[b] == p) order[nbg++] = b;

    // Janelas: o teste vertical e a faixa horizontal valem para a linha inteira.
    const bool win0 = dispcnt & 0x2000, win1 = dispcnt & 0x4000, winobj = dispcnt & 0x8000;
    const bool any_win = win0 || win1 || winobj;
    const uint16_t winin = io.reg(0x48), winout = io.reg(0x4A);
    struct WinLine {
        bool on;
        int x1, x2;
    } wl[2]{};
    for (int k = 0; k < 2; ++k) {
        const uint16_t hreg = io.reg(0x40 + k * 2), vreg = io.reg(0x44 + k * 2);
        const int y1 = vreg >> 8, y2 = vreg & 0xFF;
        const bool iny = y1 <= y2 ? (line >= y1 && line < y2) : (line >= y1 || line < y2);
        wl[k] = {(k == 0 ? win0 : win1) && iny, hreg >> 8, hreg & 0xFF};
    }
    auto in_x = [](const WinLine& w, int x) {
        return w.x1 <= w.x2 ? (x >= w.x1 && x < w.x2) : (x >= w.x1 || x < w.x2);
    };

    const uint16_t bldcnt = io.reg(0x50), bldalpha = io.reg(0x52);
    const int blend_mode = (bldcnt >> 6) & 3;
    const int eva = std::min(16, bldalpha & 31), evb = std::min(16, (bldalpha >> 8) & 31);
    const int evy = std::min(16, io.reg(0x54) & 31);
    auto alpha = [&](uint16_t a, uint16_t b) {
        uint16_t r = 0;
        for (int s = 0; s < 15; s += 5) {
            const int ca = (a >> s) & 31, cb = (b >> s) & 31;
            r |= static_cast<uint16_t>(std::min(31, (ca * eva + cb * evb) >> 4) << s);
        }
        return r;
    };

    uint32_t* out = &frame_[line * kScreenW];

    if (prof::enabled) {
        bool any_semi = false;
        for (int x = 0; x < kScreenW; ++x) any_semi |= obj_semi_[x];
        auto& c = prof::counters;
        ++c.lines;
        c.lines_win += any_win;
        c.lines_blend += blend_mode != 0;
        c.lines_semi += any_semi;
    }

    // Máscara de janela por pixel (bits 0-3 BGs, 4 OBJ, 5 efeitos).
    alignas(16) uint8_t mask[kScreenW];
    if (any_win) {
        for (int x = 0; x < kScreenW; ++x) {
            if (wl[0].on && in_x(wl[0], x)) mask[x] = winin & 0x3F;
            else if (wl[1].on && in_x(wl[1], x)) mask[x] = (winin >> 8) & 0x3F;
            else if (winobj && obj_window_[x]) mask[x] = (winout >> 8) & 0x3F;
            else mask[x] = winout & 0x3F;
        }
    }

    // Pintor, de trás para frente: cada pixel opaco de uma camada empurra o topo atual
    // para "segunda camada". Ordem: prioridade 3 → 0; dentro dela, BG3 → BG0 e por último
    // os sprites daquela prioridade (sprite fica acima de BG de mesma prioridade).
    // Resultado: top = primeira camada visível, bot = segunda (backdrop = id 5).
    alignas(16) uint16_t top[kScreenW], bot[kScreenW];
    alignas(16) uint16_t tid[kScreenW], bid[kScreenW];
    alignas(16) uint16_t mask16[kScreenW];
    alignas(16) uint16_t prio16[kScreenW];
    std::fill_n(top, kScreenW, backdrop);
    std::fill_n(bot, kScreenW, backdrop);
    std::fill_n(tid, kScreenW, uint16_t(5));
    std::fill_n(bid, kScreenW, uint16_t(5));
    if (any_win)
        for (int x = 0; x < kScreenW; ++x) mask16[x] = mask[x];
    if (obj_on && obj_prio_used_)
        for (int x = 0; x < kScreenW; ++x) prio16[x] = obj_prio_[x];

    // Aplica uma camada: onde o pixel é opaco (e liberado pela janela / tem a prioridade
    // pedida), o topo desce para "segunda" e a camada vira o topo.
    auto paint = [&](const uint16_t* src, uint16_t id, uint16_t mask_bit, bool by_prio, uint16_t prio) {
#if defined(_M_X64) || defined(__SSE2__)
        const __m128i vid = _mm_set1_epi16(static_cast<short>(id));
        const __m128i vbit = _mm_set1_epi16(static_cast<short>(mask_bit));
        const __m128i vprio = _mm_set1_epi16(static_cast<short>(prio));
        const __m128i zero = _mm_setzero_si128();
        for (int x = 0; x < kScreenW; x += 8) {
            const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x));
            __m128i o = _mm_srai_epi16(c, 15); // 0xFFFF onde o bit 15 (opaco) está ligado
            if (any_win) {
                const __m128i m = _mm_load_si128(reinterpret_cast<const __m128i*>(mask16 + x));
                o = _mm_andnot_si128(_mm_cmpeq_epi16(_mm_and_si128(m, vbit), zero), o);
            }
            if (by_prio) {
                const __m128i pr = _mm_load_si128(reinterpret_cast<const __m128i*>(prio16 + x));
                o = _mm_and_si128(o, _mm_cmpeq_epi16(pr, vprio));
            }
            __m128i* pt = reinterpret_cast<__m128i*>(top + x);
            __m128i* pb = reinterpret_cast<__m128i*>(bot + x);
            __m128i* pti = reinterpret_cast<__m128i*>(tid + x);
            __m128i* pbi = reinterpret_cast<__m128i*>(bid + x);
            const __m128i t = _mm_load_si128(pt), ti = _mm_load_si128(pti);
            _mm_store_si128(pb, _mm_or_si128(_mm_and_si128(o, t), _mm_andnot_si128(o, _mm_load_si128(pb))));
            _mm_store_si128(pbi, _mm_or_si128(_mm_and_si128(o, ti), _mm_andnot_si128(o, _mm_load_si128(pbi))));
            _mm_store_si128(pt, _mm_or_si128(_mm_and_si128(o, c), _mm_andnot_si128(o, t)));
            _mm_store_si128(pti, _mm_or_si128(_mm_and_si128(o, vid), _mm_andnot_si128(o, ti)));
        }
#else
        for (int x = 0; x < kScreenW; ++x) {
            const uint16_t c = src[x];
            bool o = (c & kOpaque) != 0;
            if (any_win) o = o && (mask16[x] & mask_bit);
            if (by_prio) o = o && prio16[x] == prio;
            if (o) {
                bot[x] = top[x];
                bid[x] = tid[x];
                top[x] = c;
                tid[x] = id;
            }
        }
#endif
    };
    for (int p = 3; p >= 0; --p) {
        for (int b = 3; b >= 0; --b)
            if (((bg_mask >> b) & 1) && bg_prio[b] == p)
                paint(bg_line_[b].data(), static_cast<uint16_t>(b), static_cast<uint16_t>(1 << b), false, 0);
        if (obj_on && ((obj_prio_used_ >> p) & 1))
            paint(obj_line_.data(), 4, 0x10, true, static_cast<uint16_t>(p));
    }

    // Efeitos de cor. Sem nada a fazer na linha (sem efeito e sem sprite semitransparente),
    // é só converter.
    bool any_semi = false;
    for (int x = 0; x < kScreenW; ++x) any_semi |= obj_semi_[x];
    if (blend_mode == 0 && !any_semi) {
        for (int x = 0; x < kScreenW; ++x) out[x] = kArgb.v[top[x] & 0x7FFF];
        return;
    }

#if defined(_M_X64) || defined(__SSE2__)
    // Efeitos de cor em SSE2, 8 pixels por vez, sem desvios: calcula alpha e brilho para
    // todos e escolhe por máscara, reproduzindo a cadeia de decisão da versão escalar:
    //   A = topo é sprite semitransparente e a segunda camada é 2º alvo  → alpha
    //   senão, se a janela libera efeitos:
    //     modo 1 e topo 1º alvo e segunda 2º alvo                        → alpha
    //     modos 2/3 e topo 1º alvo                                       → brilho
    {
        alignas(16) uint16_t semi16[kScreenW];
        alignas(16) uint16_t color16[kScreenW];
        for (int x = 0; x < kScreenW; ++x) semi16[x] = obj_semi_[x] ? 0xFFFF : 0;
        const __m128i zero = _mm_setzero_si128();
        const __m128i ones = _mm_cmpeq_epi16(zero, zero);
        const __m128i c31 = _mm_set1_epi16(31), c7fff = _mm_set1_epi16(0x7FFF);
        const __m128i v4 = _mm_set1_epi16(4), v20 = _mm_set1_epi16(0x20);
        const __m128i veva = _mm_set1_epi16(static_cast<short>(eva)), vevb = _mm_set1_epi16(static_cast<short>(evb));
        const __m128i vevy = _mm_set1_epi16(static_cast<short>(evy));
        const __m128i is_mode1 = blend_mode == 1 ? ones : zero;
        const __m128i is_mode23 = blend_mode >= 2 ? ones : zero;
        const unsigned first_set = bldcnt & 0x3F, second_set = (bldcnt >> 8) & 0x3F;
        auto in_set = [&](__m128i ids, unsigned set) {
            __m128i r = zero;
            for (int id = 0; id < 6; ++id)
                if ((set >> id) & 1) r = _mm_or_si128(r, _mm_cmpeq_epi16(ids, _mm_set1_epi16(static_cast<short>(id))));
            return r;
        };
        auto select = [](__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); };

        for (int x = 0; x < kScreenW; x += 8) {
            const __m128i t = _mm_and_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(top + x)), c7fff);
            const __m128i b = _mm_and_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(bot + x)), c7fff);
            const __m128i ti = _mm_load_si128(reinterpret_cast<const __m128i*>(tid + x));
            const __m128i bi = _mm_load_si128(reinterpret_cast<const __m128i*>(bid + x));

            // Máscaras de decisão
            const __m128i first = in_set(ti, first_set);
            const __m128i second = in_set(bi, second_set);
            const __m128i semi = _mm_load_si128(reinterpret_cast<const __m128i*>(semi16 + x));
            const __m128i a_semi = _mm_and_si128(_mm_and_si128(_mm_cmpeq_epi16(ti, v4), semi), second);
            const __m128i fxwin =
                any_win ? _mm_cmpeq_epi16(_mm_and_si128(_mm_load_si128(reinterpret_cast<const __m128i*>(mask16 + x)), v20), v20)
                        : ones;
            const __m128i use_alpha =
                _mm_or_si128(a_semi, _mm_and_si128(_mm_and_si128(fxwin, is_mode1), _mm_and_si128(first, second)));
            const __m128i use_bright = _mm_andnot_si128(a_semi, _mm_and_si128(_mm_and_si128(fxwin, is_mode23), first));

            // Alpha e brilho por canal (valores ≤ 31·16·2, cabem em 16 bits)
            __m128i alpha_c = zero, bright_c = zero;
            for (int s = 0; s < 15; s += 5) {
                const __m128i ct = _mm_and_si128(_mm_srli_epi16(t, s), c31);
                const __m128i cb = _mm_and_si128(_mm_srli_epi16(b, s), c31);
                const __m128i al = _mm_min_epi16(
                    c31, _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(ct, veva), _mm_mullo_epi16(cb, vevb)), 4));
                const __m128i br = blend_mode == 2
                                       ? _mm_add_epi16(ct, _mm_srli_epi16(_mm_mullo_epi16(_mm_sub_epi16(c31, ct), vevy), 4))
                                       : _mm_sub_epi16(ct, _mm_srli_epi16(_mm_mullo_epi16(ct, vevy), 4));
                alpha_c = _mm_or_si128(alpha_c, _mm_slli_epi16(al, s));
                bright_c = _mm_or_si128(bright_c, _mm_slli_epi16(br, s));
            }
            const __m128i color = select(use_alpha, alpha_c, select(use_bright, bright_c, t));
            _mm_store_si128(reinterpret_cast<__m128i*>(color16 + x), color);
        }
        for (int x = 0; x < kScreenW; ++x) out[x] = kArgb.v[color16[x]];
        return;
    }
#endif

    // Versão escalar (fallback fora do x86-64; também é a referência de lógica acima).
    uint8_t bright[32];
    for (int c = 0; c < 32; ++c)
        bright[c] = static_cast<uint8_t>(blend_mode == 2 ? c + (((31 - c) * evy) >> 4) : c - ((c * evy) >> 4));
    for (int x = 0; x < kScreenW; ++x) {
        const uint8_t m = any_win ? mask[x] : 0x3F;
        const uint16_t t = top[x] & 0x7FFF;
        const int ti = tid[x];
        const bool top_first = (bldcnt >> ti) & 1;
        const bool bot_second = (bldcnt >> (8 + bid[x])) & 1;
        uint16_t color = t;
        if (ti == 4 && obj_semi_[x] && bot_second) {
            color = alpha(t, bot[x] & 0x7FFF);
        } else if (m & 0x20) {
            if (blend_mode == 1 && top_first && bot_second) {
                color = alpha(t, bot[x] & 0x7FFF);
            } else if ((blend_mode == 2 || blend_mode == 3) && top_first) {
                color = static_cast<uint16_t>(bright[t & 31] | (bright[(t >> 5) & 31] << 5) |
                                              (bright[(t >> 10) & 31] << 10));
            }
        }
        out[x] = kArgb.v[color];
    }
}

void Ppu::render_line(int line) {
    Io& io = gba_.io;
    const uint16_t dispcnt = io.reg(0);
    if (dispcnt & 0x80) { // forced blank
        std::fill_n(&frame_[line * kScreenW], kScreenW, 0xFFFFFFFFu);
        return;
    }
    const int mode = dispcnt & 7;
    // Toda camada que a composição lê é reescrita por inteiro abaixo: não precisa limpar.
    {
    prof::Scope s_bg(prof::counters.bg_ns);
    switch (mode) {
    case 0:
        for (int b = 0; b < 4; ++b) if (dispcnt & (0x100 << b)) render_text_bg(b, line);
        break;
    case 1:
        for (int b = 0; b < 2; ++b) if (dispcnt & (0x100 << b)) render_text_bg(b, line);
        if (dispcnt & 0x400) render_affine_bg(2);
        break;
    case 2:
        if (dispcnt & 0x400) render_affine_bg(2);
        if (dispcnt & 0x800) render_affine_bg(3);
        break;
    default:
        if (dispcnt & 0x400) render_bitmap(mode, line);
        break;
    }
    }
    {
        prof::Scope s_obj(prof::counters.obj_ns);
        render_sprites(line);
    }
    {
        prof::Scope s_mix(prof::counters.mix_ns);
        compose(line);
    }

    // Avança as referências afins internas (PB/PD) para a próxima linha.
    for (int i = 0; i < 2; ++i) {
        const uint32_t base = 0x20 + i * 0x10;
        affine_x_[i] += static_cast<int16_t>(io.reg(base + 2));
        affine_y_[i] += static_cast<int16_t>(io.reg(base + 6));
    }
}

} // namespace gba
