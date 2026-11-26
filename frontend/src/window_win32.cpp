// Janela Win32 do frontend: imagem com proporção correta (barras pretas), tela cheia sem borda,
// pausa, turbo, capturas, save states com slots, teclado e gamepad configuráveis (tlagba.ini), a
// tela de configurações (que congela o jogo) e, no AvatarRecomp.exe, o launcher (menu.h).
//
// Uma Session guarda a janela, a configuração e as telas; o launcher e o jogo se revezam nela.

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>

#include "audio_out.h"
#include "config.h"
#include "core/rom.h"
#include "frontend.h"
#include "input_xinput.h"
#include "menu.h"
#include "png.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <timeapi.h>

#ifndef TLA_VERSION
#define TLA_VERSION "dev"
#endif

namespace fe {

namespace {

using clock = std::chrono::steady_clock;

const wchar_t* kTitle = L"Avatar: The Last Airbender Recomp v" TLA_VERSION;

// Máscaras XInput usadas na navegação das telas (layout Xbox: A confirma, B volta).
constexpr uint32_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008;
constexpr uint32_t kPadStart = 0x0010, kPadBack = 0x0020, kPadA = 0x1000, kPadB = 0x2000;
constexpr uint32_t kPadSticks = PAD_LS_UP | PAD_LS_DOWN | PAD_LS_LEFT | PAD_LS_RIGHT;

struct WinState {
    Config* cfg = nullptr;
    SettingsMenu* menu = nullptr;
    Launcher* launcher = nullptr; // não nulo enquanto o launcher está na tela
    HWND hwnd = nullptr;
    bool quit = false;  // janela fechada: encerra o programa
    bool leave = false; // "Exit to launcher": encerra só a partida
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

// Tecla → navegação nas telas: setas/Enter/Esc e também as teclas mapeadas para o direcional e A/B.
bool key_nav(const Config& c, int vk, bool repeat, Nav& out) {
    if (vk == VK_UP || in(c.keyboard[BTN_UP], vk)) out = Nav::Up;
    else if (vk == VK_DOWN || in(c.keyboard[BTN_DOWN], vk)) out = Nav::Down;
    else if (vk == VK_LEFT || in(c.keyboard[BTN_LEFT], vk)) out = Nav::Left;
    else if (vk == VK_RIGHT || in(c.keyboard[BTN_RIGHT], vk)) out = Nav::Right;
    else if (repeat) return false;
    else if (vk == VK_RETURN || vk == VK_SPACE || in(c.keyboard[BTN_A], vk)) out = Nav::Confirm;
    else if (vk == VK_ESCAPE || vk == VK_BACK || in(c.keyboard[BTN_B], vk)) out = Nav::Back;
    else return false;
    return true;
}

void menu_key(WinState& ws, int vk, bool repeat) {
    SettingsMenu& m = *ws.menu;
    if (m.capturing_key()) {
        if (!repeat) m.captured_key(vk);
        return;
    }
    if (m.capturing_pad()) {
        if (vk == VK_ESCAPE && !repeat) m.cancel_capture();
        return;
    }
    if (vk == ws.cfg->key_menu && !repeat) { // fecha de qualquer página
        m.nav(Nav::Back);
        m.nav(Nav::Back);
        return;
    }
    Nav n;
    if (key_nav(*ws.cfg, vk, repeat, n)) m.nav(n);
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
        const Config& c = *ws->cfg;
        if ((vk == VK_RETURN && alt) || (vk == c.key_fullscreen && vk != VK_RETURN)) {
            if (!repeat) ws->req_fullscreen = true;
            return 0;
        }
        if (ws->menu && ws->menu->is_open()) {
            menu_key(*ws, vk, repeat);
            return 0;
        }
        if (ws->launcher) {
            Nav n;
            if (key_nav(c, vk, repeat, n)) ws->launcher->nav(n);
            else if (vk == c.key_menu && !repeat) ws->req_menu = true;
            return 0;
        }
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

// Desenha a imagem (3:2) com a maior escala que cabe, barras pretas no resto, e uma mensagem de
// status (se houver) no canto.
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

// Navegação pelo controle numa tela (direcional/analógico com repetição, A confirma, B volta).
template <class Target>
void pad_nav(Target& t, PadRepeat& rep, uint32_t raw, uint32_t pressed) {
    const uint32_t dirs = rep.step(raw & (kPadUp | kPadDown | kPadLeft | kPadRight | kPadSticks));
    if (dirs & (kPadUp | PAD_LS_UP)) t.nav(Nav::Up);
    if (dirs & (kPadDown | PAD_LS_DOWN)) t.nav(Nav::Down);
    if (dirs & (kPadLeft | PAD_LS_LEFT)) t.nav(Nav::Left);
    if (dirs & (kPadRight | PAD_LS_RIGHT)) t.nav(Nav::Right);
    if (pressed & kPadA) t.nav(Nav::Confirm);
    if (pressed & kPadB) t.nav(Nav::Back);
}

// ─────────────────────────────────────────────────────── ritmo de quadros ──
//
// Com V-Sync, cada quadro é mostrado numa atualização do monitor (DwmFlush espera a próxima
// composição do Windows): a 60 Hz o jogo roda a 60 fps exatos, sem quadros irregulares; a 120/240 Hz
// cada quadro fica 2/4 atualizações. Sem V-Sync (ou com o monitor numa taxa que não é múltipla,
// como 144 Hz), um relógio de alta resolução segura 60 fps (ou os 59,73 do GBA original).
// O áudio deixa de ser o relógio: segue a imagem com um ajuste de velocidade imperceptível (<1%).
struct FramePacer {
    double fps = 60.0;
    int flushes = 0; // >0: V-Sync, esperando esse número de composições por quadro
    clock::time_point next{}, last{};
    uint64_t shown = 0, late = 0; // diagnóstico: quadros mostrados e intervalos > 1,5 quadro

    static double refresh_rate(HWND hwnd) {
        DWM_TIMING_INFO ti{};
        ti.cbSize = sizeof ti;
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator &&
            ti.rateRefresh.uiNumerator)
            return static_cast<double>(ti.rateRefresh.uiNumerator) / ti.rateRefresh.uiDenominator;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
            EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            return dm.dmDisplayFrequency;
        return 0;
    }

    void configure(HWND hwnd, const Config& cfg) {
        const double target = cfg.fps60 ? 60.0 : 59.7275;
        fps = target;
        flushes = 0;
        BOOL composited = FALSE;
        const double hz = refresh_rate(hwnd);
        if (cfg.vsync && hz > 0 && SUCCEEDED(DwmIsCompositionEnabled(&composited)) && composited) {
            const int n = std::max(1, static_cast<int>(hz / target + 0.5));
            // 60 fps aceita 59,94/60 Hz (e múltiplos); o ritmo original só um monitor a ~59,73 Hz.
            const double tolerance = cfg.fps60 ? 1.0 : 0.05;
            if (std::abs(hz / n - target) < tolerance) flushes = n, fps = hz / n;
        }
    }

    void reset() { next = last = clock::now(), skip_stat = true; }
    bool skip_stat = true;

    void count(clock::time_point prev, clock::duration period) {
        if (!skip_stat) {
            ++shown;
            if (clock::now() - prev > period * 3 / 2) ++late;
        }
        skip_stat = false;
    }

    void wait() {
        const auto period = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / fps));
        const auto prev = last;
        if (flushes) {
            for (int i = 0; i < flushes; ++i) DwmFlush();
            // Janela minimizada ou coberta: o DWM não espera. Cai no relógio para não disparar.
            if (clock::now() - last < period / 2) {
                next = last + period;
                sleep_until(next);
            }
            last = next = clock::now();
            count(prev, period);
            return;
        }
        next += period;
        const auto now = clock::now();
        if (next > now) sleep_until(next);
        else if (now - next > period * 5) next = now; // muito atrasado (ex.: arrastando a janela)
        last = clock::now();
        count(prev, period);
    }

    static void sleep_until(clock::time_point t) {
        // Sleep até ~1 ms antes e o resto em espera ativa curta: precisão de microssegundos.
        for (;;) {
            const auto left = t - clock::now();
            if (left <= clock::duration::zero()) return;
            if (left > std::chrono::milliseconds(2))
                Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count() - 1));
            else
                YieldProcessor();
        }
    }
};

// Áudio acompanhando a imagem: cada quadro gera ~803,6 amostras (48 kHz / 59,7275); elas são
// reamostradas para durar exatamente um quadro mostrado, com uma correção de até ±0,5% conforme a
// fila do XAudio2 (alvo: 3 buffers). Fila vazia: completa com silêncio; fila cheia demais: descarta.
void submit_audio(AudioOut& out, const std::vector<int16_t>& in, double fps, int volume, std::vector<int16_t>& buf) {
    const size_t in_frames = in.size() / 2;
    if (!in_frames) return;
    uint32_t q = out.queued();
    if (q > 8) return;
    const double per_frame = gba::Apu::kSampleRate / fps;
    if (q == 0) { // começo ou depois de uma parada: dois quadros de folga
        buf.assign(static_cast<size_t>(per_frame) * 4, 0);
        out.submit(buf);
        q = 2;
    }
    const double adjust = std::clamp(1.0 + 0.0025 * (3.0 - static_cast<double>(q)), 0.995, 1.005);
    const size_t out_frames = std::max<size_t>(1, static_cast<size_t>(per_frame * adjust + 0.5));
    buf.resize(out_frames * 2);
    const double step = static_cast<double>(in_frames) / static_cast<double>(out_frames);
    for (size_t i = 0; i < out_frames; ++i) {
        const double pos = i * step;
        const size_t a = std::min(static_cast<size_t>(pos), in_frames - 1);
        const size_t b = std::min(a + 1, in_frames - 1);
        const double f = pos - static_cast<double>(a);
        for (int ch = 0; ch < 2; ++ch) {
            const double s = in[a * 2 + ch] * (1.0 - f) + in[b * 2 + ch] * f;
            buf[i * 2 + ch] = static_cast<int16_t>(std::clamp(s * volume / 100.0, -32768.0, 32767.0));
        }
    }
    out.submit(buf);
}

bool rom_ok(const fs::path& p) {
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) return false;
    auto rom = core::Rom::load(p);
    return rom && rom->is_supported();
}

// Um .gba compatível ao lado do executável ou em roms\.
fs::path find_rom_nearby() {
    const fs::path base = exe_dir();
    for (const fs::path& dir : {base, base / "roms", base / "rom"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::wstring ext = e.path().extension().wstring();
            for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
            if (ext == L".gba" && rom_ok(e.path())) return e.path();
        }
    }
    return {};
}

fs::path ask_for_rom(HWND owner) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrTitle = L"Select your Avatar: The Last Airbender (USA) GBA ROM";
    ofn.lpstrFilter = L"GBA ROM (*.gba)\0*.gba\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? fs::path(file) : fs::path();
}

// ─────────────────────────────────────────────────────────────── Session ──

struct Session {
    const Options& o;
    fs::path ini = exe_dir() / "tlagba.ini";
    Config cfg;
    int slot = 1;
    SettingsMenu menu{cfg, slot};
    WinState ws;
    HDC dc = nullptr;
    // Controle: estado anterior (para bordas) e botões a ignorar até serem soltos (os que
    // estavam segurados ao sair de uma tela, para não vazarem para o jogo).
    uint32_t pad_prev = 0, pad_suppress = 0;

    explicit Session(const Options& opts) : o(opts) {
        std::vector<std::string> warnings;
        cfg = load_config(ini, warnings);
        for (const auto& w : warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
        ws.cfg = &cfg;
        ws.menu = &menu;
        g_ws = &ws;

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
        ws.hwnd = CreateWindowW(wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
        if (cfg.fullscreen) set_fullscreen(ws, true);
        dc = GetDC(ws.hwnd);
        timeBeginPeriod(1);
    }

    ~Session() {
        timeEndPeriod(1);
        persist_config();
        g_ws = nullptr;
        ReleaseDC(ws.hwnd, dc);
        DestroyWindow(ws.hwnd);
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void pump() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void persist_config() {
        if (!menu.config_dirty()) return;
        menu.clear_dirty();
        if (!save_config(ini, cfg)) std::fprintf(stderr, "warning: could not write %s\n", ini.string().c_str());
    }

    void toggle_fullscreen_saved() {
        set_fullscreen(ws, !ws.fullscreen);
        cfg.fullscreen = ws.fullscreen;
        save_config(ini, cfg);
    }

    // Lê o controle e devolve os botões que acabaram de ser apertados.
    uint32_t poll(uint32_t& raw) {
        raw = poll_pad_raw(cfg.deadzone);
        const uint32_t pressed = raw & ~pad_prev;
        pad_prev = raw;
        pad_suppress &= raw;
        return pressed;
    }

    // Ao sair de uma tela: o que está segurado não conta para a próxima.
    void settle_inputs() {
        ws.down.fill(false);
        pad_prev = poll_pad_raw(cfg.deadzone);
        pad_suppress = pad_prev;
    }

    // Laço da tela de configurações. Com jogo (g não nulo), desenha sobre o quadro congelado e
    // trata save states e "sair"; no launcher, desenha sobre a arte de fundo. Devolve a ação que
    // fechou a tela (Resume, Quit ou LoadState).
    SettingsMenu::Action run_settings(gba::Gba* g, const uint32_t* background,
                                      const std::function<bool(int)>& save_slot,
                                      const std::function<bool(int)>& slot_exists) {
        ws.req_menu = false;
        std::array<uint32_t, gba::kScreenW * gba::kScreenH> frozen{};
        if (g) frozen = g->frame();
        menu.show(g != nullptr);
        PadRepeat rep;
        uint32_t raw = 0;
        poll(raw); // o que já está apertado não conta como novo
        using A = SettingsMenu::Action;
        A result = A::Resume;
        while (!ws.quit) {
            pump();
            if (ws.req_fullscreen) ws.req_fullscreen = false, toggle_fullscreen_saved();
            const uint32_t pressed = poll(raw);
            if (menu.capturing_pad()) {
                const uint32_t cand = pressed & ~kPadSticks;
                if (cand) menu.captured_pad(cand & (~cand + 1)); // o bit mais baixo pressionado
            } else if (pad_menu_combo(raw, pressed)) {
                break;
            } else {
                pad_nav(menu, rep, raw, pressed);
            }

            const A a = menu.take_action();
            if (a == A::Resume) break;
            if (a == A::Quit) {
                result = a;
                break;
            }
            if (a == A::LoadState) {
                if (slot_exists(slot)) {
                    result = a;
                    break;
                }
                menu.set_status("Slot " + std::to_string(slot) + " is empty");
            }
            if (a == A::SaveState)
                menu.set_status(save_slot(slot) ? "State saved to slot " + std::to_string(slot)
                                                : "Could not save the state");
            if (a == A::ToggleFullscreen) toggle_fullscreen_saved();
            if (a == A::ApplyScale) apply_scale(ws, cfg.scale);

            menu.set_fullscreen_flag(ws.fullscreen);
            if (g) menu.set_slot_exists(slot_exists(slot));
            {
                FramePacer probe;
                probe.configure(ws.hwnd, cfg);
                menu.set_display(FramePacer::refresh_rate(ws.hwnd), probe.flushes > 0);
            }
            draw(dc, ws.hwnd, g ? menu.render(frozen.data()) : menu.render_over(background), menu.bitmap_info(), {});
            Sleep(15);
        }
        menu.hide();
        persist_config();
        settle_inputs();
        return result;
    }

    // Uma partida. Devolve true se a janela foi fechada (encerrar o programa); false se o jogador
    // voltou ao launcher.
    // files_dir: onde ficam states e capturas (vazio: ao lado do .sav, como sempre).
    bool play(gba::Gba& g, const fs::path& save_path, bool app, const fs::path& files_dir = {}) {
        ws.leave = false;
        ws.paused = false;
        ws.req_menu = false;
        menu.set_quit_label(app ? L"Exit to launcher" : L"Quit game");
        settle_inputs();

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = gba::kScreenW;
        bmi.bmiHeader.biHeight = -gba::kScreenH;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        const fs::path rom_dir = files_dir.empty() ? fs::absolute(save_path).parent_path() : files_dir;
        const std::wstring stem = save_path.stem().wstring();
        auto slot_path = [&](int s) {
            return rom_dir / "states" / (stem + L".slot" + std::to_wstring(s) + L".state");
        };
        const std::function<bool(int)> slot_exists = [&](int s) { return fs::exists(slot_path(s)); };
        const std::function<bool(int)> save_slot = [&](int s) {
            const fs::path p = slot_path(s);
            fs::create_directories(p.parent_path());
            return write_file(p, g.save_state());
        };

        auto audio = o.mute ? nullptr : AudioOut::create(gba::Apu::kSampleRate);
        FramePacer pacer;
        pacer.configure(ws.hwnd, cfg);
        pacer.reset();
        const auto started = clock::now();
        const uint64_t first_frame = g.frame_count;
        const uint64_t max_frames = o.frames_given ? o.frames : 0;

        std::wstring status;
        clock::time_point status_until{};
        auto say = [&](const std::string& s) {
            status = widen(s);
            status_until = clock::now() + std::chrono::seconds(3);
        };
        say(std::string(key_name(cfg.key_menu)) + ", Esc or Xbox/home button: settings");
        auto current_status = [&]() -> std::wstring {
            if (ws.paused) return L"Paused (" + widen(key_name(cfg.key_pause)) + L" resumes)";
            return clock::now() < status_until ? status : std::wstring();
        };

        auto load_slot = [&](int s) {
            std::vector<uint8_t> bytes;
            if (!read_file(slot_path(s), bytes)) {
                say("Slot " + std::to_string(s) + " is empty");
                return;
            }
            try {
                say("State loaded from slot " + std::to_string(s));
                g.load_state_and_restart(bytes); // não retorna: recomeça a partir do estado
            } catch (const std::runtime_error& e) {
                say(std::string("Could not load: ") + e.what());
            }
        };

        // Ações que valem com o jogo rodando ou pausado. Carregar estado desempilha (não retorna).
        auto handle_requests = [&] {
            if (ws.req_fullscreen) ws.req_fullscreen = false, set_fullscreen(ws, !ws.fullscreen);
            if (ws.slot_delta) {
                slot = ((slot + ws.slot_delta) % 10 + 10) % 10;
                ws.slot_delta = 0;
                say("Slot " + std::to_string(slot) + (slot_exists(slot) ? "" : " (empty)"));
            }
            if (ws.req_shot) {
                ws.req_shot = false;
                const fs::path p = rom_dir / "screenshots" / (stem + L"_" + widen(timestamp()) + L".png");
                fs::create_directories(p.parent_path());
                const std::u8string name = p.filename().u8string();
                say(png::write_rgb(p.string(), g.frame().data(), gba::kScreenW, gba::kScreenH)
                        ? "Screenshot saved: " + std::string(name.begin(), name.end())
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

        std::vector<int16_t> scaled;
        g.run([&] {
            pump();
            if (ws.quit || ws.leave) return false;
            uint32_t raw = 0;
            const uint32_t pressed = poll(raw);
            if (pad_menu_combo(raw, pressed)) ws.req_menu = true;
            if (ws.req_menu) {
                using A = SettingsMenu::Action;
                const A a = run_settings(&g, nullptr, save_slot, slot_exists);
                pacer.configure(ws.hwnd, cfg); // V-Sync / taxa podem ter mudado
                pacer.reset();
                if (a == A::Quit) {
                    if (app) ws.leave = true;
                    else ws.quit = true;
                }
                if (ws.quit || ws.leave) return false;
                if (a == A::LoadState) load_slot(slot); // em caso de sucesso, não retorna
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
                pacer.reset();
            }
            if (ws.quit) return false;

            if (g.frame_count % 60 == 0) store_save(g, save_path);
            auto samples = g.apu.take_samples();
            const bool turbo = cfg.key_turbo && ws.down[cfg.key_turbo];
            if (turbo) {
                pacer.reset(); // sem limite de velocidade; o áudio acelerado é descartado
            } else {
                if (audio) submit_audio(*audio, samples, pacer.fps, cfg.volume, scaled);
                if (g.frame_count % 120 == 0) pacer.configure(ws.hwnd, cfg); // mudou de monitor?
                pacer.wait();
            }
            g.set_keys(static_cast<uint16_t>(keyboard_keys(ws) | pad_to_gba(cfg, raw & ~pad_suppress)));
            return !max_frames || g.frame_count < max_frames;
        });

        const double secs = std::chrono::duration<double>(clock::now() - started).count();
        const uint64_t frames = g.frame_count - first_frame;
        std::printf("%llu frames in %.2f s (%.2f fps, target %.2f%s), %s mode\n",
                    static_cast<unsigned long long>(frames), secs, frames / secs, pacer.fps,
                    pacer.flushes ? ", V-Sync" : "", g.has_recomp() ? "recompiled" : "interpreter");
        std::printf("late frames (> 1.5 frame interval): %llu of %llu\n", static_cast<unsigned long long>(pacer.late),
                    static_cast<unsigned long long>(pacer.shown));
        store_save(g, save_path);
        return ws.quit;
    }

    // Launcher: devolve true para jogar a ROM em `rom`, false para sair.
    bool run_launcher(Launcher& l, fs::path& rom) {
        ws.launcher = &l;
        ws.req_menu = false;
        auto show_rom = [&] { l.set_rom(rom.empty() ? std::wstring() : rom.filename().wstring(), rom_ok(rom)); };
        show_rom();
        settle_inputs();
        PadRepeat rep;
        bool start = false;
        const std::function<bool(int)> no_slot = [](int) { return false; };
        auto open_settings = [&] {
            ws.launcher = nullptr;
            run_settings(nullptr, l.background(), no_slot, no_slot);
            ws.launcher = &l;
        };
        while (!ws.quit) {
            pump();
            if (ws.req_fullscreen) ws.req_fullscreen = false, toggle_fullscreen_saved();
            uint32_t raw = 0;
            const uint32_t pressed = poll(raw);
            if (ws.req_menu || pad_menu_combo(raw, pressed)) open_settings();
            else pad_nav(l, rep, raw, pressed);

            using A = Launcher::Action;
            const A a = l.take_action();
            if (a == A::Play) {
                start = true;
                break;
            }
            if (a == A::Quit) break;
            if (a == A::Settings) open_settings();
            if (a == A::SelectRom) {
                const bool was_full = ws.fullscreen;
                if (was_full) set_fullscreen(ws, false); // a janela de arquivos não fica por trás
                const fs::path chosen = ask_for_rom(ws.hwnd);
                if (was_full) set_fullscreen(ws, true);
                settle_inputs();
                if (!chosen.empty()) {
                    if (rom_ok(chosen)) {
                        rom = chosen;
                        cfg.rom = rom;
                        save_config(ini, cfg);
                        show_rom();
                        l.set_status("ROM verified. Ready to play!");
                    } else {
                        l.set_status("That file is not the expected ROM (Avatar, USA, BQZE)");
                    }
                }
            }
            if (ws.quit) break;
            draw(dc, ws.hwnd, l.render(), l.bitmap_info(), {});
            Sleep(15);
        }
        ws.launcher = nullptr;
        settle_inputs();
        return start && !ws.quit;
    }
};

} // namespace

// tlagba: só a partida, sem launcher.
int run_window(gba::Gba& g, const Options& o, const fs::path& save_path) {
    Session s(o);
    s.play(g, save_path, false);
    return 0;
}

// AvatarRecomp.exe: launcher ⇄ jogo, na mesma janela.
//
// Aberto por um launcher externo (--game <rom>, ou RECOMPLAUNCHER=1 do RecompLauncher) com uma ROM
// válida: vai direto ao jogo e fecha ao sair dele. --data <pasta> leva saves, states e capturas
// para essa pasta. Sem isso, tudo como antes.
int run_app(const Options& o) {
    Session s(o);
    auto env_is = [](const char* name, const char* value) {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
        return n > 0 && n < sizeof buf && std::strcmp(buf, value) == 0;
    };
    if (env_is("RECOMPLAUNCHER_MODE", "bigpicture")) set_fullscreen(s.ws, true); // só nesta sessão
    const bool direct = (o.game_given || env_is("RECOMPLAUNCHER", "1")) && rom_ok(o.rom);
    auto paths_for = [&](const fs::path& rom, fs::path& save_path, fs::path& files_dir) {
        if (o.data.empty()) {
            save_path = rom;
            save_path.replace_extension(".sav");
            files_dir.clear();
            return;
        }
        fs::create_directories(o.data / "saves");
        save_path = o.data / "saves" / rom.filename().replace_extension(".sav");
        files_dir = o.data;
    };
    if (direct) {
        std::string err;
        auto g = make_game(o.rom, false, err);
        if (!g) throw std::runtime_error(err);
        fs::path save_path, files_dir;
        paths_for(o.rom, save_path, files_dir);
        load_save(*g, save_path);
        s.play(*g, save_path, false, files_dir);
        return 0;
    }

    fs::path rom = o.rom;
    if (!rom_ok(rom)) rom = rom_ok(s.cfg.rom) ? s.cfg.rom : find_rom_nearby();
    if (rom_ok(rom) && rom != s.cfg.rom) {
        s.cfg.rom = rom;
        save_config(s.ini, s.cfg);
    }
    if (rom.empty()) rom = o.rom; // ROM inválida arrastada: o launcher mostra o problema

    Launcher launcher;
    bool skip = !s.cfg.launcher && rom_ok(rom); // "Launcher at startup: Off" vale só ao abrir
    while (!s.ws.quit) {
        if (!skip && !s.run_launcher(launcher, rom)) break;
        skip = false;
        std::string err;
        auto g = make_game(rom, false, err);
        if (!g) {
            launcher.set_status("Could not start: " + err);
            continue;
        }
        fs::path save_path, files_dir;
        paths_for(rom, save_path, files_dir);
        load_save(*g, save_path);
        if (s.play(*g, save_path, true, files_dir)) break;
    }
    return 0;
}

} // namespace fe

#endif
