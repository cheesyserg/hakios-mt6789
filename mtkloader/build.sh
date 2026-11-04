#!/bin/bash
# Build mtkloader.bin. MMU off means device memory: no SIMD, strict alignment, no builtins.
# Extra arguments go to the compiler (e.g. -DXNU_WDT_SECONDS=0, -DBOARD_PROBE=1).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# an ld.lld: zig's in xnu-deps/host/zig, else the one on the PATH
if [ -x "$ROOT/xnu-deps/host/zig/zig" ]; then LLD="$ROOT/xnu-deps/host/zig/zig ld.lld"; else LLD="ld.lld"; fi
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra"

# Headers made here and never kept in git (.gitignore):
#  touchfw.h   the Hot 20's touch controller's firmware, out of YOUR phone's hdl_firmware.img (only with
#              -DHAKIOS_X6826B=1: the Hot 60 Pro's Goodix controller keeps its own firmware)
#  font8x16.h  the loader's 8x16 text font, rasterized on a Mac by tools/mkfont8x16.swift (from a system font,
#              which is why the bitmap is not in the repository either)
case " $* " in *" -DHAKIOS_X6826B=1 "*)
	TOUCH_IMG="${TOUCH_IMG:-$ROOT/fw-touch/hdl_firmware.img}"
	if [ ! -f "$HERE/touchfw.h" ]; then
		[ -f "$TOUCH_IMG" ] || { echo "mtkloader: no $TOUCH_IMG -- run pull-firmware.sh first" >&2; exit 1; }
		python3 "$HERE/../tools/mktouchfw.py" "$TOUCH_IMG" ROMBOOT_APP_CODE "$HERE/touchfw.h"
	fi;;
esac
[ -f "$HERE/font8x16.h" ] || swift "$HERE/../tools/mkfont8x16.swift" > "$HERE/font8x16.h"

clang $CFLAGS "$@" -c "$HERE/start.S" -o "$HERE/start.o"
clang $CFLAGS "$@" -c "$HERE/mtkloader.c" -o "$HERE/mtkloader.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/start.o" "$HERE/mtkloader.o" -o "$HERE/mtkloader.bin"
ls -la "$HERE/mtkloader.bin"
