#!/bin/bash
# The Hot 60 Pro's firmware, out of YOUR phone's Android, into fw-*/ (at the top of the HakiOS tree, or next to this repository's mtkloader/)
# (git ignores them: they are the vendor's, and nothing of the vendor's is in the repository).
#
#   fw-connsys/   CONNSYS's (Wi-Fi, Bluetooth), for when that work starts: every file under /vendor/firmware whose name
#                 says WIFI, BT, WMT or soc (tools/x6885-facts.sh lists them all)
#
# The Hot 20's touch firmware (hdl_firmware.img, which its loader sent to the TD4160) has no counterpart here: the Hot 60
# Pro's Goodix controller keeps its own.
#
# Android keeps them where the shell user cannot read (SELinux), so this needs root on the phone
# (Magisk's su): they are copied to a temporary folder, pulled, and the folder is removed. Read only:
# nothing on the phone is changed.
set -e
S=${HAKIOS_SERIAL:?set HAKIOS_SERIAL to the serial of your phone, as adb devices shows it}
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -z "${1:-}" ] && [ -d "$HERE/../../mtk/loader" ]; then OUT="$(cd "$HERE/../.." && pwd)"; else OUT="${1:-$HERE}"; fi
TMP=/data/local/tmp/hakios-fw

adb -s "$S" shell "su -c 'rm -rf $TMP; mkdir -p $TMP; \
	for f in \$(ls /vendor/firmware | grep -E \"WIFI|BT|WMT|soc\"); do cp /vendor/firmware/\$f $TMP/; done; \
	chmod 644 $TMP/*'"
mkdir -p "$OUT/fw-connsys"
adb -s "$S" pull "$TMP/." "$OUT/fw-connsys/" >/dev/null
adb -s "$S" shell "su -c 'rm -rf $TMP'"
ls -l "$OUT/fw-connsys"
