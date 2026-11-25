// Janela Win32 do tlagba: imagem com proporção correta (barras pretas), tela cheia sem borda,
// pausa, turbo, capturas, save states com slots, teclado e gamepad configuráveis (tlagba.ini) e
// a tela de configurações (menu.h), que congela o jogo enquanto está aberta.

#ifdef _WIN32

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

#include "audio_out.h"
#include "config.h"
#include "frontend.h"
#include "input_xinput.h"
#include "menu.h"
#include "png.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

namespace fe {

namespace {

using clock = std::chrono::steady_clock;

const wchar_t* kTitle = L"Avatar: The Last Airbender Recomp v" TLA_VERSION;

// Máscaras XInput usadas na navegação do menu (layout Xbox: A confirma, B volta).
constexpr uint32_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr uint32_t kPadStart = 0x0010, kPadBack = 0x0020, kPadA = 0x1000, kPadB = 0x2000;
constexpr uint32_t kPadSticks = PAD_LS_UP | PAD_LS_DOWN | PAD_LS_LEFT | PAD_LS_RIGHT;

struct WinState {
    Config* cfg = nullptr;
    SettingsMenu* menu = nullptr;
    HWND hwnd = nullptr;
    bool quit = false;
    std::array<bool, 256> down{};
    bool paused = false;
    bool fullscreen = false;
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    // Ações pedidas pelo teclado (tratadas no laço de quadros).
    bool req_save = false, req_load = false, req_shot = false, req_fullscreen = false, req_menu = false;
    int slot_delta = 0;
};

WinState* g_ws = nullptr;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void set_fullscreen(WinState& ws, bool on) {
    if (on == ws.fullscreen) return;
    HWND h = ws.hwnd;
    if (on) {
        GetWindowPlacement(h, &ws.placement);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(h, &ws.placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    ws.fullscreen = on;
}

// Janela com área interna de 240×160 × escala (sem efeito em tela cheia).
void apply_scale(WinState& ws, int scale) {
    if (ws.fullscreen) return;
    if (IsZoomed(ws.hwnd)) ShowWindow(ws.hwnd, SW_RESTORE);
    RECT rc{0, 0, gba::kScreenW * scale, gba::kScreenH * scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(ws.hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

bool in(const std::vector<int>& v, int vk) {
    for (int k : v)
        if (k == vk) return true;
    return false;
}

// Teclado com o menu aberto: setas/Enter/Esc, e também as teclas mapeadas para o direcional e A/B.
void menu_key(WinState& ws, int vk, bool repeat) {
    SettingsMenu& m = *ws.menu;
    const Config& c = *ws.cfg;
    if (m.capturing_key()) {
        if (!repeat) m.captured_key(vk);
        return;
    }
    if (m.capturing_pad()) {
        if (vk == VK_ESCAPE && !repeat) m.cancel_capture();
        return;
    }
    using N = SettingsMenu::Nav;
    if (vk == VK_UP || in(c.keyboard[BTN_UP], vk)) m.nav(N::Up);
    else if (vk == VK_DOWN || in(c.keyboard[BTN_DOWN], vk)) m.nav(N::Down);
    else if (vk == VK_LEFT || in(c.keyboard[BTN_LEFT], vk)) m.nav(N::Left);
    else if (vk == VK_RIGHT || in(c.keyboard[BTN_RIGHT], vk)) m.nav(N::Right);
    else if (repeat) return;
    else if (vk == c.key_menu) m.nav(N::Back), m.nav(N::Back); // fecha de qualquer página
    else if (vk == VK_RETURN || vk == VK_SPACE || in(c.keyboard[BTN_A], vk)) m.nav(N::Confirm);
    else if (vk == VK_ESCAPE || vk == VK_BACK || in(c.keyboard[BTN_B], vk)) m.nav(N::Back);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinState* ws = g_ws;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (!ws) break;
        const int vk = static_cast<int>(wp) & 0xFF;
        const bool repeat = (lp >> 30) & 1;
        const bool alt = (lp >> 29) & 1;
        if (vk == VK_RETURN && alt) { // Alt+Enter
            if (!repeat) ws->req_fullscreen = true;
            return 0;
        }
        if (ws->menu && ws->menu->is_open()) {
            menu_key(*ws, vk, repeat);
            return 0;
        }
        const Config& c = *ws->cfg;
        if (!repeat && (vk == VK_ESCAPE || vk == c.key_menu)) {
            ws->req_menu = true;
            return 0;
        }
        ws->down[vk] = true;
        if (!repeat) {
            if (vk == c.key_save) ws->req_save = true;
            else if (vk == c.key_load) ws->req_load = true;
            else if (vk == c.key_slot_prev) ws->slot_delta -= 1;
            else if (vk == c.key_slot_next) ws->slot_delta += 1;
            else if (vk == c.key_pause) ws->paused = !ws->paused;
            else if (vk == c.key_fullscreen) ws->req_fullscreen = true;
            else if (vk == c.key_screenshot) ws->req_shot = true;
        }
        if (msg == WM_SYSKEYDOWN && vk != VK_F4) return 0; // F10/Alt não abrem o menu do sistema
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (ws) ws->down[static_cast<int>(wp) & 0xFF] = false;
        if (msg == WM_SYSKEYUP) return 0;
        break;
    case WM_KILLFOCUS:
        if (ws) ws->down.fill(false);
        return 0;
    case WM_ERASEBKGND:
        return 1; // o desenho cobre tudo (imagem + barras): evita piscar
    case WM_CLOSE:
        if (ws) ws->quit = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Desenha a imagem (src_w×src_h, 3:2) com a maior escala que cabe, barras pretas no resto, e uma
// mensagem de status (se houver) no canto.
void draw(HDC dc, HWND hwnd, const uint32_t* pixels, const BITMAPINFO& bmi, const std::wstring& status) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    const int src_w = bmi.bmiHeader.biWidth, src_h = -bmi.bmiHeader.biHeight;
    int w = cw, h = cw * src_h / src_w;
    if (h > ch) h = ch, w = ch * src_w / src_h;
    const int x = (cw - w) / 2, y = (ch - h) / 2;
    if (x > 0) PatBlt(dc, 0, 0, x, ch, BLACKNESS), PatBlt(dc, x + w, 0, cw - x - w, ch, BLACKNESS);
    if (y > 0) PatBlt(dc, 0, 0, cw, y, BLACKNESS), PatBlt(dc, 0, y + h, cw, ch - y - h, BLACKNESS);
    SetStretchBltMode(dc, COLORONCOLOR); // vizinho mais próximo: pixels nítidos
    StretchDIBits(dc, x, y, w, h, 0, 0, src_w, src_h, pixels, &bmi, DIB_RGB_COLORS, SRCCOPY);
    if (!status.empty()) {
        SetBkMode(dc, TRANSPARENT);
        const int tx = x + 8, ty = y + 6;
        SetTextColor(dc, RGB(0, 0, 0));
        TextOutW(dc, tx + 1, ty + 1, status.c_str(), static_cast<int>(status.size()));
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, tx, ty, status.c_str(), static_cast<int>(status.size()));
    }
}

uint16_t keyboard_keys(const WinState& ws) {
    uint16_t k = 0;
    for (int b = 0; b < BTN_COUNT; ++b)
        for (int vk : ws.cfg->keyboard[b])
            if (vk > 0 && vk < 256 && ws.down[vk]) k |= static_cast<uint16_t>(1u << b);
    return k;
}

std::string timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// Abre/fecha o menu pelo controle: botão Xbox/home, ou Back+Start (quando o driver não expõe o home).
bool pad_menu_combo(uint32_t raw, uint32_t pressed) {
    if (pressed & PAD_GUIDE) return true;
    return (raw & (kPadBack | kPadStart)) == (kPadBack | kPadStart) && (pressed & (kPadBack | kPadStart));
}

// Direções do controle com repetição automática enquanto seguradas (400 ms, depois a cada 110 ms).
struct PadRepeat {
    uint32_t held = 0;
    clock::time_point next{};
    uint32_t step(uint32_t dirs) {
        const auto now = clock::now();
        const uint32_t fresh = dirs & ~held;
        if (fresh) {
            held = dirs;
            next = now + std::chrono::milliseconds(400);
            return fresh;
        }
        held = dirs;
        if (dirs && now >= next) {
            next = now + std::chrono::milliseconds(110);
            return dirs;
        }
        return 0;
    }
};

} // namespace

int run_window(gba::Gba& g, const Options& o, const fs::path& save_path) {
    const fs::path ini = exe_dir() / "tlagba.ini";
    std::vector<std::string> warnings;
    Config cfg = load_config(ini, warnings);
    for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());

    int slot = 1;
    SettingsMenu menu(cfg, slot);
    WinState ws;
    ws.cfg = &cfg;
    ws.menu = &menu;
    g_ws = &ws;

    const fs::path rom_dir = fs::absolute(o.rom).parent_path();
    const std::string stem = o.rom.stem().string();
    auto slot_path = [&](int s) { return rom_dir / "states" / (stem + ".slot" + std::to_string(s) + ".state"); };

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); // ícone do app.rc
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"TlaRecomp";
    RegisterClassW(&wc);
    const int scale = o.scale > 0 ? o.scale : cfg.scale;
    RECT rc{0, 0, gba::kScreenW * scale, gba::kScreenH * scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    ws.hwnd = CreateWindowW(wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (cfg.fullscreen) set_fullscreen(ws, true);
    HDC dc = GetDC(ws.hwnd);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = gba::kScreenW;
    bmi.bmiHeader.biHeight = -gba::kScreenH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    // O áudio continua ativo mesmo com volume 0: ele é o relógio do laço de quadros.
    auto audio = o.mute ? nullptr : AudioOut::create(gba::Apu::kSampleRate);
    timeBeginPeriod(1);
    const auto frame_time = std::chrono::nanoseconds(16742706); // 59,7275 Hz
    const auto started = clock::now();
    auto next = started;
    const uint64_t max_frames = o.frames_given ? o.frames : 0;

    std::wstring status;
    clock::time_point status_until{};
    auto say = [&](const std::string& s) {
        status = widen(s);
        status_until = clock::now() + std::chrono::seconds(3);
    };
    say(std::string(key_name(cfg.key_menu)) + ", Esc or Xbox/home button: settings");

    auto pump = [&] {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    };
    auto current_status = [&]() -> std::wstring {
        if (ws.paused) return L"Paused (" + widen(key_name(cfg.key_pause)) + L" resumes)";
        return clock::now() < status_until ? status : std::wstring();
    };

    auto persist_config = [&] {
        if (!menu.config_dirty()) return;
        menu.clear_dirty();
        if (!save_config(ini, cfg)) std::fprintf(stderr, "warning: could not write %s\n", ini.string().c_str());
    };

    auto load_slot = [&](int s) {
        std::vector<uint8_t> bytes;
        if (!read_file(slot_path(s), bytes)) {
            say("Slot " + std::to_string(s) + " is empty");
            return false;
        }
        try {
            say("State loaded from slot " + std::to_string(s));
            g.load_state_and_restart(bytes); // não retorna: recomeça a partir do estado
        } catch (const std::runtime_error& e) {
            say(std::string("Could not load: ") + e.what());
        }
        return false;
    };
    auto save_slot = [&](int s) {
        const fs::path p = slot_path(s);
        fs::create_directories(p.parent_path());
        return write_file(p, g.save_state());
    };

    // Ações que valem com o jogo rodando ou pausado. Carregar estado desempilha (não retorna).
    auto handle_requests = [&] {
        if (ws.req_fullscreen) ws.req_fullscreen = false, set_fullscreen(ws, !ws.fullscreen);
        if (ws.slot_delta) {
            slot = ((slot + ws.slot_delta) % 10 + 10) % 10;
            ws.slot_delta = 0;
            say("Slot " + std::to_string(slot) + (fs::exists(slot_path(slot)) ? "" : " (empty)"));
        }
        if (ws.req_shot) {
            ws.req_shot = false;
            const fs::path p = rom_dir / "screenshots" / (stem + "_" + timestamp() + ".png");
            fs::create_directories(p.parent_path());
            say(png::write_rgb(p.string(), g.frame().data(), gba::kScreenW, gba::kScreenH)
                    ? "Screenshot saved: " + p.filename().string()
                    : "Could not save the screenshot");
        }
        if (ws.req_save) {
            ws.req_save = false;
            say(save_slot(slot) ? "State saved to slot " + std::to_string(slot) : "Could not save the state");
        }
        if (ws.req_load) {
            ws.req_load = false;
            load_slot(slot);
        }
    };

    // Controle: estado anterior (para bordas) e botões a ignorar até serem soltos (os que
    // estavam segurados ao fechar o menu, para não vazarem para o jogo).
    uint32_t pad_prev = 0, pad_suppress = 0;

    // Tela de configurações. A emulação fica parada aqui dentro (o jogo congela); a janela e o
    // controle continuam sendo atendidos.
    auto run_menu = [&] {
        ws.req_menu = false;
        const std::array<uint32_t, gba::kScreenW * gba::kScreenH> frozen = g.frame();
        menu.show();
        PadRepeat rep;
        uint32_t prev = poll_pad_raw(cfg.deadzone); // o que já está apertado não conta como novo
        auto close = [&] {
            menu.hide();
            persist_config();
            ws.down.fill(false);
            pad_prev = poll_pad_raw(cfg.deadzone);
            pad_suppress = pad_prev;
            next = clock::now();
        };
        while (!ws.quit) {
            pump();
            handle_requests(); // Alt+Enter continua valendo
            const uint32_t raw = poll_pad_raw(cfg.deadzone);
            const uint32_t pressed = raw & ~prev;
            prev = raw;
            if (menu.capturing_pad()) {
                const uint32_t cand = pressed & ~kPadSticks;
                if (cand) menu.captured_pad(cand & (~cand + 1)); // o bit mais baixo pressionado
            } else if (pad_menu_combo(raw, pressed)) {
                close();
                return;
            } else {
                using N = SettingsMenu::Nav;
                const uint32_t dirs = rep.step(raw & (kPadUp | kPadDown | kPadLeft | kPadRight | kPadSticks));
                if (dirs & (kPadUp | PAD_LS_UP)) menu.nav(N::Up);
                if (dirs & (kPadDown | PAD_LS_DOWN)) menu.nav(N::Down);
                if (dirs & (kPadLeft | PAD_LS_LEFT)) menu.nav(N::Left);
                if (dirs & (kPadRight | PAD_LS_RIGHT)) menu.nav(N::Right);
                if (pressed & kPadA) menu.nav(N::Confirm);
                if (pressed & kPadB) menu.nav(N::Back);
            }

            using A = SettingsMenu::Action;
            switch (menu.take_action()) {
            case A::Resume: close(); return;
            case A::Quit: close(); ws.quit = true; return;
            case A::SaveState:
                menu.set_status(save_slot(slot) ? "State saved to slot " + std::to_string(slot)
                                                : "Could not save the state");
                break;
            case A::LoadState:
                if (!fs::exists(slot_path(slot))) {
                    menu.set_status("Slot " + std::to_string(slot) + " is empty");
                    break;
                }
                close();
                load_slot(slot); // em caso de sucesso, não retorna
                return;
            case A::ToggleFullscreen:
                set_fullscreen(ws, !ws.fullscreen);
                cfg.fullscreen = ws.fullscreen;
                save_config(ini, cfg);
                break;
            case A::ApplyScale: apply_scale(ws, cfg.scale); break;
            case A::ApplyVolume: case A::None: break;
            }

            menu.set_fullscreen_flag(ws.fullscreen);
            menu.set_slot_exists(fs::exists(slot_path(slot)));
            draw(dc, ws.hwnd, menu.render(frozen.data()), menu.bitmap_info(), {});
            Sleep(15);
        }
        close();
    };

    std::vector<int16_t> scaled;
    g.run([&] {
        pump();
        if (ws.quit) return false;
        uint32_t raw = poll_pad_raw(cfg.deadzone);
        const uint32_t pressed = raw & ~pad_prev;
        pad_prev = raw;
        pad_suppress &= raw;
        if (pad_menu_combo(raw, pressed)) ws.req_menu = true;
        if (ws.req_menu) {
            run_menu();
            if (ws.quit) return false;
            raw = pad_prev;
        }
        handle_requests();
        draw(dc, ws.hwnd, g.frame().data(), bmi, current_status());

        // Pausa: a emulação fica parada aqui dentro; janela, atalhos e save states seguem.
        while (ws.paused && !ws.quit && !ws.req_menu) {
            Sleep(15);
            pump();
            handle_requests();
            draw(dc, ws.hwnd, g.frame().data(), bmi, current_status());
            next = clock::now();
        }
        if (ws.quit) return false;

        if (g.frame_count % 60 == 0) store_save(g, save_path);
        auto samples = g.apu.take_samples();
        const bool turbo = cfg.key_turbo && ws.down[cfg.key_turbo];
        if (turbo) {
            next = clock::now(); // sem limite de velocidade; o áudio acelerado é descartado
        } else if (audio) {
            if (cfg.volume < 100) {
                scaled.resize(samples.size());
                const int v = cfg.volume;
                for (size_t i = 0; i < samples.size(); ++i) scaled[i] = static_cast<int16_t>(samples[i] * v / 100);
                audio->submit(scaled);
            } else {
                audio->submit(samples);
            }
            while (audio->queued() > 3) Sleep(1); // áudio como relógio: ~3 quadros na fila
        } else {
            next += frame_time;
            const auto now = clock::now();
            if (next > now)
                Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count()));
            else if (now - next > frame_time * 5)
                next = now;
        }
        g.set_keys(static_cast<uint16_t>(keyboard_keys(ws) | pad_to_gba(cfg, raw & ~pad_suppress)));
        return !max_frames || g.frame_count < max_frames;
    });

    timeEndPeriod(1);
    const double secs = std::chrono::duration<double>(clock::now() - started).count();
    std::printf("%llu frames in %.2f s (%.2f fps, target 59.73)%s, %s mode\n",
                static_cast<unsigned long long>(g.frame_count), secs, g.frame_count / secs,
                audio ? ", paced by audio" : "", g.has_recomp() ? "recompiled" : "interpreter");
    persist_config();
    g_ws = nullptr;
    ReleaseDC(ws.hwnd, dc);
    DestroyWindow(ws.hwnd);
    store_save(g, save_path);
    return 0;
}

} // namespace fe

#endif
