#pragma once
// Peças compartilhadas do frontend (tlagba): opções de linha de comando, arquivos de save.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "gba/gba.h"

namespace fe {

namespace fs = std::filesystem;

struct Press {
    uint16_t key;
    uint64_t from, to;
};

struct Options {
    fs::path rom;
    bool headless = false, mute = false, interp = false, profile = false, no_render = false;
    uint64_t frames = 600, shot_every = 0;
    bool frames_given = false;
    std::string shot, shot_prefix = "frame", wav, fallback_log, dump_ram;
    int scale = 0; // 0 = usar o tlagba.ini
    std::vector<Press> script;
    fs::path load_state;           // --load-state: carrega antes de começar
    uint64_t save_state_at = 0;    // --save-state-at N arquivo (headless)
    fs::path save_state_path;
    uint64_t load_state_at = 0;    // --load-state-at N arquivo (headless; carrega no meio de run)
    fs::path load_state_at_path;
    // AvatarRecomp.exe, para launchers como o RecompLauncher (sem elas, nada muda):
    bool game_given = false;       // --game <rom>: vai direto ao jogo, sem o launcher próprio
    fs::path data;                 // --data <pasta>: saves, states e capturas ficam nela
};

bool read_file(const fs::path& path, std::vector<uint8_t>& out);
bool write_file(const fs::path& path, const std::vector<uint8_t>& data);
void store_save(gba::Gba& g, const fs::path& path); // grava a EEPROM se mudou
fs::path exe_dir();
void load_save(gba::Gba& g, const fs::path& path); // lê a EEPROM (.sav), se existir
// Carrega a ROM e monta o GBA (com o código recompilado, a menos que interp). nullptr + err se falhar.
std::unique_ptr<gba::Gba> make_game(const fs::path& rom, bool interp, std::string& err);

#ifdef _WIN32
int run_window(gba::Gba& g, const Options& o, const fs::path& save_path); // tlagba: só o jogo
int run_app(const Options& o);                                            // AvatarRecomp: launcher + jogo
#endif

} // namespace fe
