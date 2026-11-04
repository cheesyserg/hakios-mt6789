# The Hot 60 Pro (X6885, MT6789): the port from the Hot 20

**State, 4 November 2026: written, compiled, not yet run on the phone.** Nothing here has a run's
log behind it yet; the first run is the probe below, and the numbers in this file are what the
phone's own device tree and Android's drivers for it say, not what the phone has shown.

## What the phone is, and where that came from

From the X6885's own device tree (the `dtb.img` a recovery tree for this phone carries:
XTENSEI/twrp_device_infinix_X6885 — read for addresses, not kept here), its BoardConfig, and the
names of Android's kernel modules for it:

| | Hot 20 (X6826B, MT6768) | Hot 60 Pro (X6885, MT6789) |
| --- | --- | --- |
| CPU | 6× A55 + 2× A75, MPIDR 0x000–0x500 / 0x600, 0x700 | 6× A55 + 2× **A76**, the same MPIDRs |
| GIC, UART0, TOPRGU | 0x0c000000 / 0x0c040000, 0x11002000, 0x10007000 | the same |
| PMIC, wrapper | MT6358 at 0x1000d000 | **MT6366** (MT6358's sibling, `mt6358-regulator`) at **0x10026000** |
| keypad | 0x10010000, keys 0 and 1 | 0x10010000, key 0 = volume down (0x72); power through the PMIC |
| flash | eMMC, MSDC0 0x11230000 | **UFS 2.2** at **0x11270000** (`mediatek,mt8183-ufshci`, MCQ off); microSD on MSDC1 0x11240000 |
| display | 720×1640 LCD, video mode, OVL0 0x1400b000 | **1224×2720 AMOLED**, video mode with DSC (`nt37706a_…_dsi_vdo_dsc_…_x6885`, `vtdr6126a_…_dsi_vdo_dsc_…_x6885`); mutex 0x14001000, **OVL0 0x14005000**, DSC 0x14012000, DSI0 0x14013000 |
| touch | TD4160 over SPI2, firmware sent by the loader | **Goodix** (`goodix,touch`) |
| GPU | Mali-G52 MC2 (Bifrost) | **Mali-G57 MC2 (Valhall)** at 0x13000000 |
| boot image | header v2, kernel entered at 0x40080000 | **header v4 (GKI)**, base 0x3fff8000 + offset 0x8000: entered at **0x40000000**; ramdisk in `init_boot` |
| RAM | 6 GB (4 for XNU at 0xc0000000) | 8 GB from 0x40000000 (4 for XNU at **0x140000000**) |

## What changed

- **Boot image** (`tools/chainboot.py`): headers v3/v4 besides v2; the kernel kept in the form it came
  in (raw, gzip, lz4); the v4 boot signature dropped (it was the old kernel's); a check that the
  unpacked image stays clear of LK's device tree (`KERNEL_ROOM`, 0x07c80000 from the kernel address
  on this phone) and of the boot partition's size (`BOOT_PARTITION`, 64 MB by default for v4 — read
  the real one with `blockdev --getsize64`).
- **Load address** (`mtkloader/start.S`): the loader stays linked at 0x40080000 and moves its whole
  image there (memmove) when LK enters it anywhere else — 0x40000000 here. Android's Image goes back
  to exactly where LK entered (`lk_entry_pa`), which is where LK would have put it; the chain stub
  is memmove-safe and runs from just past the image instead of a fixed 0x4a000000.
- **No fixed board addresses** (`mtkloader/board.h`): DRAM, every reserved range (reserved-memory
  nodes and the FDT's memreserve block), ramoops (laid out as `fs/pstore/ram.c` does), the UFS,
  OVL0, DSI, mutex, Mali, MSDC1, keypad and PMIC-wrapper bases are read out of the tree LK passes
  in x0. XNU's window and the loader's 16 MB scratch below it (run marker, panic log, UFS DMA
  window) are checked against that map; anything on a reservation or outside DRAM refuses the run
  and says so. Until the check passes, nothing of DRAM outside the loader's own image is written
  (the marker, the ramoops zones fall back to a buffer of our own).
- **Display**: OVL0's base from the tree, the panel's size from OVL0's ROI register, the
  framebuffer from OVL0's layer-0 address if it lies in DRAM; the DSI mode is read and reported
  (if it were command mode, nothing drawn would show without a frame being sent).
- **UFS, read only** (`mtkloader/ufs.h`, `kernel/bsd/dev/redwood_ufs.c`): as the eMMC was read on
  the Hot 20 — nothing set up, the host taken as LK left it (enabled, link up). One transfer request
  at a time, polled, the host's interrupts off; one slot of the request list LK runs, pointed at a
  command descriptor and data in the DMA window (if LK's list is stopped and the host ready, our own
  list in the window instead). SCSI READ CAPACITY(10) and READ(10) only. The LUs 0–7 are looked at
  for the GPT holding `xnusys`. MediaTek's reference-clock handshake is asked for again if LK
  released it. The kernel gets `ufs_root=1 ufs_lun=N ufs_dma=…` and exposes `/dev/ufs0`,
  `/dev/rufs0` (512-byte blocks over the LU's 4 KB ones), `kern.redwood.ufs`.
- **GPT** (`tools/gpt-carve.py`): the sector size is the GPT's own (4096 on this UFS).
- **Buttons** (`redwood_mtkkeys.c`): `buttons=3` is this phone (the wrapper at 0x10026000); the
  Hot 20's clock and GPU-rail code runs only with `buttons=2`.
- **The Hot 20's own steps** (TD4160 firmware over SPI2, its ARMPLL, the eMMC test, the G52 probe)
  are behind `HAKIOS_X6826B=1` and are off here.

## Running it: in this order

Each step reads before anything is written, and says its result in the log.

1. `tools/x6885-facts.sh` — under rooted Android, reads only: the live memory map
   (`/proc/iomem`, the reserved-memory nodes), ramoops, `boot_a`'s header and size, which `sdX` is
   the user LU, the touch controller, the panel, the firmware names, the live device tree and the
   GPT. Into `logs/x6885-facts/` (not for the tree). Check there that 0x13f000000–0x240000000 is
   plain System RAM; if not, build with other `-DXNU_PHYS_BASE= -DXNU_MEM_SIZE=`.
2. `tools/x6885-probe.sh` — the loader with `-DBOARD_PROBE=1` in front of the stock `boot_a`: the
   entry address, LK's tree as read, the check of XNU's window, OVL0/DSI registers, the UFS
   host's registers, each LU's capacity and GPT, `xnusys` if there is one. 60 s on the screen,
   then Android; the log from pstore; the stock `boot_a` back. The watchdog is armed for 30 s
   around the reads, so a read that hangs resets the phone into Android.
3. `xnusys`: `gpt-carve.py` on the user LU's GPT (wipes `userdata`, as on the Hot 20), then
   `tools/system-flash.sh`.
4. `tools/xnu-cycle.sh` with `XNU_ARGS="cpus=4 buttons=3"` — XNU, its root on the UFS.

## Not done

- The common repository's `mtk/mt6789/` layer (`mt6789.config`, these drivers under `hakios.*`
  names, the `ufs_root=1` hook in `IOFindBSDRoot` and `bsd/conf/files`, as for `emmc_root=1`).
- Touch: a driver for the Goodix controller (the protocol from Linux's `goodix_berlin`, read, not copied).
- GPU: the Mali-G57's power-up (MFG power domains, clocks; Valhall IDs) and the Metal driver's
  Valhall compiler (hakios-metal). No voltage is raised for it.
- Wi-Fi/Bluetooth: the MT6789's CONNSYS is not the MT6768's; `redwood_connsys.c` does not apply.
- `fbdiag.c` and `diag.c` are still the Hot 20's (fixed addresses, header v2).
