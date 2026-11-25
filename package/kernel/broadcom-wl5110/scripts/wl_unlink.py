#!/usr/bin/env python3
"""
wl_unlink.py - cut the closed 2.6.22 Linux glue (wl_linux.c) out of stock wl.ko 5.110.27.20012,
leaving the wlc/phy core as a plain relocatable object (stage 1 output, before objcopy cleanup).

What it does, in place on a copy of the input:
  1. every GLOBAL function defined inside the glue ranges becomes an UNDEFINED symbol, so the core's
     calls (wl_up, wl_sendup, wl_add_timer, ...) bind to the new 4.14 glue at link time;
  2. all relocations located inside the glue code are dropped, so the dead glue imports nothing;
  3. relocations inside glue-only data objects (wl_pci_driver, wl_ethtool_ops) are dropped;
  4. glue code bytes are overwritten with MIPS `break 0x3ff` so any stray jump into them traps loudly.
Section removal and symbol localisation are done afterwards with objcopy (see build_core.sh).

Usage: wl_unlink.py in.ko out.o
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wlsplit_common import GLUE_RANGES, in_glue, load, symsec

GLUE_DATA_OBJECTS = {'wl_pci_driver', 'wl_ethtool_ops'}
BREAK = struct.pack('<I', 0x0000000d | (0x3ff << 16))   # break 0x3ff (code1 is a 10-bit field)

def main(src, dst):
    f, syms = load(src)
    buf = bytearray(open(src, 'rb').read())
    secs = list(f.iter_sections())
    symtab = f.get_section_by_name('.symtab')
    sym_off, sym_ent = symtab['sh_offset'], symtab['sh_entsize']

    # glue-only data object ranges: (section, start, end)
    glue_data = [(symsec(f, s), s['st_value'], s['st_value'] + s['st_size'])
                 for s in syms if s.name in GLUE_DATA_OBJECTS]

    # 1. undefine glue globals
    undefined = []
    for idx, s in enumerate(syms):
        ss = symsec(f, s)
        if (ss in ('.text', '.text.fastpath') and in_glue(ss, s['st_value'])
                and s['st_info']['bind'] == 'STB_GLOBAL' and s['st_info']['type'] == 'STT_FUNC'):
            o = sym_off + idx * sym_ent
            # Elf32_Sym: name(4) value(4) size(4) info(1) other(1) shndx(2)
            struct.pack_into('<IIBBH', buf, o + 4, 0, 0, (1 << 4) | 0, 0, 0)  # GLOBAL, NOTYPE, UNDEF
            undefined.append(s.name)

    # 2+3. filter relocation sections
    dropped = {}
    for s in secs:
        if s['sh_type'] != 'SHT_REL':
            continue
        tgt = secs[s['sh_info']].name
        ent = s['sh_entsize']
        base, n = s['sh_offset'], s['sh_size'] // ent
        keep = []
        for i in range(n):
            r_off, r_info = struct.unpack_from('<II', buf, base + i * ent)
            if in_glue(tgt, r_off):
                continue
            if any(sec == tgt and a <= r_off < b for sec, a, b in glue_data):
                continue
            keep.append((r_off, r_info))
        if len(keep) != n:
            dropped[s.name] = n - len(keep)
            for i, (a, b) in enumerate(keep):
                struct.pack_into('<II', buf, base + i * ent, a, b)
            for i in range(len(keep), n):           # zero the tail
                struct.pack_into('<II', buf, base + i * ent, 0, 0)
            # sh_size lives at offset 20 in Elf32_Shdr
            hdr_off = f['e_shoff'] + secs.index(s) * f['e_shentsize']
            struct.pack_into('<I', buf, hdr_off + 20, len(keep) * ent)

    # 4. poison glue code
    for name in ('.text', '.text.fastpath'):
        s = f.get_section_by_name(name)
        a, b = GLUE_RANGES[name]
        buf[s['sh_offset'] + a: s['sh_offset'] + b] = BREAK * ((b - a) // 4)

    open(dst, 'wb').write(buf)
    print(f"undefined {len(undefined)} glue globals: {' '.join(sorted(undefined))}")
    print("dropped relocations:", dropped)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
