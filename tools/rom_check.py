#!/usr/bin/env python3
"""Confere se a ROM fornecida é a versão suportada pelo recomp."""
import hashlib
import struct
import sys

EXPECTED_SHA1 = "9d864e9b3ccce4e5e1b1c566afa0d06088e88dfd"
EXPECTED_CODE = b"BQZE"


def main() -> int:
    if len(sys.argv) != 2:
        print("uso: rom_check.py <rom.gba>")
        return 2
    data = open(sys.argv[1], "rb").read()
    sha1 = hashlib.sha1(data).hexdigest()
    title = data[0xA0:0xAC].rstrip(b"\0").decode("ascii", "replace")
    code = data[0xAC:0xB0]
    branch = struct.unpack_from("<I", data, 0)[0]
    entry = 0x08000000 + 8 + ((branch & 0xFFFFFF) << 2)

    print(f"Título : {title}")
    print(f"Código : {code.decode('ascii', 'replace')}")
    print(f"Tamanho: {len(data)} bytes")
    print(f"Entrada: 0x{entry:08X}")
    print(f"SHA-1  : {sha1}")

    if sha1 != EXPECTED_SHA1 or code != EXPECTED_CODE:
        print("ERRO: ROM não suportada (esperado BQZE, SHA-1 " + EXPECTED_SHA1 + ")")
        return 1
    print("OK: ROM suportada.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
