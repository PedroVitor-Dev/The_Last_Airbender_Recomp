// tlagba — executa o jogo sobre o runtime (interpretador por enquanto).
//
//   tlagba <rom>                                   janela Win32 (escala 3x)
//   tlagba <rom> --headless --frames N [opções]    sem janela, para testes
//
// Opções:
//   --shot <arquivo.png>        captura do último quadro (headless)
//   --shot-every <K> <prefixo>  captura a cada K quadros: <prefixo>_<quadro>.png
//   --press <TECLA>@<ini>[-<fim>]  segura a tecla nesses quadros (repetível)
//                               TECLAS: A B SELECT START RIGHT LEFT UP DOWN R L
//   --scale <n>                 escala da janela (padrão 3)
//
// Teclado (janela): setas, X=A, Z=B, Enter=Start, Backspace=Select, A=L, S=R, Esc=sair.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/rom.h"
#include "gba/gba.h"
#include "png.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif

namespace fs = std::filesystem;

namespace {

struct Press {
    uint16_t key;
    uint64_t from, to;
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

int run_headless(gba::Gba& g, uint64_t frames, const std::string& shot, uint64_t shot_every,
                 const std::string& shot_prefix, const std::vector<Press>& script) {
    const auto t0 = std::chrono::steady_clock::now();
    for (uint64_t f = 0; f < frames; ++f) {
        g.set_keys(scripted_keys(script, f));
        g.run_frame();
        if (shot_every && (f + 1) % shot_every == 0) {
            png::write_rgb(shot_prefix + "_" + std::to_string(f + 1) + ".png", g.frame().data(), gba::kScreenW,
                           gba::kScreenH);
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%llu quadros em %.2f s (%.1f fps, %.1fx tempo real), %llu instruções, PC=0x%08X\n",
                static_cast<unsigned long long>(frames), secs, frames / secs, frames / secs / 59.7275,
                static_cast<unsigned long long>(g.cpu.instr_count), g.cpu.pc());
    if (!shot.empty()) png::write_rgb(shot, g.frame().data(), gba::kScreenW, gba::kScreenH);
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

int run_window(gba::Gba& g, int scale, const fs::path& save_path) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"TlaRecomp";
    RegisterClassW(&wc);
    RECT rc{0, 0, gba::kScreenW * scale, gba::kScreenH * scale};
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

    timeBeginPeriod(1);
    using clock = std::chrono::steady_clock;
    const auto frame_time = std::chrono::nanoseconds(16742706); // 59,7275 Hz
    auto next = clock::now();
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        g.set_keys(g_keys);
        g.run_frame();
        StretchDIBits(dc, 0, 0, gba::kScreenW * scale, gba::kScreenH * scale, 0, 0, gba::kScreenW, gba::kScreenH,
                      g.frame().data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        if (g.frame_count % 60 == 0) store_save(g, save_path);
        next += frame_time;
        const auto now = clock::now();
        if (next > now) {
            Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count()));
        } else if (now - next > frame_time * 5) {
            next = now; // atrasou demais: não tenta compensar
        }
    }
    timeEndPeriod(1);
    ReleaseDC(hwnd, dc);
    store_save(g, save_path);
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: tlagba <rom.gba> [--headless --frames N] [--shot f.png] "
                             "[--shot-every K prefixo] [--press TECLA@ini-fim] [--scale n]\n");
        return 2;
    }
    const fs::path rom_path = argv[1];
    bool headless = false;
    uint64_t frames = 600, shot_every = 0;
    std::string shot, shot_prefix = "frame";
    int scale = 3;
    std::vector<Press> script;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--headless") headless = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::stoull(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--shot-every" && i + 2 < argc) shot_every = std::stoull(argv[++i]), shot_prefix = argv[++i];
        else if (a == "--scale" && i + 1 < argc) scale = std::stoi(argv[++i]);
        else if (a == "--press" && i + 1 < argc) {
            Press p{};
            if (!parse_press(argv[++i], p)) {
                std::fprintf(stderr, "erro: --press inválido: %s\n", argv[i]);
                return 2;
            }
            script.push_back(p);
        } else {
            std::fprintf(stderr, "erro: opção desconhecida: %s\n", a.c_str());
            return 2;
        }
    }

    std::string err;
    auto rom = core::Rom::load(rom_path, &err);
    if (!rom) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    if (!rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM não suportada (esperado %s)\n", core::kExpectedGameCode);
        return 1;
    }

    auto g = std::make_unique<gba::Gba>(*rom);
    fs::path save_path = rom_path;
    save_path.replace_extension(".sav");
    load_save(*g, save_path);

    try {
        int rc;
#ifdef _WIN32
        if (!headless) rc = run_window(*g, scale, save_path);
        else
#endif
            rc = run_headless(*g, frames, shot, shot_every, shot_prefix, script);
        if (headless) store_save(*g, save_path);
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "erro fatal: %s (quadro %llu)\n", e.what(), static_cast<unsigned long long>(g->frame_count));
        if (!shot.empty()) png::write_rgb(shot, g->frame().data(), gba::kScreenW, gba::kScreenH);
        return 1;
    }
}
