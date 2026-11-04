# hakios-mt6789

HakiOS for the **Infinix Hot 60 Pro (X6885)**: MediaTek Helio G200 (**MT6789**), GPU **Mali-G57 MC2**, UFS 2.2, 8 GB, a
1224×2720 AMOLED. iOS 16.5's own user space on Apple's open XNU kernel (xnu-8796.121.2), with this phone's own loader and
drivers, ported from the Hot 20 (X6826B, MT6768).

**State: the port is written and compiles; it has not run on the phone yet.** What changed from the Hot 20, what is not done,
and the order to run it in: [docs/x6885-port.md](docs/x6885-port.md). The table below is the Hot 20's, where all of it ran.

Nothing on the phone is replaced for good: `mtkloader` sits in `boot_a` in front of Android's own kernel,
starts XNU once when Android asks for it, and the phone comes back to Android after every run. The iOS
system image lives in a partition of its own (`xnusys`, carved from the end of `userdata`) and the kernel
reads it off the flash (the UFS here, the eMMC on the Hot 20) as it needs it.

## First runs on the Hot 60 Pro

```sh
export HAKIOS_SERIAL=...      # your phone, as adb devices shows it
tools/x6885-facts.sh          # under rooted Android, reads only: memory map, ramoops, boot_a, UFS LUs -> logs/
tools/x6885-probe.sh          # the loader reads and reports (screen and pstore), no XNU; the stock boot_a back after
```

## What works (on the Hot 20)

| | | |
| --- | --- | --- |
| boot | works | mtkloader (`mtk/loader/` of the common tree): MMU off, eMMC through MSDC0, device tree for XNU; 4 cores |
| CPU | works | big cores clocked up (`mtk_cpufreq=3`) |
| storage | works, read only | the system image read off the eMMC as needed (`hakios.mtk_emmc.c`, `emmc_root=1`): ~2.5 GB of 4 for iOS (docs/x6826b-emmc.md) |
| display | works | 720×1640, iOS at scale 2 |
| touch | works | OmniVision TD4160 over SPI (`hakios.tcm.c`; the loader sends its firmware) |
| buttons | works | power, volume (`hakios.mtk_keys.c`) |
| GPU | works | Mali-G52 brought up from the kernel (`hakios.mali.c`, `hakios.mali.cpp`), Mesa's panfrost, a Metal driver: CoreAnimation draws through Metal, iOS 16's own wallpaper |
| sensors | partial | accelerometer through the SCP sensor hub (`hakios.mtk_scp.c`), screen rotation (docs/sensors-x6826b.md) |
| Wi-Fi / Bluetooth | in progress | CONNSYS powered, firmware in, its CPU running, the WMT command channel over BTIF/STP, the MCU patch by PDA taken (`hakios.mtk_connsys.c`, docs/wifi-x6826b.md); no network yet |
| audio, camera, haptics, cellular | missing | |

## What is here

- `mtkloader/` — the bare-metal loader (MIT): chains in front of Android's kernel in `boot_a`, brings up
  what XNU cannot (touch firmware, PMIC, clocks), reads the eMMC, builds XNU's device tree and boot
  arguments.
- `kernel/` — this phone's drivers in XNU (APSL 2.0, see `kernel/README.md`).
- `tools/` — running it: `xnu-cycle.sh` (one run: build, flash `boot_a`, start XNU, fetch the log),
  `boot-xnu.sh`, `chainboot.py`, `system-flash.sh` (the system image into `xnusys`), `gpt-carve.py`,
  probes (`gpu-probe.sh`, `touch-probe.sh`, `msdc-test.sh`), log readers (`fbshot.py`, `frames.py`,
  `cache-name.py`, `rwm-recipes.py`).
- `configs/` — `x6826b_defconfig` and `x6826b.config`: the kernel's configuration for this phone in the common tree.
- `pull-firmware.sh` — the phone's firmware out of **your** phone into `fw-*/` (needs root on Android).
- `docs/` — how each part was found and what each run showed.
- `device.conf` — the phone and the state of each component.

The common tree's branch `mtk` ([hakios-xnu-kernel, branch mtk](https://github.com/oosexgang/hakios-xnu-kernel/tree/mtk)) has
the same loader and scripts (`mtk/loader/`, `mtk/tools/`) and these drivers under `hakios` names (`mtk/mt6768/`), and builds and
runs them with `make`.

## Not here, by design

No file of iOS, no firmware of the phone, no screenshot (LEGAL.md). The loader's build makes two headers and git ignores both:
`touchfw.h` from your phone's `hdl_firmware.img` (`pull-firmware.sh`), and `font8x16.h`, the loader's text font, rasterized on
your Mac.

## Running

In the common tree, the branch `mtk` (what it needs:
- an Infinix Hot 20 (X6826B) with an unlocked bootloader, rooted Android (Magisk), its stock
  `boot_a` image saved (`backup/boot_a_magisk.img` at the top of the tree);
- a Mac, your own Darwin userland 16.5 IPSW (for `xnuloader/identity.h`)).

```sh
git clone -b mtk https://github.com/oosexgang/hakios-xnu-kernel.git hakios && cd hakios
tools/hakios-device.py sync x6826b      # this repository, into devices/x6826b
export HAKIOS_SERIAL=...                # your phone, as `adb devices` shows it
devices/x6826b/pull-firmware.sh         # fw-touch/, fw-connsys/
xnuloader/mkidentity.sh your.ipsw
make x6826b_defconfig && make kernel    # hakios-kernel: mali mtk_keys mtk_scp tcm mtk_emmc mtk_connsys
make phone SECS=300 RAMDISK=...         # boot_a written, XNU once, Android back, the log in logs/
```

The user space that runs on this phone (Mesa's panfrost, the Metal driver) still reads the project's former names
(`kern.redwood.*`, `RedwoodMali`) and is not yet in the common tree; `RAMDISK=` and the system image in `xnusys` are its.

With this repository's own `tools/`, in the layout of the tree they were written in (`mtkloader/` and `tools/` at its top, next
to `xnuloader/`, `xnu-deps/` and `userland/`):

```sh
./pull-firmware.sh                   # fw-touch/, fw-connsys/ next to this repository's mtkloader/
XNU_SECS=300 XNU_ARGS="cpus=4 touchscreen=2 buttons=2 mali=1 mtk_cpufreq=3 metal=1 camtl=1" \
    tools/xnu-cycle.sh                # one run; the log lands in logs/
```

## Licenses

`kernel/` is APSL 2.0 (`LICENSE.APSL`), as code built into XNU; everything else is MIT (`LICENSE`).
See `NOTICE` and `LEGAL.md`.
