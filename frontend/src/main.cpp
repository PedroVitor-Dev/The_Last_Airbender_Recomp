// tlagba — executa o jogo: código recompilado + runtime (com interpretador de fallback).
//
//   tlagba <rom>                                   janela Win32 (escala 3x)
//   tlagba <rom> --headless --frames N [opções]    sem janela, para testes
//
// Opções:
//   --interp                    ignora o código recompilado (só interpretador)
//   --shot <arquivo.png>        captura do último quadro (headless)
//   --shot-every <K> <prefixo>  captura a cada K quadros: <prefixo>_<quadro>.png
//   --press <TECLA>@<ini>[-<fim>]  segura a tecla nesses quadros (repetível)
//                               TECLAS: A B SELECT START RIGHT LEFT UP DOWN R L
//   --scale <n>                 escala da janela (padrão 3)
//   --wav <arquivo.wav>         grava o áudio gerado (headless)
//   --mute                      janela sem áudio (ritmo pelo relógio)
//   --fallback-log <arquivo>    lista endereços executados pelo interpretador
//   --dump-ram <prefixo>        grava <prefixo>_ewram.bin e <prefixo>_iwram.bin no fim (headless)
//
// Teclado (janela): setas, X=A, Z=B, Enter=Start, Backspace=Select, A=L, S=R, Esc=sair.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio_out.h"
#include "core/rom.h"
#include "gba/gba.h"
#include "png.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif

// Tabela gerada pelo recompilador (ou vazia, sem código gerado).
extern const gba::RecompEntry tla_recomp_table[];
extern const size_t tla_recomp_count;
extern const gba::RecompOverlay tla_recomp_overlays[];
extern const size_t tla_recomp_overlay_count;

namespace fs = std::filesystem;

namespace {

struct Press {
    uint16_t key;
    uint64_t from, to;
};

struct Options {
    fs::path rom;
    bool headless = false, mute = false, interp = false;
    uint64_t frames = 600, shot_every = 0;
    bool frames_given = false;
    std::string shot, shot_prefix = "frame", wav, fallback_log, dump_ram;
    int scale = 3;
    std::vector<Press> script;
};

uint16_t key_from_name(const std::string& n) {
    static const std::pair<const char*, uint16_t> keys[] = {
        {"A", gba::KEY_A}, {"B", gba::KEY_B}, {"SELECT", gba::KEY_SELECT}, {"START", gba::KEY_START},
        {"RIGHT", gba::KEY_RIGHT}, {"LEFT", gba::KEY_LEFT}, {"UP", gba::KEY_UP}, {"DOWN", gba::KEY_DOWN},
        {"R", gba::KEY_R}, {"L", gba::KEY_L}};
    for (const auto& [name, k] : keys)
        if (n == name) return k;
    return 0;
}

bool parse_press(const std::string& s, Press& p) {
    const auto at = s.find('@');
    if (at == std::string::npos) return false;
    p.key = key_from_name(s.substr(0, at));
    const std::string range = s.substr(at + 1);
    const auto dash = range.find('-');
    p.from = std::stoull(range.substr(0, dash));
    p.to = dash == std::string::npos ? p.from + 5 : std::stoull(range.substr(dash + 1));
    return p.key != 0;
}

uint16_t scripted_keys(const std::vector<Press>& script, uint64_t frame) {
    uint16_t k = 0;
    for (const auto& p : script)
        if (frame >= p.from && frame <= p.to) k |= p.key;
    return k;
}

void load_save(gba::Gba& g, const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    auto& d = g.bus.eeprom.data();
    f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

void store_save(gba::Gba& g, const fs::path& path) {
    if (!g.bus.eeprom.dirty()) return;
    std::ofstream f(path, std::ios::binary);
    const auto& d = g.bus.eeprom.data();
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    g.bus.eeprom.clear_dirty();
}

bool write_wav(const std::string& path, const std::vector<int16_t>& s, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(s.size() * 2);
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(s.data()), bytes);
    return true;
}

void print_stats(const gba::Gba& g, const Options& o) {
    for (int ch = 0; ch < 2; ++ch) {
        if (!g.io.fifo_ticks[ch]) continue;
        std::printf("FIFO %c: %llu amostras, %llu underruns\n", 'A' + ch,
                    static_cast<unsigned long long>(g.io.fifo_ticks[ch]),
                    static_cast<unsigned long long>(g.io.fifo_underruns[ch]));
    }
    std::printf("Execução: %llu chamadas recompiladas, %llu instruções interpretadas, %zu pontos de fallback\n",
                static_cast<unsigned long long>(g.recomp_calls), static_cast<unsigned long long>(g.cpu.instr_count),
                g.fallback_entries.size());
    std::vector<std::pair<uint32_t, uint64_t>> fb(g.fallback_entries.begin(), g.fallback_entries.end());
    std::sort(fb.begin(), fb.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < fb.size() && i < 8; ++i)
        std::printf("  fallback 0x%08X (%s) × %llu, %llu instruções\n", fb[i].first & ~1u,
                    (fb[i].first & 1) ? "thumb" : "arm", static_cast<unsigned long long>(fb[i].second),
                    static_cast<unsigned long long>(g.fallback_instrs.count(fb[i].first)
                                                        ? g.fallback_instrs.at(fb[i].first) : 0));
    std::vector<std::pair<uint32_t, uint64_t>> reg(g.interp_regions.begin(), g.interp_regions.end());
    std::sort(reg.begin(), reg.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < reg.size() && i < 5; ++i)
        std::printf("  interpretado a partir de 0x%08X (%s): %llu instruções\n", reg[i].first & ~1u,
                    (reg[i].first & 1) ? "thumb" : "arm", static_cast<unsigned long long>(reg[i].second));
    if (!o.fallback_log.empty()) {
        std::ofstream f(o.fallback_log);
        for (auto& [a, n] : fb) {
            char line[64];
            std::snprintf(line, sizeof line, "0x%08X %s %llu\n", a & ~1u, (a & 1) ? "thumb" : "arm",
                          static_cast<unsigned long long>(n));
            f << line;
        }
    }
}

int run_headless(gba::Gba& g, const Options& o) {
    std::vector<int16_t> audio;
    const auto t0 = std::chrono::steady_clock::now();
    g.set_keys(scripted_keys(o.script, 0));
    g.run([&] {
        const uint64_t f = g.frame_count; // quadros completos
        auto chunk = g.apu.take_samples();
        if (!o.wav.empty()) audio.insert(audio.end(), chunk.begin(), chunk.end());
        if (o.shot_every && f % o.shot_every == 0)
            png::write_rgb(o.shot_prefix + "_" + std::to_string(f) + ".png", g.frame().data(), gba::kScreenW,
                           gba::kScreenH);
        g.set_keys(scripted_keys(o.script, f));
        return f < o.frames;
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%llu quadros em %.2f s (%.1f fps, %.1fx tempo real), modo %s\n",
                static_cast<unsigned long long>(g.frame_count), secs, g.frame_count / secs,
                g.frame_count / secs / 59.7275, g.has_recomp() ? "recompilado" : "interpretador");
    print_stats(g, o);
    if (!o.dump_ram.empty()) {
        std::ofstream(o.dump_ram + "_ewram.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(g.bus.ewram.data()), g.bus.ewram.size());
        std::ofstream(o.dump_ram + "_iwram.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(g.bus.iwram.data()), g.bus.iwram.size());
    }
    if (!o.shot.empty()) png::write_rgb(o.shot, g.frame().data(), gba::kScreenW, gba::kScreenH);
    if (!o.wav.empty()) {
        write_wav(o.wav, audio, gba::Apu::kSampleRate);
        std::printf("áudio: %zu amostras estéreo em %s\n", audio.size() / 2, o.wav.c_str());
    }
    return 0;
}

#ifdef _WIN32
uint16_t g_keys = 0;

uint16_t vk_to_key(WPARAM vk) {
    switch (vk) {
    case VK_UP: return gba::KEY_UP;
    case VK_DOWN: return gba::KEY_DOWN;
    case VK_LEFT: return gba::KEY_LEFT;
    case VK_RIGHT: return gba::KEY_RIGHT;
    case 'X': return gba::KEY_A;
    case 'Z': return gba::KEY_B;
    case VK_RETURN: return gba::KEY_START;
    case VK_BACK: return gba::KEY_SELECT;
    case 'A': return gba::KEY_L;
    case 'S': return gba::KEY_R;
    default: return 0;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostQuitMessage(0);
        g_keys |= vk_to_key(wp);
        return 0;
    case WM_KEYUP: g_keys &= static_cast<uint16_t>(~vk_to_key(wp)); return 0;
    case WM_KILLFOCUS: g_keys = 0; return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int run_window(gba::Gba& g, const Options& o, const fs::path& save_path) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TlaRecomp";
    RegisterClassW(&wc);
    RECT rc{0, 0, gba::kScreenW * o.scale, gba::kScreenH * o.scale};
    const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Avatar: The Last Airbender — Recomp", style | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                              nullptr, inst, nullptr);
    HDC dc = GetDC(hwnd);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = gba::kScreenW;
    bmi.bmiHeader.biHeight = -gba::kScreenH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    auto audio = o.mute ? nullptr : AudioOut::create(gba::Apu::kSampleRate);
    timeBeginPeriod(1);
    using clock = std::chrono::steady_clock;
    const auto frame_time = std::chrono::nanoseconds(16742706); // 59,7275 Hz
    const auto started = clock::now();
    auto next = started;
    const uint64_t max_frames = o.frames_given ? o.frames : 0;

    g.run([&] {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        StretchDIBits(dc, 0, 0, gba::kScreenW * o.scale, gba::kScreenH * o.scale, 0, 0, gba::kScreenW,
                      gba::kScreenH, g.frame().data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        if (g.frame_count % 60 == 0) store_save(g, save_path);
        auto samples = g.apu.take_samples();
        if (audio) {
            audio->submit(samples); // áudio como relógio: ~3 quadros na fila
            while (audio->queued() > 3) Sleep(1);
        } else {
            next += frame_time;
            const auto now = clock::now();
            if (next > now)
                Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count()));
            else if (now - next > frame_time * 5)
                next = now;
        }
        g.set_keys(g_keys);
        return !max_frames || g.frame_count < max_frames;
    });

    timeEndPeriod(1);
    const double secs = std::chrono::duration<double>(clock::now() - started).count();
    std::printf("%llu quadros em %.2f s (%.2f fps, alvo 59,73)%s, modo %s\n",
                static_cast<unsigned long long>(g.frame_count), secs, g.frame_count / secs,
                audio ? ", ritmo pelo áudio" : "", g.has_recomp() ? "recompilado" : "interpretador");
    DestroyWindow(hwnd);
    ReleaseDC(hwnd, dc);
    store_save(g, save_path);
    return 0;
}
#endif

bool parse_args(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.rom = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--headless") o.headless = true;
        else if (a == "--interp") o.interp = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "--frames" && i + 1 < argc) o.frames = std::stoull(argv[++i]), o.frames_given = true;
        else if (a == "--shot" && i + 1 < argc) o.shot = argv[++i];
        else if (a == "--shot-every" && i + 2 < argc) o.shot_every = std::stoull(argv[++i]), o.shot_prefix = argv[++i];
        else if (a == "--scale" && i + 1 < argc) o.scale = std::stoi(argv[++i]);
        else if (a == "--wav" && i + 1 < argc) o.wav = argv[++i];
        else if (a == "--fallback-log" && i + 1 < argc) o.fallback_log = argv[++i];
        else if (a == "--dump-ram" && i + 1 < argc) o.dump_ram = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            Press p{};
            if (!parse_press(argv[++i], p)) {
                std::fprintf(stderr, "erro: --press inválido: %s\n", argv[i]);
                return false;
            }
            o.script.push_back(p);
        } else {
            std::fprintf(stderr, "erro: opção desconhecida: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        std::fprintf(stderr, "uso: tlagba <rom.gba> [--headless --frames N] [--interp] [--shot f.png] "
                             "[--shot-every K prefixo] [--press TECLA@ini-fim] [--wav f.wav] [--mute] "
                             "[--scale n] [--fallback-log arq]\n");
        return 2;
    }

    std::string err;
    auto rom = core::Rom::load(o.rom, &err);
    if (!rom) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    if (!rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM não suportada (esperado %s)\n", core::kExpectedGameCode);
        return 1;
    }

    auto g = std::make_unique<gba::Gba>(*rom);
    if (!o.interp)
        g->set_recomp(tla_recomp_table, tla_recomp_count, tla_recomp_overlays, tla_recomp_overlay_count);
    fs::path save_path = o.rom;
    save_path.replace_extension(".sav");
    load_save(*g, save_path);

    try {
#ifdef _WIN32
        if (!o.headless) return run_window(*g, o, save_path);
#endif
        const int rc = run_headless(*g, o);
        store_save(*g, save_path);
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro fatal: %s (quadro %llu)\n", e.what(),
                     static_cast<unsigned long long>(g->frame_count));
        if (!o.shot.empty()) png::write_rgb(o.shot, g->frame().data(), gba::kScreenW, gba::kScreenH);
        return 1;
    }
}
