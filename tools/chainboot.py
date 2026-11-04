#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in a boot image.

  chainboot.py <android-boot.img> <program.bin> <out.img> [payload.bin]

The boot image's kernel becomes: program.bin, padded to CHAIN_OFFSET, then Android's own
(uncompressed) Image, then an optional payload (mtkloader's "XNUP"), 16-aligned. The program runs
first, moves itself to its link address if LK put it elsewhere (mtkloader/start.S), and later puts
Android's Image back where LK entered it and starts it (mtkloader.c, chain_android). The program's
own arm64 header gets the Image's length in res2 (0x28), the payload's offset in res3 (0x30) and a
covering image_size (0x10). Everything else is copied from the Android image.

Boot image headers:
  v2  the Hot 20 (X6826B): kernel, ramdisk, second, recovery dtbo, dtb, page size from the header.
  v3, v4  GKI, the Hot 60 Pro (X6885): page size 4096, kernel then ramdisk (empty when the ramdisk
      is in init_boot, as on Android 13 and later), then for v4 the boot signature. The signature
      covers the original kernel and so is dropped; LK on an unlocked phone does not check it.

The kernel is written in the form it came in (raw, gzip or lz4 legacy) unless KERNEL_FORMAT says
otherwise: LK has to unpack what it is given, and a form it already unpacked for this very phone is
the safe one. lz4 needs the lz4 command (brew install lz4).

Environment:
  BOOT_PARTITION   the boot partition's size in bytes (default: 32 MB for v2, 64 MB for v3/v4).
                   Read it off the phone: adb shell su -c 'blockdev --getsize64 /dev/block/by-name/boot_a'
  KERNEL_FORMAT    raw, gz or lz4 (default: as the input)
  KERNEL_ROOM      bytes from the kernel's load address that the unpacked kernel (with the room
                   start.S needs to move it up) must stay within: LK puts its device tree right
                   after (default for v3/v4: 0x07c80000, the X6885's tags offset from its kernel
                   address; 0 to skip the check)
"""
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys

CHAIN_OFFSET = 0x40000
RELOCATE_ROOM = 0x80000          # start.S may move the image up by this much (0x40000000 -> 0x40080000)
STUB_ROOM = 0x100000             # chain_android's stub runs just past the image
V3_PAGE = 4096
LZ4_LEGACY_MAGIC = b'\x02\x21\x4c\x18'


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def lz4_tool():
    t = shutil.which('lz4')
    if not t:
        raise SystemExit('the kernel is lz4: install the lz4 command (brew install lz4)')
    return t


def unpack_kernel(kernel):
    if kernel[:2] == b'\x1f\x8b':
        return gzip.decompress(kernel), 'gz'
    if kernel[:4] == LZ4_LEGACY_MAGIC:
        out = subprocess.run([lz4_tool(), '-dc'], input=kernel, stdout=subprocess.PIPE, check=True).stdout
        return out, 'lz4'
    return kernel, 'raw'


def pack_kernel(blob, form):
    if form == 'gz':
        return gzip.compress(blob, 9, mtime=0)
    if form == 'lz4':
        # -l: the legacy frame, which is what Linux's Image.lz4 and LK read
        return subprocess.run([lz4_tool(), '-l', '-12', '-c'], input=blob, stdout=subprocess.PIPE, check=True).stdout
    if form == 'raw':
        return blob
    raise SystemExit(f'KERNEL_FORMAT={form}: raw, gz or lz4')


def parse_v2(d):
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from('<9I', d, 8)
    rdo_size = struct.unpack_from('<I', d, 1632)[0]
    dtb_size = struct.unpack_from('<I', d, 1648)[0]

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    second = d[off:off + ssize]; off += pages(ssize)
    rdo = d[off:off + rdo_size]; off += pages(rdo_size)
    dtb = d[off:off + dtb_size]
    return dict(page=page, kernel=kernel, rest=[ramdisk, second, rdo, dtb])


def build_v2(d, img, new_kernel):
    page = img['page']
    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    sha = hashlib.sha1()
    for part in [new_kernel] + img['rest']:
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    hdr[576:608] = pad(sha.digest(), 32)
    out = bytes(hdr) + pad(new_kernel, page)
    for part in img['rest']:
        out += pad(part, page)
    return out


def parse_v3(d, hver):
    ksize, rsize, _osv, hsize = struct.unpack_from('<4I', d, 8)
    sig_size = struct.unpack_from('<I', d, 1580)[0] if hver >= 4 else 0
    assert hsize in (1580, 1584), f'header size {hsize}'

    def pages(n):
        return (n + V3_PAGE - 1) // V3_PAGE * V3_PAGE

    off = V3_PAGE
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    return dict(page=V3_PAGE, kernel=kernel, ramdisk=ramdisk, sig_size=sig_size)


def build_v3(d, img, new_kernel, hver):
    hdr = bytearray(d[:V3_PAGE])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    if hver >= 4:
        struct.pack_into('<I', hdr, 1580, 0)        # no boot signature: it was the old kernel's
    return bytes(hdr) + pad(new_kernel, V3_PAGE) + pad(img['ramdisk'], V3_PAGE)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    src, prog_path, out = sys.argv[1:4]
    payload = open(sys.argv[4], 'rb').read() if len(sys.argv) > 4 else b''
    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    hver = struct.unpack_from('<I', d, 40)[0]
    if hver == 2:
        img = parse_v2(d)
    elif hver in (3, 4):
        img = parse_v3(d, hver)
    else:
        raise SystemExit(f'boot image header v{hver}: only v2, v3 and v4 are known')

    image, form = unpack_kernel(img['kernel'])
    form = os.environ.get('KERNEL_FORMAT', form)
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_text_offset, linux_image_size = struct.unpack_from('<QQ', image, 0x08)

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    # image_size covers everything start.S moves, and leaves the stub's page beyond it free
    image_size = (total + 0xfffff) & ~0xfffff
    struct.pack_into('<Q', prog, 0x10, image_size)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = pack_kernel(blob, form)

    room = int(os.environ.get('KERNEL_ROOM', '0x07c80000' if hver >= 3 else '0'), 0)
    need = image_size + RELOCATE_ROOM + STUB_ROOM
    if room and need > room:
        raise SystemExit(f'{out}: the unpacked kernel needs {need:#x} bytes from its load address, more than the '
                         f'{room:#x} before LK\'s device tree (KERNEL_ROOM): the payload has to shrink')

    img_bytes = build_v2(d, img, new_kernel) if hver == 2 else build_v3(d, img, new_kernel, hver)
    part = int(os.environ.get('BOOT_PARTITION', str((32 if hver == 2 else 64) << 20)), 0)
    # an image larger than boot_a is refused by fastboot, and a script that sends fastboot's output
    # nowhere would carry on as if it had been flashed
    if len(img_bytes) > part:
        raise SystemExit(f'{out}: {len(img_bytes)} bytes, more than the {part} of the boot partition '
                         f'({len(img_bytes) - part} too many): the payload has to shrink (or set BOOT_PARTITION)')
    open(out, 'wb').write(img_bytes)
    print(f'{out}: {len(img_bytes)} bytes, header v{hver} (program {len(prog)}, Image {len(image)} '
          f'text_offset {linux_text_offset:#x}, payload {len(payload)}, kernel {form} {len(new_kernel)}, '
          f'unpacked {image_size:#x} of {part} / room {room:#x})')


if __name__ == '__main__':
    main()
