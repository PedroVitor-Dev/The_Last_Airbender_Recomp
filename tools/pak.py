#!/usr/bin/env python3
"""Ferramenta para o arquivo de dados HBPak (Halfbrick) de Avatar: The Last Airbender (GBA).

Formato (little-endian), descrito em docs/PAK_FORMAT.md:
    cabeçalho (header_size = 0x80 bytes)
        0x00  char[16]  "HBPakFileV10\\r\\n\\x1a\\n"
        0x10  u32       tamanho total do pak (cabeçalho + tabela + dados)
        0x14  u16       header_size
        0x16  u16       número de entradas
        0x18  char[104] caminho de build ("D:\\Build\\Avatar\\data")
    entradas (0x84 bytes cada)
        0x00  char[120] nome (separador '\\')
        0x78  u32       hash do nome (algoritmo ainda desconhecido)
        0x7C  u32       tamanho
        0x80  u32       offset relativo ao início do pak
    dados: arquivos contíguos, sem alinhamento, na ordem dos offsets.

Uso:
    pak.py list    <rom>
    pak.py extract <rom> [-o assets/extracted] [--no-previews]
    pak.py build   <dir> <saida.pak>
    pak.py verify  <rom>        extrai + remonta em memória e compara com a ROM
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

MAGIC = b"HBPakFileV10\r\n\x1a\n"
ENTRY_SIZE = 0x84
NAME_SIZE = 120
SCAN_START = 0x200000  # o jogo procura a assinatura a partir de 0x08200000, de 4 em 4 bytes
SEP = "\\"


class Pak:
    def __init__(self, data: bytes, base: int):
        self.data = data
        self.base = base
        if data[base:base + 16] != MAGIC:
            raise ValueError("assinatura HBPak não encontrada em 0x%X" % base)
        self.total, self.header_size, count = struct.unpack_from("<IHH", data, base + 0x10)
        self.build_path = data[base + 0x18:base + self.header_size].split(b"\0")[0].decode("ascii")
        self.entries = []
        for i in range(count):
            p = base + self.header_size + i * ENTRY_SIZE
            name = data[p:p + NAME_SIZE].split(b"\0")[0].decode("ascii")
            h, size, off = struct.unpack_from("<III", data, p + NAME_SIZE)
            self.entries.append({"name": name, "hash": h, "size": size, "offset": off})

    def file_bytes(self, e) -> bytes:
        start = self.base + e["offset"]
        return self.data[start:start + e["size"]]

    def raw(self) -> bytes:
        return self.data[self.base:self.base + self.total]


def find_pak(rom: bytes) -> int:
    for off in range(SCAN_START, len(rom) - 16, 4):
        if rom[off:off + 16] == MAGIC:
            return off
    raise ValueError("HBPak não encontrado na ROM")


def load_rom(path: str) -> Pak:
    rom = open(path, "rb").read()
    return Pak(rom, find_pak(rom))


def safe_relpath(name: str) -> str:
    parts = [p for p in name.split(SEP) if p not in ("", ".", "..")]
    if not parts:
        raise ValueError("nome de arquivo inválido: %r" % name)
    return os.path.join(*parts)


# ─────────────────────────────────────────────── prévias ──

def bgr555_to_rgb(c: int):
    r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


def write_png(path: str, width: int, height: int, rgb_rows):
    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload))
    raw = b"".join(b"\0" + bytes(row) for row in rgb_rows)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def palette_preview(data: bytes, path: str, cell: int = 12) -> bool:
    """.pal = cabeçalho de 4 bytes + N paletas de 16 cores BGR555. Gera uma grade 16×N."""
    body = data[4:]
    if len(body) < 32 or len(body) % 32:
        return False
    colors = [bgr555_to_rgb(c) for c in struct.unpack("<%dH" % (len(body) // 2), body)]
    rows = []
    for pal in range(len(colors) // 16):
        line = []
        for i in range(16):
            line.extend(colors[pal * 16 + i] * cell)
        rows.extend([line] * cell)
    write_png(path, 16 * cell, len(rows), rows)
    return True


# ───────────────────────────────────────────── comandos ──

def cmd_list(args):
    pak = load_rom(args.rom)
    print("HBPak em 0x%08X, %d bytes, %d arquivos (%s)" %
          (0x08000000 + pak.base, pak.total, len(pak.entries), pak.build_path))
    for e in pak.entries:
        print("%08X  %7d  %08X  %s" % (e["offset"], e["size"], e["hash"], e["name"]))


def cmd_extract(args):
    pak = load_rom(args.rom)
    out = args.output
    files_dir = os.path.join(out, "files")
    os.makedirs(files_dir, exist_ok=True)
    manifest = {
        "format": "HBPakFileV10",
        "rom_offset": pak.base,
        "header_size": pak.header_size,
        "build_path": pak.build_path,
        # Ordem da tabela ≠ ordem dos dados; guardamos ambas para remontar byte a byte.
        "entries": [],
        "data_order": [e["name"] for e in sorted(pak.entries, key=lambda e: e["offset"])],
    }
    previews = 0
    for e in pak.entries:
        blob = pak.file_bytes(e)
        rel = safe_relpath(e["name"])
        dst = os.path.join(files_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(blob)
        manifest["entries"].append({"name": e["name"], "hash": "0x%08X" % e["hash"], "size": e["size"],
                                    "sha1": hashlib.sha1(blob).hexdigest()})
        if not args.no_previews and e["name"].endswith(".pal"):
            pdst = os.path.join(out, "previews", rel + ".png")
            os.makedirs(os.path.dirname(pdst), exist_ok=True)
            previews += palette_preview(blob, pdst)
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print("%d arquivos extraídos em %s (%d prévias de paleta)" % (len(pak.entries), out, previews))


def build_pak(manifest: dict, read_file) -> bytes:
    entries = manifest["entries"]
    header_size = manifest["header_size"]
    blobs = {e["name"]: read_file(e["name"]) for e in entries}
    order = manifest.get("data_order") or [e["name"] for e in entries]
    offsets, cursor = {}, header_size + len(entries) * ENTRY_SIZE
    for name in order:
        offsets[name] = cursor
        cursor += len(blobs[name])
    out = bytearray()
    out += MAGIC
    out += struct.pack("<IHH", cursor, header_size, len(entries))
    out += manifest["build_path"].encode("ascii").ljust(header_size - 0x18, b"\0")
    for e in entries:
        name = e["name"].encode("ascii")
        if len(name) >= NAME_SIZE:
            raise ValueError("nome longo demais: %s" % e["name"])
        out += name.ljust(NAME_SIZE, b"\0")
        out += struct.pack("<III", int(e["hash"], 16), len(blobs[e["name"]]), offsets[e["name"]])
    for name in order:
        out += blobs[name]
    return bytes(out)


def cmd_build(args):
    with open(os.path.join(args.dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    files_dir = os.path.join(args.dir, "files")

    def read_file(name):
        with open(os.path.join(files_dir, safe_relpath(name)), "rb") as fh:
            return fh.read()

    blob = build_pak(manifest, read_file)
    with open(args.output, "wb") as f:
        f.write(blob)
    print("pak gerado: %s (%d bytes, %d arquivos)" % (args.output, len(blob), len(manifest["entries"])))


def cmd_verify(args):
    pak = load_rom(args.rom)
    manifest = {
        "header_size": pak.header_size,
        "build_path": pak.build_path,
        "entries": [{"name": e["name"], "hash": "0x%08X" % e["hash"]} for e in pak.entries],
        "data_order": [e["name"] for e in sorted(pak.entries, key=lambda e: e["offset"])],
    }
    by_name = {e["name"]: e for e in pak.entries}
    rebuilt = build_pak(manifest, lambda n: pak.file_bytes(by_name[n]))
    original = pak.raw()
    if rebuilt == original:
        print("OK: pak remontado é idêntico ao original (%d bytes, %d arquivos)" % (len(rebuilt), len(pak.entries)))
        return 0
    diff = next((i for i, (a, b) in enumerate(zip(rebuilt, original)) if a != b), min(len(rebuilt), len(original)))
    print("ERRO: diferença no byte 0x%X (tamanhos %d vs %d)" % (diff, len(rebuilt), len(original)))
    return 1


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("rom"); p.set_defaults(fn=cmd_list)
    p = sub.add_parser("extract"); p.add_argument("rom")
    p.add_argument("-o", "--output", default=os.path.join("assets", "extracted"))
    p.add_argument("--no-previews", action="store_true"); p.set_defaults(fn=cmd_extract)
    p = sub.add_parser("build"); p.add_argument("dir"); p.add_argument("output"); p.set_defaults(fn=cmd_build)
    p = sub.add_parser("verify"); p.add_argument("rom"); p.set_defaults(fn=cmd_verify)
    args = ap.parse_args()
    return args.fn(args) or 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:  # saída cortada por "| head"
        sys.exit(0)
