#!/bin/bash
# What the Hot 60 Pro (X6885) says of itself under Android, before anything is flashed: read only.
# Everything the port takes from a static device tree is checked here against the phone's own:
# the memory map and its reservations (where XNU's 4 GB and the loader's scratch go), ramoops (where
# the log comes back), boot_a's header and size (tools/chainboot.py), which UFS LU holds the GPT,
# the touch controller, the panel, the firmware's names.
#
#   HAKIOS_SERIAL=... tools/x6885-facts.sh        -> logs/x6885-facts/
#
# Needs root on the phone (Magisk's su). Nothing on the phone is changed. The output stays on this
# machine (logs/ and *.dtb are ignored by git): it is the vendor's and the phone's, not for the tree.
# The serial number is taken out of what is written.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/logs/x6885-facts"
mkdir -p "$OUT"
sh() { adb -s "$S" shell "su -c '$1'" 2>&1 | tr -d '\r' | sed -e "s/$S/SERIAL/g" -e 's/serialno=[^ ]*/serialno=SERIAL/g'; }

{
	echo "== props"
	for p in ro.product.model ro.product.device ro.boot.hardware ro.build.version.release ro.boot.slot_suffix \
		ro.boot.flash.locked ro.boot.verifiedbootstate ro.board.platform; do
		echo "$p=$(adb -s "$S" shell getprop $p | tr -d '\r')"
	done
	echo "== boot partitions (size in bytes; header magic and version of boot_a)"
	for p in boot_a init_boot_a vendor_boot_a vbmeta_a userdata; do
		echo "$p $(sh "blockdev --getsize64 /dev/block/by-name/$p 2>/dev/null")"
	done
	sh "dd if=/dev/block/by-name/boot_a bs=48 count=1 2>/dev/null | od -A d -t x4"
	echo "== by-name -> block devices (the UFS LUs are sda, sdb, sdc...)"
	sh "ls -l /dev/block/by-name/ | grep -E \"boot_a|init_boot_a|userdata|preloader|xnusys\""
	sh "for d in /sys/class/block/sd?; do echo \$d \$(cat \$d/size) \$(cat \$d/device/scsi_level 2>/dev/null); done"
	sh "ls /sys/bus/scsi/devices/"
	echo "== memory map"
	sh "cat /proc/iomem"
	echo "== reserved memory nodes (name: reg)"
	sh "cd /proc/device-tree/reserved-memory && for n in *; do [ -f \$n/reg ] && echo \$n \$(od -A n -t x1 \$n/reg | tr -d \"\\n\"); done"
	echo "== ramoops / pstore"
	sh "ls -la /sys/fs/pstore/; ls /proc/device-tree/reserved-memory/ | grep -i -E \"ramoops|pstore\""
	sh "for f in /proc/device-tree/reserved-memory/*ramoops*/*; do echo \$f \$(od -A n -t x1 \$f | tr -d \"\\n\" | cut -c1-80); done"
	sh "cat /sys/module/ramoops/parameters/* 2>/dev/null"
	echo "== cmdline"
	sh "cat /proc/cmdline"
	echo "== panel and touch"
	sh "dmesg | grep -i -E \"lcm|panel|nt37706|vtdr6126|goodix|gt99|brl|touch\" | head -60"
	sh "for d in /sys/bus/spi/devices/*; do echo \$d \$(cat \$d/modalias 2>/dev/null) \$(readlink \$d/driver); done"
	sh "for d in /sys/bus/i2c/devices/*; do echo \$d \$(cat \$d/name 2>/dev/null) \$(readlink \$d/driver); done"
	echo "== UFS"
	sh "dmesg | grep -i -E \"ufs\" | head -40"
	echo "== firmware names (names only)"
	sh "ls /vendor/firmware/ /vendor/firmware/*/ 2>/dev/null | head -200"
	echo "== GPU"
	sh "dmesg | grep -i -E \"mali|gpu\" | head -20"
} > "$OUT/facts.txt"

# the live device tree (LK's, as Android got it) and the GPT of the LU holding userdata: for this machine only
sh "cat /sys/firmware/fdt > /data/local/tmp/x6885.dtb; chmod 644 /data/local/tmp/x6885.dtb"
adb -s "$S" pull /data/local/tmp/x6885.dtb "$OUT/x6885-live.dtb" >/dev/null && sh "rm -f /data/local/tmp/x6885.dtb"
UD=$(sh "readlink -f /dev/block/by-name/userdata" | sed 's/[0-9]*$//')
echo "userdata is on $UD" >> "$OUT/facts.txt"
sh "dd if=$UD of=/data/local/tmp/gpt-primary.bin bs=4096 count=6 2>/dev/null; chmod 644 /data/local/tmp/gpt-primary.bin"
adb -s "$S" pull /data/local/tmp/gpt-primary.bin "$OUT/gpt-primary.bin" >/dev/null && sh "rm -f /data/local/tmp/gpt-primary.bin"
ls -l "$OUT"
echo "facts: $OUT/facts.txt"
