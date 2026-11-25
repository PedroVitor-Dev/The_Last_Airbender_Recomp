#!/usr/bin/env python3
"""Teste de mutação dos verificadores: injeta um bug conhecido em recomp_rt.h.

Uso: mutate.py <raiz do projeto> <M1|M2|M3|list|revert>

Cada mutação é um erro sutil de semântica que os testes diferenciais (test_synth,
tlaverify) PRECISAM detectar. "revert" restaura o original e atualiza a data do arquivo
para o build recompilar tudo (senão o Ninja manteria os objetos mutados).
"""
import os
import shutil
import sys

MUTATIONS = {
    'M1': ('SBC com flags ignora o carry de entrada',
           'inline uint32_t sbcs(Cpu& c, uint32_t a, uint32_t b) { return add_wc(c, a, ~b, c.c_); }',
           'inline uint32_t sbcs(Cpu& c, uint32_t a, uint32_t b) { return add_wc(c, a, ~b, true); }'),
    'M2': ('LSR por registrador de 32 perde o carry de saída',
           '    carry = amt == 32 ? (v >> 31) != 0 : false;\n    return 0;\n}\ninline uint32_t asr_r',
           '    carry = false;\n    return 0;\n}\ninline uint32_t asr_r'),
    'M3': ('LDRH desalinhado não rotaciona',
           'inline uint32_t ldrh(Gba& g, uint32_t a) { return std::rotr(rd16(g, a & ~1u), static_cast<int>((a & 1) * 8)); }',
           'inline uint32_t ldrh(Gba& g, uint32_t a) { return rd16(g, a & ~1u); }'),
}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    root, which = sys.argv[1], sys.argv[2]
    path = os.path.join(root, 'runtime', 'include', 'gba', 'recomp_rt.h')
    backup = path + '.orig'

    if which == 'list':
        for k, (desc, _, _) in MUTATIONS.items():
            print(k, '-', desc)
        return 0
    if which == 'revert':
        if os.path.exists(backup):
            shutil.copyfile(backup, path)
            os.remove(backup)
        os.utime(path, None)  # data atual: força o rebuild
        print('revertido')
        return 0

    if not os.path.exists(backup):
        shutil.copyfile(path, backup)
    src = open(backup, encoding='utf-8').read()
    desc, a, b = MUTATIONS[which]
    if src.count(a) != 1:
        print('mutação', which, 'não se aplica mais (o código mudou)')
        return 1
    open(path, 'w', encoding='utf-8').write(src.replace(a, b))
    print('mutação', which, 'aplicada:', desc)
    return 0


if __name__ == '__main__':
    sys.exit(main())
