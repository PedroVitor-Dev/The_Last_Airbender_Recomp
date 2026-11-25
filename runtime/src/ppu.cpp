// Renderizador por scanline: BGs de texto (modo 0/1), afins (1/2), bitmap (3/4/5),
// sprites normais e afins, janelas (WIN0/WIN1/OBJ) e efeitos de cor (alpha/brilho).

#include <algorithm>
#include <cstring>

#include "gba/gba.h"

namespace gba {

namespace {

constexpr uint16_t kOpaque = 0x8000;

inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

inline uint32_t to_argb(uint16_t c) {
    const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

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

void Ppu::render_text_bg(int bg, int line) {
    Io& io = gba_.io;
    const uint8_t* vram = gba_.bus.vram.data();
    const uint8_t* pal = gba_.bus.palette.data();
    const uint16_t cnt = io.reg(0x08 + bg * 2);
    const uint32_t char_base = ((cnt >> 2) & 3) * 0x4000;
    const uint32_t screen_base = ((cnt >> 8) & 31) * 0x800;
    const bool c256 = cnt & 0x80;
    const int size = cnt >> 14;
    const int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    const int hofs = io.reg(0x10 + bg * 4) & 0x1FF, vofs = io.reg(0x12 + bg * 4) & 0x1FF;
    const int ty = (line + vofs) & (h - 1);
    auto& out = bg_line_[bg];

    for (int x = 0; x < kScreenW; ++x) {
        const int tx = (x + hofs) & (w - 1);
        const int block = (tx >> 8) + (ty >> 8) * (w == 512 ? 2 : 1);
        const uint32_t map_addr = screen_base + block * 0x800 + (((ty & 255) >> 3) * 32 + ((tx & 255) >> 3)) * 2;
        const uint16_t e = rd16(vram + (map_addr & 0xFFFF));
        const uint32_t tile = e & 0x3FF;
        int px = tx & 7, py = ty & 7;
        if (e & 0x400) px = 7 - px;
        if (e & 0x800) py = 7 - py;
        uint32_t idx;
        uint32_t color_off;
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
        out[x] = idx ? static_cast<uint16_t>((rd16(pal + color_off) & 0x7FFF) | kOpaque) : 0;
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

    std::array<int, 4> bg_prio{};
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

    uint32_t* out = &frame_[line * kScreenW];
    for (int x = 0; x < kScreenW; ++x) {
        uint8_t mask = 0x3F;
        if (any_win) {
            if (win0 && in_win(0, x)) mask = winin & 0x3F;
            else if (win1 && in_win(1, x)) mask = (winin >> 8) & 0x3F;
            else if (winobj && obj_window_[x]) mask = (winout >> 8) & 0x3F;
            else mask = winout & 0x3F;
        }

        // Duas camadas do topo: id 0-3 = BG, 4 = OBJ, 5 = backdrop
        int top_id = 5, bot_id = 5;
        uint16_t top = backdrop, bot = backdrop;
        int found = 0;
        for (int p = 0; p < 4 && found < 2; ++p) {
            if (obj_on && (mask & 0x10) && (obj_line_[x] & kOpaque) && obj_prio_[x] == p) {
                if (found == 0) top = obj_line_[x], top_id = 4;
                else bot = obj_line_[x], bot_id = 4;
                ++found;
            }
            for (int b = 0; b < 4 && found < 2; ++b) {
                if (!((bg_mask >> b) & 1) || !((mask >> b) & 1) || bg_prio[b] != p) continue;
                const uint16_t c = bg_line_[b][x];
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
        if (top_id == 4 && obj_semi_[x] && bot_second) {
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

void Ppu::render_line(int line) {
    Io& io = gba_.io;
    const uint16_t dispcnt = io.reg(0);
    if (dispcnt & 0x80) { // forced blank
        std::fill_n(&frame_[line * kScreenW], kScreenW, 0xFFFFFFFFu);
        return;
    }
    const int mode = dispcnt & 7;
    for (auto& l : bg_line_) l.fill(0);
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
    render_sprites(line);
    compose(line);

    // Avança as referências afins internas (PB/PD) para a próxima linha.
    for (int i = 0; i < 2; ++i) {
        const uint32_t base = 0x20 + i * 0x10;
        affine_x_[i] += static_cast<int16_t>(io.reg(base + 2));
        affine_y_[i] += static_cast<int16_t>(io.reg(base + 6));
    }
}

} // namespace gba
