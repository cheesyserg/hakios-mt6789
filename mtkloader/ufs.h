/*
 * The Hot 60 Pro's UFS (2.2, MediaTek's host at 0x11270000, "mediatek,mt8183-ufshci"), read only, the
 * way the eMMC was read on the Hot 20 (msdc.h): nothing is set up here. LK has brought the host and
 * the link up, read the GPT and the boot partition through it, and leaves it enabled; Android's
 * Linux resets and sets it up again from nothing after us. So: no reset, no link start-up, no
 * UniPro or M-PHY register -- one transfer request at a time through the request list the
 * controller already runs (SDB mode: the tree says mediatek,ufs-disable-mcq), polled.
 *
 * What is used of UFSHCI (JESD223; the register and descriptor layout as Linux's include/ufs/ufshci.h
 * has it):
 *   HCE (0x34) bit 0 must be 1, HCS (0x30) DP and UTRLRDY;
 *   UTRLBA (0x50/0x54) is LK's list, UTRLRSR (0x60) bit 0 it running: one of its slots is
 *   borrowed, pointed at our own command descriptor. If the list is not running but the host
 *   says it is ready for one (UTRLRDY), our own list is given to it and started -- the only time
 *   anything but a slot of it and the doorbell is written;
 *   UTRLDBR (0x58): the doorbell, cleared by the host when the request is done;
 *   IS (0x20) cleared, IE (0x24) set to 0: nothing here or in XNU takes the UFS interrupt.
 * The SCSI commands are READ CAPACITY(10) and READ(10), nothing else: this cannot write.
 *
 * Everything the controller reads or writes lies in UFS_DMA_PA, a range outside XNU's memory that
 * board.h has checked is DRAM and nobody's (mtkloader.c, the run's checks), and which the kernel's
 * driver (bsd/dev/redwood_ufs.c) uses after us at the same place. With the MMU off it is all device
 * memory: nothing to clean or invalidate.
 *
 * Logical units: MediaTek puts the two preloader copies in the boot LUs and the GPT of everything
 * else in a user LU; which one is not assumed, the LUs 0..7 are looked at for a GPT that holds the
 * partition asked for.
 */
#ifndef UFS_H
#define UFS_H

#include <stdint.h>

#define UFS_BASE_DEFAULT     0x11270000ULL

#define UFS_CAP              0x00
#define UFS_VER              0x08
#define UFS_IS               0x20
#define UFS_IE               0x24
#define UFS_HCS              0x30
#define UFS_HCE              0x34
#define UFS_UTRLBA           0x50
#define UFS_UTRLBAU          0x54
#define UFS_UTRLDBR          0x58
#define UFS_UTRLCLR          0x5c
#define UFS_UTRLRSR          0x60
#define UFS_MTK_REFCLK_CTRL  0x144     /* MediaTek's (ufs-mediatek.h REG_UFS_REFCLK_CTRL): reported only */

#define UFS_HCS_DP           (1u << 0)
#define UFS_HCS_UTRLRDY      (1u << 1)

/* Our DMA window: layout shared with bsd/dev/redwood_ufs.c */
#ifndef UFS_DMA_PA
#define UFS_DMA_PA           0x13f200000ULL
#endif
#define UFS_DMA_SIZE         0x200000u
#define UFS_DMA_LIST         0x0000u     /* our own request list, if LK's is not running: 32 x 32 bytes */
#define UFS_DMA_UCD          0x1000u     /* command UPIU at +0, response UPIU at +512, PRDT at +1024 */
#define UFS_DMA_DATA         0x10000u    /* the data, up to UFS_MAX_BYTES */
#define UFS_UCD_RSP          512u
#define UFS_UCD_PRDT         1024u
#define UFS_PRDT_MAX         256u        /* PRDT entries of up to 256 KB each */
#define UFS_MAX_BYTES        0x100000u   /* 1 MB a command */

struct ufs_state {
	uint64_t base;
	uint64_t list;          /* the request list in use */
	uint32_t slot;
	int ready;              /* 1: a request can be sent */
	int own_list;
	/* the partition, once found */
	uint32_t lun, block_size;
	uint64_t lun_blocks, part_lba, part_blocks;
	/* the last failure */
	const char *what;
	uint32_t status;
};

static struct ufs_state ufs = { .base = UFS_BASE_DEFAULT };

static inline uint32_t ufs_rd(uint32_t reg) { return *(volatile uint32_t *)(ufs.base + reg); }
static inline void ufs_wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(ufs.base + reg) = v; }

static int ufs_fail(const char *what, uint32_t status)
{
	ufs.what = what;
	ufs.status = status;
	return 0;
}

static inline void ufs_put32(uint64_t pa, uint32_t v) { *(volatile uint32_t *)pa = v; }
static inline uint32_t ufs_get32(uint64_t pa) { return *(volatile uint32_t *)pa; }
static inline uint8_t ufs_get8(uint64_t pa) { return *(volatile uint8_t *)pa; }
static inline uint32_t ufs_bswap32(uint32_t v) { return __builtin_bswap32(v); }

/*
 * Take the controller as LK left it. Returns 1 if requests can be sent. Writes, at most: IE (0),
 * IS (its own bits back, to clear them) and -- only with LK's list stopped -- UTRLBA and UTRLRSR.
 */
static int ufs_attach(uint64_t base)
{
	uint32_t hce, hcs, rsr, dbr;
	uint64_t lba;

	ufs.base = base ? base : UFS_BASE_DEFAULT;
	ufs.ready = 0;
	hce = ufs_rd(UFS_HCE);
	hcs = ufs_rd(UFS_HCS);
	if (!(hce & 1)) {
		return ufs_fail("host not enabled (HCE 0): LK left it off", hce);
	}
	if (!(hcs & UFS_HCS_DP)) {
		return ufs_fail("no device present (HCS)", hcs);
	}
	ufs_wr(UFS_IE, 0);
	ufs_wr(UFS_IS, ufs_rd(UFS_IS));
	/*
	 * The device's reference clock: if LK released it (MediaTek's handshake, REFCLK_REQUEST bit 0
	 * answered by REFCLK_ACK bit 1, as ufs-mediatek.c's ufs_mtk_setup_ref_clk), ask for it again and
	 * wait up to 3 ms for the answer.
	 */
	if ((ufs_rd(UFS_MTK_REFCLK_CTRL) & 3u) != 3u) {
		uint64_t end = dbg_ticks() + dbg_freq() / 333;
		ufs_wr(UFS_MTK_REFCLK_CTRL, 1u);
		while ((ufs_rd(UFS_MTK_REFCLK_CTRL) & 3u) != 3u && dbg_ticks() < end) {
		}
		if ((ufs_rd(UFS_MTK_REFCLK_CTRL) & 3u) != 3u) {
			return ufs_fail("reference clock not acknowledged", ufs_rd(UFS_MTK_REFCLK_CTRL));
		}
	}
	rsr = ufs_rd(UFS_UTRLRSR);
	lba = (uint64_t)ufs_rd(UFS_UTRLBAU) << 32 | ufs_rd(UFS_UTRLBA);
	dbr = ufs_rd(UFS_UTRLDBR);
	if (rsr & 1) {
		if (!lba || (lba & 0x3ff)) {
			return ufs_fail("request list running at no proper address", (uint32_t)lba);
		}
		ufs.list = lba;
		ufs.own_list = lba == UFS_DMA_PA + UFS_DMA_LIST;
	} else if (hcs & UFS_HCS_UTRLRDY) {
		for (uint32_t i = 0; i < 32 * 8; i++) {
			ufs_put32(UFS_DMA_PA + UFS_DMA_LIST + i * 4, 0);
		}
		ufs_wr(UFS_UTRLBA, (uint32_t)(UFS_DMA_PA + UFS_DMA_LIST));
		ufs_wr(UFS_UTRLBAU, (uint32_t)((UFS_DMA_PA + UFS_DMA_LIST) >> 32));
		__asm__ volatile("dsb sy");
		ufs_wr(UFS_UTRLRSR, 1);
		ufs.list = UFS_DMA_PA + UFS_DMA_LIST;
		ufs.own_list = 1;
		dbr = 0;
	} else {
		return ufs_fail("request list stopped, host not ready for one (HCS)", hcs);
	}
	/* the lowest slot nobody has rung for; LK has nothing outstanding, so slot 0 as a rule */
	ufs.slot = 0;
	while (ufs.slot < 32 && (dbr & (1u << ufs.slot))) {
		ufs.slot++;
	}
	if (ufs.slot == 32) {
		return ufs_fail("every slot of the request list busy", dbr);
	}
	ufs.ready = 1;
	return 1;
}

/*
 * One SCSI command that reads `bytes` (a multiple of 4, at most UFS_MAX_BYTES) into the window's data
 * area. Returns 1 on GOOD status; on CHECK CONDITION the sense key is kept in ufs.status.
 */
static int ufs_scsi_in(uint32_t lun, const uint8_t *cdb, uint32_t cdb_len, uint32_t bytes)
{
	const uint64_t ucd = UFS_DMA_PA + UFS_DMA_UCD, data = UFS_DMA_PA + UFS_DMA_DATA;
	const uint64_t utrd = ufs.list + (uint64_t)ufs.slot * 32;
	uint32_t prdt = 0;

	if (!ufs.ready) {
		return ufs_fail("UFS not attached", 0);
	}
	if (bytes == 0 || bytes > UFS_MAX_BYTES || (bytes & 3)) {
		return ufs_fail("bad length", bytes);
	}
	for (uint32_t i = 0; i < UFS_UCD_PRDT / 4; i++) {
		ufs_put32(ucd + i * 4, 0);
	}
	/* command UPIU: COMMAND, flags READ, the LU, task tag = the slot, SCSI; expected length; CDB */
	ufs_put32(ucd + 0, 0x01u | 0x40u << 8 | (lun & 0xff) << 16 | ufs.slot << 24);
	ufs_put32(ucd + 4, 0);
	ufs_put32(ucd + 8, 0);
	ufs_put32(ucd + 12, ufs_bswap32(bytes));
	for (uint32_t i = 0; i < 16; i += 4) {
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4; k++) {
			w |= (uint32_t)(i + k < cdb_len ? cdb[i + k] : 0) << (8 * k);
		}
		ufs_put32(ucd + 16 + i, w);
	}
	/* PRDT: 256 KB a piece */
	for (uint32_t at = 0; at < bytes; at += 0x40000, prdt++) {
		uint32_t n = bytes - at < 0x40000 ? bytes - at : 0x40000;
		uint64_t e = ucd + UFS_UCD_PRDT + prdt * 16;
		ufs_put32(e + 0, (uint32_t)(data + at));
		ufs_put32(e + 4, (uint32_t)((data + at) >> 32));
		ufs_put32(e + 8, 0);
		ufs_put32(e + 12, n - 1);
	}
	/* UTRD: UFS storage, device to host, no interrupt; OCS "invalid" until the host writes it */
	ufs_put32(utrd + 0, 1u << 28 | 2u << 25);
	ufs_put32(utrd + 4, 0);
	ufs_put32(utrd + 8, 0x0f);
	ufs_put32(utrd + 12, 0);
	ufs_put32(utrd + 16, (uint32_t)ucd);
	ufs_put32(utrd + 20, (uint32_t)(ucd >> 32));
	ufs_put32(utrd + 24, (UFS_UCD_RSP / 4) << 16 | (512u / 4));
	ufs_put32(utrd + 28, (UFS_UCD_PRDT / 4) << 16 | prdt);
	__asm__ volatile("dsb sy" ::: "memory");
	ufs_wr(UFS_UTRLDBR, 1u << ufs.slot);

	uint64_t end = dbg_ticks() + dbg_freq() * 2;
	while (ufs_rd(UFS_UTRLDBR) & (1u << ufs.slot)) {
		if (dbg_ticks() > end) {
			ufs_wr(UFS_UTRLCLR, ~(1u << ufs.slot));   /* (0 bits clear: this slot) */
			return ufs_fail("no answer in 2 s (doorbell still set)", ufs_rd(UFS_IS));
		}
	}
	ufs_wr(UFS_IS, ufs_rd(UFS_IS));
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t ocs = ufs_get32(utrd + 8) & 0xff;
	if (ocs != 0) {
		return ufs_fail("overall command status", ocs);
	}
	/* response UPIU: byte 6 response, byte 7 SCSI status; sense data after the 32-byte header */
	uint32_t w1 = ufs_get32(ucd + UFS_UCD_RSP + 4);
	uint8_t response = (uint8_t)(w1 >> 16), status = (uint8_t)(w1 >> 24);
	if (response != 0) {
		return ufs_fail("UPIU response", response);
	}
	if (status != 0) {
		uint8_t key = ufs_get8(ucd + UFS_UCD_RSP + 32 + 2 + 2) & 0x0f;
		return ufs_fail(status == 2 ? "check condition (sense key)" : "SCSI status", status == 2 ? key : status);
	}
	return 1;
}

/* the same, once more after a unit attention (the first command a LU sees after its reset) */
static int ufs_scsi_in_retry(uint32_t lun, const uint8_t *cdb, uint32_t cdb_len, uint32_t bytes)
{
	for (int i = 0; i < 3; i++) {
		if (ufs_scsi_in(lun, cdb, cdb_len, bytes)) {
			return 1;
		}
		if (!(ufs.what && ufs.what[0] == 'c' && ufs.status == 6)) { /* UNIT ATTENTION */
			return 0;
		}
	}
	return 0;
}

static int ufs_capacity(uint32_t lun, uint64_t *blocks, uint32_t *block_size)
{
	const uint8_t cdb[10] = { 0x25 };
	const uint64_t d = UFS_DMA_PA + UFS_DMA_DATA;

	if (!ufs_scsi_in_retry(lun, cdb, sizeof(cdb), 8)) {
		return 0;
	}
	*blocks = (uint64_t)ufs_bswap32(ufs_get32(d)) + 1;
	*block_size = ufs_bswap32(ufs_get32(d + 4));
	return *block_size == 4096 || *block_size == 512;
}

/* `count` blocks of the LU's size from `lba`, into the window's data area */
static int ufs_read_lu(uint32_t lun, uint64_t lba, uint32_t count, uint32_t block_size)
{
	uint8_t cdb[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0 };

	if (lba >> 32) {
		return ufs_fail("block beyond READ(10)", (uint32_t)(lba >> 32));
	}
	return ufs_scsi_in_retry(lun, cdb, sizeof(cdb), count * block_size);
}

static void ufs_copy_out(void *dst, uint32_t bytes)
{
	const volatile uint32_t *s = (const volatile uint32_t *)(UFS_DMA_PA + UFS_DMA_DATA);
	volatile uint32_t *d = (volatile uint32_t *)dst;
	for (uint32_t i = 0; i < bytes / 4; i++) {
		d[i] = s[i];
	}
}

static int ufs_name_is(const volatile uint8_t *e, const char *name)
{
	uint32_t k = 0;
	for (; name[k]; k++) {
		if (e[56 + 2 * k] != (uint8_t)name[k] || e[57 + 2 * k] != 0) {
			return 0;
		}
	}
	return e[56 + 2 * k] == 0 && e[57 + 2 * k] == 0;
}

/*
 * The GPT of LU `lun` (block 1: "EFI PART"), and in it `name`. 1 if found (ufs.lun, part_lba,
 * part_blocks, in the LU's blocks), 0 if the LU has a GPT without it, -1 if it has none.
 */
static int ufs_gpt_find(uint32_t lun, const char *name)
{
	const uint64_t d = UFS_DMA_PA + UFS_DMA_DATA;
	uint64_t blocks;
	uint32_t bs;

	if (!ufs_capacity(lun, &blocks, &bs) || !ufs_read_lu(lun, 1, 1, bs)) {
		return -1;
	}
	if (ufs_get32(d) != 0x20494645u || ufs_get32(d + 4) != 0x54524150u) { /* "EFI PART" */
		return -1;
	}
	uint64_t ent = (uint64_t)ufs_get32(d + 76) << 32 | ufs_get32(d + 72);
	uint32_t count = ufs_get32(d + 80), size = ufs_get32(d + 84);
	if (size != 128 || count == 0 || count > 256) {
		return -1;
	}
	uint32_t bytes = count * size, nblk = (bytes + bs - 1) / bs;
	if (!ufs_read_lu(lun, ent, nblk, bs)) {
		return -1;
	}
	for (uint32_t i = 0; i < count; i++) {
		const volatile uint8_t *e = (const volatile uint8_t *)(d + i * size);
		if (ufs_name_is(e, name)) {
			uint64_t first = (uint64_t)ufs_get32(d + i * size + 36) << 32 | ufs_get32(d + i * size + 32);
			uint64_t last = (uint64_t)ufs_get32(d + i * size + 44) << 32 | ufs_get32(d + i * size + 40);
			if (!first || last < first) {
				return 0;
			}
			ufs.lun = lun;
			ufs.block_size = bs;
			ufs.lun_blocks = blocks;
			ufs.part_lba = first;
			ufs.part_blocks = last - first + 1;
			return 1;
		}
	}
	return 0;
}

/* Every LU 0..7 for the partition; the LU of the first GPT found is kept even without it (for the log). */
static int ufs_find_part(const char *name)
{
	int gpt_lun = -1;

	for (uint32_t lun = 0; lun < 8; lun++) {
		int r = ufs_gpt_find(lun, name);
		if (r == 1) {
			return 1;
		}
		if (r == 0 && gpt_lun < 0) {
			gpt_lun = (int)lun;
		}
	}
	ufs.lun = gpt_lun < 0 ? 0xff : (uint32_t)gpt_lun;
	return 0;
}

/* Bytes of the partition from byte `at` (both multiples of its block size) into dst. */
static int ufs_read_part(uint64_t at, uint64_t bytes, void *dst)
{
	uint8_t *out = (uint8_t *)dst;
	uint32_t bs = ufs.block_size;

	if (!bs || (at % bs) || (bytes % bs) || at / bs + bytes / bs > ufs.part_blocks) {
		return ufs_fail("outside the partition", (uint32_t)(at / (bs ? bs : 1)));
	}
	while (bytes) {
		uint32_t n = bytes > UFS_MAX_BYTES ? UFS_MAX_BYTES : (uint32_t)bytes;
		if (!ufs_read_lu(ufs.lun, ufs.part_lba + at / bs, n / bs, bs)) {
			return 0;
		}
		ufs_copy_out(out, n);
		out += n;
		at += n;
		bytes -= n;
	}
	return 1;
}

#endif /* UFS_H */
