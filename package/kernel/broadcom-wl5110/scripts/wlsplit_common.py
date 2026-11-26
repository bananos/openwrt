# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Andrew Druchenko
"""Shared definitions: where the closed wl_linux.c glue lives inside stock wl.ko 5.110.27.20012."""
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

# (section name, start, end) of the glue object's code, from layout.txt
GLUE_RANGES = {
    '.text': (0x0c4bf0, 0x0c8150),          # wl_ifname .. wl_ioctl (end = start of wlc_rminprog)
    '.text.fastpath': (0x00087c, 0x001558),  # wl_sendup .. wl_start_txqwork
    '.init.text': (0, 1 << 32),
    '.exit.text': (0, 1 << 32),
}

def in_glue(secname, off):
    r = GLUE_RANGES.get(secname)
    return bool(r) and r[0] <= off < r[1]

def load(path):
    f = ELFFile(open(path, 'rb'))
    syms = list(f.get_section_by_name('.symtab').iter_symbols())
    return f, syms

def symsec(f, sym):
    sh = sym['st_shndx']
    return f.get_section(sh).name if isinstance(sh, int) and sh != 0 else None

def relocs(f):
    """Yield (target_section_name, reloc_section, entry)."""
    for s in f.iter_sections():
        if isinstance(s, RelocationSection):
            tgt = f.get_section(s['sh_info']).name
            for r in s.iter_relocations():
                yield tgt, s, r
