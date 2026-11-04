#!/bin/bash
# The first run on the Hot 60 Pro (X6885): mtkloader built with -DBOARD_PROBE=1 in front of the stock
# boot_a, no XNU. On the next boot it reads LK's device tree, the display's registers and the UFS host,
# looks at each UFS LU (READ CAPACITY, then a READ of its GPT: reads only), shows it all on the screen
# for 60 s and writes it into ramoops, then starts Android. This script then fetches the log and
# puts the stock boot_a back (KEEP=1 leaves the probe image in place).
#
#   HAKIOS_SERIAL=... tools/x6885-probe.sh           -> logs/x6885-probe.log
#
# Needs: backup/boot_a.img (the phone's own boot_a: adb shell su -c 'dd if=/dev/block/by-name/boot_a' >
# backup/boot_a.img), an unlocked bootloader, root for reading pstore. Writes boot_a only, slot a only.
# If the phone's tree has no ramoops, the log is only on the screen: take a photograph of it.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STOCK="$ROOT/backup/boot_a.img"
OUT="$ROOT/logs/x6885-probe.log"
[ -f "$STOCK" ] || { echo "no $STOCK: save the phone's own boot_a there first"; exit 1; }
mkdir -p "$ROOT/logs"
"$ROOT/mtkloader/build.sh" -DBOARD_PROBE=1 "$@" >/dev/null
NO_PAYLOAD=1 python3 "$ROOT/mtkloader/build_flashed.py" "$STOCK"

flash() {
	[ "$(adb -s $S get-state 2>/dev/null)" = device ] && adb -s $S reboot bootloader
	for i in $(seq 1 120); do fastboot devices | grep -q $S && break; sleep 1; done
	fastboot devices | grep -q $S || { echo "the phone did not come up in fastboot"; exit 1; }
	[ "$(fastboot -s $S getvar current-slot 2>&1 | sed -n 's/^current-slot: //p')" = a ] || { echo "current slot is not a"; exit 1; }
	fastboot -s $S flash boot_a "$1"
	fastboot -s $S reboot
}

flash "$ROOT/xnu-flashed-slot_a.img"
echo "the probe runs on this boot: 60 s on the screen, then Android"
for i in $(seq 1 120); do [ "$(adb -s $S get-state 2>/dev/null)" = device ] && break; sleep 3; done
sleep 5
adb -s $S shell 'su -c "cat /sys/fs/pstore/console-ramoops-0"' > "$OUT" 2>/dev/null || true
if grep -q 'board probe' "$OUT"; then
	echo "log: $OUT ($(wc -l < "$OUT") lines)"
else
	echo "no probe in pstore (no ramoops in LK's tree, or it is elsewhere): the screen's photograph is the log"
fi
if [ -z "$KEEP" ]; then
	flash "$STOCK"
	echo "stock boot_a back"
fi
