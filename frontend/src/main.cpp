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
//   --profile                   tempo por subsistema (headless)
//   --dump-ram <prefixo>        grava <prefixo>_ewram.bin e <prefixo>_iwram.bin no fim (headless)
//   --load-state <arquivo>      carrega um save state antes de começar
//   --save-state-at <N> <arq>   salva o estado ao fim do quadro N (headless)
//   --load-state-at <N> <arq>   carrega o estado ao fim do quadro N, no meio da execução (headless)
//
// Janela: teclas, controle e atalhos no tlagba.ini (criado ao lado do executável). Padrões:
// setas, X=A, Z=B, Enter=Start, Backspace=Select, A=L, S=R; F5 salva / F9 carrega estado,
// F6/F7 trocam o slot, Tab acelera, P pausa, F11 ou Alt+Enter tela cheia, F12 captura, Esc sai.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/rom.h"
#include "config.h"
#include "frontend.h"
#include "gba/gba.h"
#include "gba/profile.h"
#include "png.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <cwctype>
#endif

// Tabela gerada pelo recompilador (ou vazia, sem código gerado).
extern const gba::RecompEntry tla_recomp_table[];
extern const size_t tla_recomp_count;
extern const gba::RecompOverlay tla_recomp_overlays[];
extern const size_t tla_recomp_overlay_count;

namespace fs = std::filesystem;

// ─────────────────────────────────────────── arquivos (compartilhados) ──
namespace fe {

bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size())));
}

bool write_file(const fs::path& path, const std::vector<uint8_t>& data) {
    // Grava num temporário e troca: um save state nunca fica pela metade.
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f || !f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))
            return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

void store_save(gba::Gba& g, const fs::path& path) {
    if (!g.bus.eeprom.dirty()) return;
    std::ofstream f(path, std::ios::binary);
    const auto& d = g.bus.eeprom.data();
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    g.bus.eeprom.clear_dirty();
}

fs::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#endif
    return fs::current_path();
}

} // namespace fe

namespace {

using fe::Options;
using fe::Press;
using fe::store_save;

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
    bool loaded_mid_run = false;
    const auto t0 = std::chrono::steady_clock::now();
    g.set_keys(scripted_keys(o.script, 0));
    gba::prof::enabled = o.profile;
    g.render = !o.no_render;
    g.run([&] {
        const uint64_t f = g.frame_count; // quadros completos
        auto chunk = g.apu.take_samples();
        if (!o.wav.empty()) audio.insert(audio.end(), chunk.begin(), chunk.end());
        if (o.shot_every && f % o.shot_every == 0)
            png::write_rgb(o.shot_prefix + "_" + std::to_string(f) + ".png", g.frame().data(), gba::kScreenW,
                           gba::kScreenH);
        g.set_keys(scripted_keys(o.script, f));
        if (o.save_state_at && f == o.save_state_at) {
            if (!fe::write_file(o.save_state_path, g.save_state()))
                std::fprintf(stderr, "erro: não foi possível gravar %s\n", o.save_state_path.string().c_str());
        }
        if (o.load_state_at && f == o.load_state_at && !loaded_mid_run) {
            loaded_mid_run = true;
            std::vector<uint8_t> bytes;
            if (!fe::read_file(o.load_state_at_path, bytes)) throw std::runtime_error("save state not found");
            g.load_state_and_restart(bytes); // não retorna
        }
        return f < o.frames;
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%llu quadros em %.2f s (%.1f fps, %.1fx tempo real), modo %s\n",
                static_cast<unsigned long long>(g.frame_count), secs, g.frame_count / secs,
                g.frame_count / secs / 59.7275, g.has_recomp() ? "recompilado" : "interpretador");
    print_stats(g, o);
    if (o.profile) {
        const auto& c = gba::prof::counters;
        const double total = secs * 1e9;
        // sync inclui o PPU; APU é chamada tanto dentro quanto fora do sync.
        const double cpu = total - static_cast<double>(c.sync_ns) - static_cast<double>(c.apu_ns);
        std::printf("Perfil (%.2f s): PPU %.1f%%, APU %.1f%%, sync (sem PPU) %.1f%%, CPU/recomp+resto %.1f%% "
                    "(%llu syncs, %.1f por quadro)\n",
                    secs, 100.0 * c.ppu_ns / total, 100.0 * c.apu_ns / total,
                    100.0 * (static_cast<double>(c.sync_ns) - c.ppu_ns) / total, 100.0 * cpu / total,
                    static_cast<unsigned long long>(c.syncs), static_cast<double>(c.syncs) / g.frame_count);
        std::printf("  PPU: BGs %.1f%%, sprites %.1f%%, composição %.1f%%\n", 100.0 * c.bg_ns / total,
                    100.0 * c.obj_ns / total, 100.0 * c.mix_ns / total);
        if (c.lines)
            std::printf("  linhas: %llu, com janela %.0f%%, com efeito de cor %.0f%%, com sprite semitransparente %.0f%%\n",
                        static_cast<unsigned long long>(c.lines), 100.0 * c.lines_win / c.lines,
                        100.0 * c.lines_blend / c.lines, 100.0 * c.lines_semi / c.lines);
    }
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


bool parse_args(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.rom = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--headless") o.headless = true;
        else if (a == "--interp") o.interp = true;
        else if (a == "--profile") o.profile = true;
        else if (a == "--no-render") o.no_render = true;
        else if (a == "--mute") o.mute = true;
        else if (a == "--frames" && i + 1 < argc) o.frames = std::stoull(argv[++i]), o.frames_given = true;
        else if (a == "--shot" && i + 1 < argc) o.shot = argv[++i];
        else if (a == "--shot-every" && i + 2 < argc) o.shot_every = std::stoull(argv[++i]), o.shot_prefix = argv[++i];
        else if (a == "--scale" && i + 1 < argc) o.scale = std::stoi(argv[++i]);
        else if (a == "--wav" && i + 1 < argc) o.wav = argv[++i];
        else if (a == "--fallback-log" && i + 1 < argc) o.fallback_log = argv[++i];
        else if (a == "--dump-ram" && i + 1 < argc) o.dump_ram = argv[++i];
        else if (a == "--load-state" && i + 1 < argc) o.load_state = argv[++i];
        else if (a == "--save-state-at" && i + 2 < argc)
            o.save_state_at = std::stoull(argv[++i]), o.save_state_path = argv[++i];
        else if (a == "--load-state-at" && i + 2 < argc)
            o.load_state_at = std::stoull(argv[++i]), o.load_state_at_path = argv[++i];
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

// Erro para o usuário: caixa de diálogo no executável distribuído, stderr no tlagba.
void fatal(const std::string& msg) {
#if defined(_WIN32) && defined(TLA_GUI)
    const int n = MultiByteToWideChar(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, msg.data(), static_cast<int>(msg.size()), w.data(), n);
    MessageBoxW(nullptr, w.c_str(), L"Avatar: The Last Airbender Recomp", MB_OK | MB_ICONERROR);
#else
    std::fprintf(stderr, "error: %s\n", msg.c_str());
#endif
}

#if defined(_WIN32) && defined(TLA_GUI)
// Executável distribuído: a ROM vem do argumento (arrastar a ROM sobre o .exe), da última usada
// (tlagba.ini), de um .gba compatível ao lado do executável ou em roms\, ou de uma janela de
// seleção. A escolhida fica gravada no ini para as próximas vezes.
bool is_supported_rom(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    auto rom = core::Rom::load(p);
    return rom && rom->is_supported();
}

fs::path find_rom_nearby() {
    const fs::path base = fe::exe_dir();
    for (const fs::path& dir : {base, base / "roms", base / "rom"}) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            std::wstring ext = e.path().extension().wstring();
            for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
            if (ext == L".gba" && is_supported_rom(e.path())) return e.path();
        }
    }
    return {};
}

fs::path ask_for_rom() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrTitle = L"Select your Avatar: The Last Airbender (USA) GBA ROM";
    ofn.lpstrFilter = L"GBA ROM (*.gba)\0*.gba\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameW(&ofn) ? fs::path(file) : fs::path();
}

// Devolve false se o usuário desistiu.
bool resolve_gui_rom(Options& o) {
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv && wargc >= 2 && wargv[1][0] != L'-') o.rom = wargv[1];
    if (wargv) LocalFree(wargv);

    const fs::path ini = fe::exe_dir() / "tlagba.ini";
    std::vector<std::string> warnings;
    fe::Config cfg = fe::load_config(ini, warnings);
    if (o.rom.empty() && is_supported_rom(cfg.rom)) o.rom = cfg.rom;
    if (o.rom.empty()) o.rom = find_rom_nearby();
    while (o.rom.empty() || !is_supported_rom(o.rom)) {
        if (!o.rom.empty())
            MessageBoxW(nullptr,
                        L"This file is not the expected ROM.\n\nYou need Avatar: The Last Airbender for the "
                        L"Game Boy Advance, USA version (game code BQZE).",
                        L"Avatar: The Last Airbender Recomp", MB_OK | MB_ICONWARNING);
        o.rom = ask_for_rom();
        if (o.rom.empty()) return false;
    }
    if (cfg.rom != o.rom) {
        cfg.rom = o.rom;
        fe::save_config(ini, cfg);
    }
    return true;
}
#endif

} // namespace

int main(int argc, char** argv) {
    Options o;
#if defined(_WIN32) && defined(TLA_GUI)
    (void)argc;
    (void)argv;
    if (!resolve_gui_rom(o)) return 0;
#else
    if (!parse_args(argc, argv, o)) {
        std::fprintf(stderr, "uso: tlagba <rom.gba> [--headless --frames N] [--interp] [--shot f.png] "
                             "[--shot-every K prefixo] [--press TECLA@ini-fim] [--wav f.wav] [--mute] "
                             "[--scale n] [--fallback-log arq] [--load-state arq] [--save-state-at N arq] "
                             "[--load-state-at N arq]\n");
        return 2;
    }
#endif

    std::string err;
    auto rom = core::Rom::load(o.rom, &err);
    if (!rom) {
        fatal("could not open the ROM: " + err);
        return 1;
    }
    if (!rom->is_supported()) {
        fatal(std::string("unsupported ROM (expected game code ") + core::kExpectedGameCode + ")");
        return 1;
    }

    auto g = std::make_unique<gba::Gba>(*rom);
    if (!o.interp)
        g->set_recomp(tla_recomp_table, tla_recomp_count, tla_recomp_overlays, tla_recomp_overlay_count);
    fs::path save_path = o.rom;
    save_path.replace_extension(".sav");
    load_save(*g, save_path);

    try {
        if (!o.load_state.empty()) {
            std::vector<uint8_t> bytes;
            if (!fe::read_file(o.load_state, bytes)) throw std::runtime_error("save state not found");
            g->load_state(bytes);
        }
#ifdef _WIN32
        if (!o.headless) return fe::run_window(*g, o, save_path);
#endif
        const int rc = run_headless(*g, o);
        store_save(*g, save_path);
        return rc;
    } catch (const std::exception& e) {
        fatal(std::string(e.what()) + " (frame " + std::to_string(g->frame_count) + ")");
        if (!o.shot.empty()) png::write_rgb(o.shot, g->frame().data(), gba::kScreenW, gba::kScreenH);
        return 1;
    }
}
