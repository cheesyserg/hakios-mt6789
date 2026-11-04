#!/usr/bin/env python3
"""
Pack xnu-flashed-slot_a.img: mtkloader, then Android's kernel, then the XNU payload, in the
Android boot image (backup/boot_a.img by default: on the Hot 60 Pro, a GKI phone, Magisk patches
init_boot and boot_a stays the stock one; the Hot 20's was backup/boot_a_magisk.img). Its header,
ramdisk and cmdline are kept. Booting it runs XNU once, then Android comes back with XNU's log in
/sys/fs/pstore/console-ramoops-0 (see mtkloader/mtkloader.c).

  build_flashed.py [android-boot.img]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

android = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'backup', 'boot_a.img')
# (no payload: an image that only probes, mtkloader built with -DBOARD_PROBE=1)
payload = os.path.join(ROOT, 'build-payload.bin')
subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'chainboot.py'), android,
                       os.path.join(HERE, 'mtkloader.bin'), os.path.join(ROOT, 'xnu-flashed-slot_a.img')] +
                      ([payload] if os.path.exists(payload) and not os.environ.get('NO_PAYLOAD') else []))
