// tlarecomp — recompilador estático de Avatar: The Last Airbender (GBA).
// Fase 0: valida a ROM e mostra o header. A análise e a emissão de C vêm nas próximas fases.

#include <cstdio>
#include <string>

#include "core/rom.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: tlarecomp <rom.gba>\n");
        return 2;
    }
    std::string err;
    auto rom = core::Rom::load(argv[1], &err);
    if (!rom) {
        std::fprintf(stderr, "erro: %s\n", err.c_str());
        return 1;
    }
    const auto& h = rom->header();
    std::printf("Título : %s\n", h.title.c_str());
    std::printf("Código : %s\n", h.game_code.c_str());
    std::printf("Tamanho: %zu bytes\n", rom->size());
    std::printf("Entrada: 0x%08X\n", h.entry_point);
    std::printf("SHA-1  : %s\n", rom->sha1().c_str());

    if (!rom->is_supported()) {
        std::fprintf(stderr, "erro: ROM não suportada (esperado %s / %s)\n",
                     core::kExpectedGameCode, core::kExpectedSha1);
        return 1;
    }
    std::printf("OK: ROM suportada.\n");
    return 0;
}
