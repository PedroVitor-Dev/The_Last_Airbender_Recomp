#pragma once
// Gera C++ a partir das funções descobertas pela análise.
//
// Cada função ARM/Thumb vira `void f_XXXXXXXX(gba::Gba& g)`. Registradores e flags
// ficam no estado da CPU (g.cpu), então interrupções podem ser atendidas em qualquer
// ponto de sincronização (início de bloco). Ver docs/ARCHITECTURE.md.

#include <filesystem>
#include <string>

#include "analysis.h"

namespace recomp {

struct EmitStats {
    size_t functions = 0;
    size_t instructions = 0;
    size_t files = 0;
    size_t unimplemented = 0;
};

EmitStats emit_cpp(const Analyzer& a, const core::Rom& rom, const std::filesystem::path& out_dir,
                   size_t functions_per_file = 120);

} // namespace recomp
