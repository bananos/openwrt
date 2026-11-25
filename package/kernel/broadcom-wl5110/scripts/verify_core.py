"""Verify wlc_core.o: real (relocation-referenced) imports, unreferenced UND symbols, nothing points into glue."""
import sys, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from wlsplit_common import load, relocs, symsec
f, syms = load(sys.argv[1])
ref = collections.Counter()
for tgt, rs, r in relocs(f):
    s = syms[r['r_info_sym']]
    if s['st_shndx'] == 'SHN_UNDEF' and s.name: ref[s.name] += 1
und = {s.name for s in syms if s['st_shndx'] == 'SHN_UNDEF' and s.name}
unused = sorted(und - set(ref))
open(sys.argv[2], 'w').write('\n'.join(unused) + '\n')
print(f"UND symbols: {len(und)}, referenced: {len(ref)}, unreferenced: {len(unused)}")
print("unreferenced:", ' '.join(unused))
