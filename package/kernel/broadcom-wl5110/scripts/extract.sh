#!/bin/sh
# Extract the Broadcom 5.110.27.20012 wl core and userspace from an ASUS RT-N12 D1 firmware image,
# then turn the stock wl.ko into wlc_core.o (glue unlinked, see wl_unlink.py).
#
#   extract.sh <firmware> <outdir> <sasquatch> <objcopy>
#
# <firmware>: the ASUS download (FW_RT_N12_D1_*.zip), a bare .trx, an mtd1 ("linux") dump,
#             or a full 8 MB flash dump. The TRX is located by its HDR0 magic.
# Only files whose SHA-256 is listed below are accepted, so a different firmware build fails here
# instead of producing a module that crashes at runtime.
set -eu

FW=$1; OUT=$2; SQ=$3; OBJCOPY=$4
HERE=$(cd "$(dirname "$0")" && pwd)
die() { echo "broadcom-wl5110: $*" >&2; exit 1; }

# --- accepted inputs -------------------------------------------------------------------------
# wl.ko 5.110.27.20012 builds with byte-identical .text (only TX-power table bytes and the build
# date differ in .rodata):
#   7aae04ae... ASUS 3.0.0.4.374.813 (FW_RT_N12_D1_3004374813.zip, official download)
#   87b97bac... ASUS 3.0.0.4.374_168 (factory firmware of some units; from a flash dump)
WLKO_OK="7aae04ae6175d02ba097cc124e34d0fe5651dd4770c83c5f3b38e6f382cd6e27
87b97bacd3b2cb7d2d2eae8e0f4ab76d518a6af055033f2e7778aaefc21b11e6"
# .text of the generated wlc_core.o: the core code the glue was written and tested against
CORE_TEXT=d9064faa69869d2841c2697ecfdabd11ed02a512c9b6bea304cb18a747be49b2
# stock userspace (identical in both builds)
USR_OK="3e495c34e4f8e9fd7443fd0ee3ecb2029f39e9ed328dbd9ee0c64dae4b6f75a3  usr/sbin/wl
2b9c30954706b2df0453d58b8432796d1457b2efe1fc4713e852d1a48f2274d5  lib/ld-uClibc.so.0
d706924513d835f351cfe2167d66069c45f1a0657b7fac4ac424ce471a30486b  lib/libc.so.0
1bfbf61fe8274e84e7e82d277b00f696f8856933ef3ea0ab4896d9180ca37b1d  lib/libgcc_s.so.1"

python3 -B -I -c 'import elftools' 2>/dev/null || \
	die "host python3 module 'elftools' is missing: install python3-pyelftools (apt) or 'pip install pyelftools'"

rm -rf "$OUT"; mkdir -p "$OUT"
W=$OUT/.work; mkdir -p "$W"

# --- 1. get the TRX --------------------------------------------------------------------------
case "$FW" in
*.zip)	unzip -p "$FW" '*.trx' > "$W/fw.trx" || die "no .trx in $FW" ;;
*)	cp "$FW" "$W/fw.trx" ;;
esac
python3 -B -I "$HERE/trx_rootfs.py" "$W/fw.trx" "$W/rootfs.sqsh" || die "no usable TRX image in $FW"

# --- 2. unpack the needed files from the LZMA SquashFS ---------------------------------------
WLKO=lib/modules/2.6.22.19/kernel/drivers/net/wl.ko
"$SQ" -no-progress -d "$W/root" "$W/rootfs.sqsh" \
	$WLKO usr/sbin/wl lib/ld-uClibc.so.0 lib/libc.so.0 lib/libgcc_s.so.1 >/dev/null 2>&1 || true
[ -f "$W/root/$WLKO" ] || die "wl.ko not found in the firmware's root filesystem"

h=$(sha256sum "$W/root/$WLKO" | cut -d' ' -f1)
echo "$WLKO_OK" | grep -qx "$h" || die "unsupported wl.ko (sha256 $h); supported: ASUS 3.0.0.4.374.813 / 374_168"

# --- 3. stock userspace for the brcmwl-utils package -----------------------------------------
(cd "$W/root" && echo "$USR_OK" | sha256sum -c --quiet -) || die "stock wl tool or uClibc libraries don't match the supported build"
mkdir -p "$OUT/usr"
for f in usr/sbin/wl lib/ld-uClibc.so.0 lib/libc.so.0 lib/libgcc_s.so.1; do
	cp "$W/root/$f" "$OUT/usr/$(basename $f)"
done

# --- 4. wl.ko -> wlc_core.o (same steps as wl-port/scripts/build_core.sh) --------------------
python3 -B -I "$HERE/wl_unlink.py" "$W/root/$WLKO" "$W/core.stage1"
RM_SECS=".modinfo __param .gnu.linkonce.this_module .init.text .exit.text .pdr"
STRIP=$(python3 -B -I - "$W/core.stage1" <<'PY'
import sys
from elftools.elf.elffile import ELFFile
f = ELFFile(open(sys.argv[1], 'rb'))
rm = {'.modinfo', '__param', '.gnu.linkonce.this_module', '.init.text', '.exit.text', '.pdr'}
for s in f.get_section_by_name('.symtab').iter_symbols():
    sh = s['st_shndx']
    if isinstance(sh, int) and sh and f.get_section(sh).name in rm and s.name and s['st_info']['type'] != 'STT_SECTION':
        print(s.name)
PY
)
LOCALIZE="wl_found instance_base nompc oneonly passivemode piomode wl_txq_thresh name wl_pci_driver wl_ethtool_ops"
set --
for s in $RM_SECS; do set -- "$@" --remove-section=$s --remove-section=.rel$s; done
for s in $STRIP; do set -- "$@" --strip-symbol=$s; done
for s in $LOCALIZE; do set -- "$@" --localize-symbol=$s; done
"$OBJCOPY" "$@" "$W/core.stage1" "$W/core.o"
python3 -B -I "$HERE/verify_core.py" "$W/core.o" "$W/core.unref" >/dev/null
set --; for s in $(cat "$W/core.unref"); do set -- "$@" --strip-symbol=$s; done
[ $# -gt 0 ] && "$OBJCOPY" "$@" "$W/core.o" "$W/core.tmp" && mv "$W/core.tmp" "$W/core.o"
python3 -B -I "$HERE/verify_core.py" "$W/core.o" "$W/core.unref" >/dev/null
grep -q "[^[:space:]]" "$W/core.unref" && die "wlc_core.o still has unreferenced imports: $(cat "$W/core.unref")"

"$OBJCOPY" -O binary -j .text "$W/core.o" "$W/core.text"
h=$(sha256sum "$W/core.text" | cut -d' ' -f1)
[ "$h" = "$CORE_TEXT" ] || die "wlc_core.o .text mismatch ($h): unlink step produced unexpected code"

cp "$W/core.o" "$OUT/wlc_core.o_shipped"
rm -rf "$W"
echo "broadcom-wl5110: core extracted and verified"
