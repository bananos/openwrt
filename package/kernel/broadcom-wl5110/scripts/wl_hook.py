#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Andrew Druchenko
"""Interpose functions of the wl core (a relocatable ELF32 little-endian MIPS object).

For every name given, the core's own definition is renamed to __real_<name> and every relocation
that referred to it now refers to a new undefined symbol <name>, which the glue defines (a logging
wrapper that calls __real_<name>). Used only by the optional register-capture build.

    wl_hook.py in.o out.o name...
"""
import struct
import sys


def main():
    src, dst, names = sys.argv[1], sys.argv[2], sys.argv[3:]
    b = bytearray(open(src, 'rb').read())
    assert b[:4] == b'\x7fELF' and b[4] == 1 and b[5] == 1, 'ELF32 little-endian only'
    shoff, = struct.unpack_from('<I', b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', b, 0x2e)
    shdrs = [list(struct.unpack_from('<10I', b, shoff + i * shentsize)) for i in range(shnum)]
    # sh: name type flags addr offset size link info addralign entsize
    symi = next(i for i, s in enumerate(shdrs) if s[1] == 2)            # SHT_SYMTAB
    sym, stri = shdrs[symi], shdrs[symi][6]
    strtab = bytearray(b[shdrs[stri][4]:shdrs[stri][4] + shdrs[stri][5]])
    symtab = bytearray(b[sym[4]:sym[4] + sym[5]])
    nsyms = len(symtab) // 16

    def name_of(off):
        return strtab[off:strtab.index(b'\0', off)].decode()

    index = {}
    for i in range(nsyms):
        st_name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', symtab, i * 16)
        if (info >> 4) == 1 and shndx != 0:                             # GLOBAL, defined
            index[name_of(st_name)] = i
    remap = {}
    for n in names:
        if n not in index:
            sys.exit('wl_hook: %s is not a defined global symbol' % n)
        i = index[n]
        old_name, = struct.unpack_from('<I', symtab, i * 16)
        struct.pack_into('<I', symtab, i * 16, len(strtab))
        strtab += ('__real_' + n).encode() + b'\0'
        new = nsyms + len(remap)
        # the wrapper: GLOBAL NOTYPE, undefined, keeps the original name string
        symtab += struct.pack('<IIIBBH', old_name, 0, 0, 0x10, 0, 0)
        remap[i] = new

    count = dict.fromkeys(names, 0)
    local = dict.fromkeys(names, 0)
    rev = {v: k for k, v in index.items()}
    # section symbols, and the hooked functions by (section, offset): many calls in the core are
    # relocated against the section symbol with the target offset in the instruction/data word
    secsym = {}
    for i in range(nsyms):
        _, value, _, info, _, shndx = struct.unpack_from('<IIIBBH', symtab, i * 16)
        if (info & 0xf) == 3:                                           # STT_SECTION
            secsym[i] = shndx
    at = {}
    for n in names:
        _, value, _, _, _, shndx = struct.unpack_from('<IIIBBH', symtab, index[n] * 16)
        at[(shndx, value)] = n
    for s in shdrs:
        if s[1] not in (9, 4) or s[6] != symi:                          # SHT_REL / SHT_RELA
            continue
        assert s[1] == 9, 'SHT_RELA not handled'
        target = shdrs[s[7]][4]                                         # data of the relocated section
        for off in range(s[4], s[4] + s[5], 8):
            r_offset, r_info = struct.unpack_from('<II', b, off)
            si, rtype = r_info >> 8, r_info & 0xff
            if si in remap:
                struct.pack_into('<I', b, off + 4, (remap[si] << 8) | rtype)
                count[rev[si]] += 1
            elif si in secsym:
                w, = struct.unpack_from('<I', b, target + r_offset)
                if rtype == 4:                                          # R_MIPS_26: jal
                    addend, keep = (w & 0x3ffffff) << 2, w & ~0x3ffffff
                elif rtype == 2:                                        # R_MIPS_32: pointer
                    addend, keep = w, 0
                else:
                    continue
                n = at.get((secsym[si], addend))
                if n:
                    struct.pack_into('<I', b, target + r_offset, keep)
                    struct.pack_into('<I', b, off + 4, (remap[index[n]] << 8) | rtype)
                    local[n] += 1

    # new symtab and strtab go at the end of the file; their old copies are left unused
    def append(data, sh):
        while len(b) % 4:
            b.append(0)
        sh[4], sh[5] = len(b), len(data)
        b.extend(data)
    append(symtab, shdrs[symi])
    append(strtab, shdrs[stri])
    for i, s in enumerate(shdrs):
        struct.pack_into('<10I', b, shoff + i * shentsize, *s)
    open(dst, 'wb').write(b)
    for n in names:
        print('wl_hook: %-28s %5d by symbol, %5d by section offset' % (n, count[n], local[n]))


if __name__ == '__main__':
    main()
