#!/usr/bin/env python3
"""Find a Broadcom TRX (HDR0) image in a file and write out its root filesystem partition.

usage: trx_rootfs.py <image> <rootfs-out>

<image> may be a bare .trx, an mtd "linux" partition dump or a whole flash dump: the first HDR0
header on a 4 KB boundary whose third partition offset points at a SquashFS magic (any byte order) is used.
"""
import struct
import sys


def main(src, dst):
    data = open(src, 'rb').read()
    for off in range(0, len(data) - 28, 0x1000):
        if data[off:off + 4] != b'HDR0':
            continue
        length, _crc, _flags, _p0, _p1, p2 = struct.unpack_from('<IIIIII', data, off + 4)
        if not 28 <= p2 < length or off + length > len(data):
            continue
        if data[off + p2:off + p2 + 4] not in (b'hsqs', b'sqsh', b'shsq', b'qshs'):
            continue
        open(dst, 'wb').write(data[off + p2:off + length])
        print('TRX at 0x%x, %d bytes, rootfs at +0x%x' % (off, length, p2))
        return 0
    sys.stderr.write('no TRX image with a SquashFS root filesystem in %s\n' % src)
    return 1


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
