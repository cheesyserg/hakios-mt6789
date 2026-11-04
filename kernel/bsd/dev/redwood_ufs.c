/*
 * The X6885's UFS (2.2, MediaTek's host at 0x11270000) as a block device: the "xnusys" partition,
 * where the system image lives, read when the kernel asks for it. Boot-arg ufs_root=1 makes it the
 * root (iokit/bsddev/IOKitBSDInit.cpp asks redwood_ufs_root for it, as it asks redwood_emmc_root
 * for emmc_root=1 on the Hot 20); /dev/ufs0 and /dev/rufs0 are its nodes once devfs is up.
 *
 * Nothing is set up here, as with the eMMC: LK brought the host and the link up and left the host
 * enabled; mtkloader (mtkloader/ufs.h) has read the GPT through it the same way and says which LU
 * holds the partition (boot-arg ufs_lun=) and where the DMA window is (ufs_dma=). One transfer
 * request at a time through the request list the controller already runs (SDB mode), polled, the
 * host's interrupts off. UFSHCI's registers and descriptors as Linux's include/ufs/ufshci.h has them.
 *
 * The window (2 MB at ufs_dma, outside the kernel's memory: the loader's scratch below XNU's window,
 * checked against Android's device tree before the run) holds the command descriptor and the data,
 * so nothing the controller touches is the kernel's memory and there is no cache to keep in step: it
 * is mapped write-combined (normal, uncached), and copied out of.
 *
 * Read only, by construction: the only SCSI commands sent are READ CAPACITY(10) and READ(10); the
 * strategy routine refuses writes and opening for writing is refused. One request at a time, under
 * a mutex, in pieces of UFS_MAX_BYTES. The device is shown with 512-byte blocks; the LU's own
 * (4096) are read around them.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/OSByteOrder.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/redwood_ufs.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

#define UFS_PHYS         0x11270000u
#define UFS_PART         "xnusys"
#define UFS_DMA_DEFAULT  0x13f200000ull     /* mtkloader's UFS_DMA_PA with its default XNU_PHYS_BASE */
#define UFS_DMA_SIZE     0x200000u
#define UFS_DMA_LIST     0x0000u
#define UFS_DMA_UCD      0x1000u
#define UFS_DMA_DATA     0x10000u
#define UFS_UCD_RSP      512u
#define UFS_UCD_PRDT     1024u
#define UFS_MAX_BYTES    0x100000u          /* 1 MB a command */

#define REG_IS           0x20
#define REG_IE           0x24
#define REG_HCS          0x30
#define REG_HCE          0x34
#define REG_UTRLBA       0x50
#define REG_UTRLBAU      0x54
#define REG_UTRLDBR      0x58
#define REG_UTRLCLR      0x5c
#define REG_UTRLRSR      0x60
#define HCS_DP           (1u << 0)

static vm_offset_t ufs_regs, ufs_dma, ufs_list;
static uint64_t ufs_dma_pa, ufs_list_pa;
static uint32_t ufs_slot, ufs_lun, ufs_bs;
static uint64_t ufs_lba, ufs_lu_blocks;     /* the partition's first block, in the LU's blocks */
static uint64_t ufs_bytes;                  /* the image's length (its APFS containers) */
static int ufs_bmajor = -1, ufs_cmajor = -1;
static const char *ufs_failed;
static uint32_t ufs_failed_status;
static uint64_t ufs_reads, ufs_read_bytes, ufs_errors;
static LCK_GRP_DECLARE(ufs_lck_grp, "redwood-ufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);

static inline uint32_t
rd(uint32_t reg)
{
	return *(volatile uint32_t *)(ufs_regs + reg);
}

static inline void
wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(ufs_regs + reg) = value;
}

static inline void
put32(vm_offset_t va, uint32_t value)
{
	*(volatile uint32_t *)va = value;
}

static inline uint32_t
get32(vm_offset_t va)
{
	return *(volatile uint32_t *)va;
}

static bool
fail(const char *what, uint32_t status)
{
	ufs_failed = what;
	ufs_failed_status = status;
	return false;
}

/* The host as LK (and the loader) left it; the same rules as mtkloader/ufs.h's ufs_attach. */
static bool
attach(void)
{
	uint32_t hce, hcs, rsr, dbr;
	uint64_t list;

	ufs_regs = ml_io_map(UFS_PHYS, 0x4000);
	if (!PE_parse_boot_argn("ufs_dma", &ufs_dma_pa, sizeof(ufs_dma_pa)) || ufs_dma_pa == 0) {
		ufs_dma_pa = UFS_DMA_DEFAULT;
	}
	ufs_dma = ml_io_map_wcomb((vm_offset_t)ufs_dma_pa, UFS_DMA_SIZE);
	hce = rd(REG_HCE);
	hcs = rd(REG_HCS);
	if (!(hce & 1)) {
		return fail("host not enabled", hce);
	}
	if (!(hcs & HCS_DP)) {
		return fail("no device present", hcs);
	}
	wr(REG_IE, 0);
	wr(REG_IS, rd(REG_IS));
	rsr = rd(REG_UTRLRSR);
	list = (uint64_t)rd(REG_UTRLBAU) << 32 | rd(REG_UTRLBA);
	if (!(rsr & 1) || list == 0 || (list & 0x3ff)) {
		return fail("request list not running (the loader starts one)", rsr);
	}
	ufs_list_pa = list;
	ufs_list = list == ufs_dma_pa + UFS_DMA_LIST ? ufs_dma + UFS_DMA_LIST : ml_io_map((vm_offset_t)(list & ~0x3fffull), 0x4000) + (list & 0x3fff);
	dbr = rd(REG_UTRLDBR);
	for (ufs_slot = 0; ufs_slot < 32 && (dbr & (1u << ufs_slot)); ufs_slot++) {
	}
	if (ufs_slot == 32) {
		return fail("every slot busy", dbr);
	}
	return true;
}

/* One SCSI command reading `bytes` (a multiple of 4, at most UFS_MAX_BYTES) into the window. */
static bool
scsi_in(uint32_t lun, const uint8_t *cdb, uint32_t cdb_len, uint32_t bytes)
{
	const vm_offset_t ucd = ufs_dma + UFS_DMA_UCD, utrd = ufs_list + ufs_slot * 32;
	const uint64_t ucd_pa = ufs_dma_pa + UFS_DMA_UCD, data_pa = ufs_dma_pa + UFS_DMA_DATA;
	uint32_t prdt = 0, w1, ocs;
	uint64_t deadline;

	if (bytes == 0 || bytes > UFS_MAX_BYTES || (bytes & 3)) {
		return fail("bad length", bytes);
	}
	for (uint32_t i = 0; i < UFS_UCD_PRDT / 4; i++) {
		put32(ucd + i * 4, 0);
	}
	put32(ucd + 0, 0x01u | 0x40u << 8 | (lun & 0xff) << 16 | ufs_slot << 24);
	put32(ucd + 12, OSSwapInt32(bytes));
	for (uint32_t i = 0; i < 16; i += 4) {
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4; k++) {
			w |= (uint32_t)(i + k < cdb_len ? cdb[i + k] : 0) << (8 * k);
		}
		put32(ucd + 16 + i, w);
	}
	for (uint32_t at = 0; at < bytes; at += 0x40000, prdt++) {
		uint32_t n = bytes - at < 0x40000 ? bytes - at : 0x40000;
		vm_offset_t e = ucd + UFS_UCD_PRDT + prdt * 16;
		put32(e + 0, (uint32_t)(data_pa + at));
		put32(e + 4, (uint32_t)((data_pa + at) >> 32));
		put32(e + 8, 0);
		put32(e + 12, n - 1);
	}
	put32(utrd + 0, 1u << 28 | 2u << 25);       /* UFS storage, device to host, no interrupt */
	put32(utrd + 4, 0);
	put32(utrd + 8, 0x0f);                      /* OCS invalid until the host writes it */
	put32(utrd + 12, 0);
	put32(utrd + 16, (uint32_t)ucd_pa);
	put32(utrd + 20, (uint32_t)(ucd_pa >> 32));
	put32(utrd + 24, (UFS_UCD_RSP / 4) << 16 | (512u / 4));
	put32(utrd + 28, (UFS_UCD_PRDT / 4) << 16 | prdt);
	__builtin_arm_dsb(0xf);
	wr(REG_UTRLDBR, 1u << ufs_slot);

	nanoseconds_to_absolutetime(2ull * NSEC_PER_SEC, &deadline);
	deadline += mach_absolute_time();
	while (rd(REG_UTRLDBR) & (1u << ufs_slot)) {
		if (mach_absolute_time() > deadline) {
			wr(REG_UTRLCLR, ~(1u << ufs_slot));
			return fail("no answer in 2 s", rd(REG_IS));
		}
	}
	wr(REG_IS, rd(REG_IS));
	__builtin_arm_dsb(0xf);
	ocs = get32(utrd + 8) & 0xff;
	if (ocs != 0) {
		return fail("overall command status", ocs);
	}
	w1 = get32(ucd + UFS_UCD_RSP + 4);
	if ((w1 >> 16) & 0xff) {
		return fail("UPIU response", (w1 >> 16) & 0xff);
	}
	if (w1 >> 24) {
		uint8_t key = *(volatile uint8_t *)(ucd + UFS_UCD_RSP + 32 + 4) & 0x0f;
		return fail((w1 >> 24) == 2 ? "check condition (sense key)" : "SCSI status", (w1 >> 24) == 2 ? key : (w1 >> 24));
	}
	return true;
}

static bool
scsi_in_retry(uint32_t lun, const uint8_t *cdb, uint32_t cdb_len, uint32_t bytes)
{
	for (int i = 0; i < 3; i++) {
		if (scsi_in(lun, cdb, cdb_len, bytes)) {
			return true;
		}
		ufs_errors++;
		if (!(ufs_failed[0] == 'c' && ufs_failed_status == 6)) {      /* only a UNIT ATTENTION is tried again */
			return false;
		}
	}
	return false;
}

static bool
capacity(uint32_t lun, uint64_t *blocks, uint32_t *bs)
{
	const uint8_t cdb[10] = { 0x25 };

	if (!scsi_in_retry(lun, cdb, sizeof(cdb), 8)) {
		return false;
	}
	*blocks = (uint64_t)OSSwapInt32(get32(ufs_dma + UFS_DMA_DATA)) + 1;
	*bs = OSSwapInt32(get32(ufs_dma + UFS_DMA_DATA + 4));
	return *bs == 4096 || *bs == 512;
}

/* `count` blocks of the LU from `lba` into the window's data area */
static bool
read_lu(uint32_t lun, uint64_t lba, uint32_t count, uint32_t bs)
{
	uint8_t cdb[10] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		            0, (uint8_t)(count >> 8), (uint8_t)count, 0 };

	if (lba >> 32) {
		return fail("block beyond READ(10)", (uint32_t)(lba >> 32));
	}
	return scsi_in_retry(lun, cdb, sizeof(cdb), count * bs);
}

/* bytes [at, at + len) of the partition into dst, through the window, at most UFS_MAX_BYTES - ufs_bs at a time */
static bool
read_part(uint64_t at, uint32_t len, void *dst)
{
	uint64_t first = at / ufs_bs, last = (at + len - 1) / ufs_bs;
	uint32_t blocks = (uint32_t)(last - first + 1);

	if (blocks * ufs_bs > UFS_MAX_BYTES) {
		return fail("piece too large", len);
	}
	if (!read_lu(ufs_lun, ufs_lba + first, blocks, ufs_bs)) {
		return false;
	}
	memcpy(dst, (const void *)(ufs_dma + UFS_DMA_DATA + (at - first * ufs_bs)), len);
	ufs_reads++;
	ufs_read_bytes += len;
	return true;
}

/* ---- the partition ---- */

static int
gpt_find(uint32_t lun)
{
	vm_offset_t d = ufs_dma + UFS_DMA_DATA;
	uint64_t blocks, ent;
	uint32_t bs, count, size;

	if (!capacity(lun, &blocks, &bs) || !read_lu(lun, 1, 1, bs)) {
		return -1;
	}
	if (memcmp((const void *)d, "EFI PART", 8) != 0) {
		return -1;
	}
	ent = (uint64_t)get32(d + 76) << 32 | get32(d + 72);
	count = get32(d + 80);
	size = get32(d + 84);
	if (size != 128 || count == 0 || count > 256 || !read_lu(lun, ent, (count * size + bs - 1) / bs, bs)) {
		return -1;
	}
	for (uint32_t i = 0; i < count; i++) {
		const volatile uint8_t *e = (const volatile uint8_t *)(d + i * size);
		size_t k;
		uint64_t first, last;

		for (k = 0; UFS_PART[k] && e[56 + 2 * k] == (uint8_t)UFS_PART[k] && e[57 + 2 * k] == 0; k++) {
		}
		if (UFS_PART[k] != 0 || e[56 + 2 * k] != 0 || e[57 + 2 * k] != 0) {
			continue;
		}
		first = (uint64_t)get32(d + i * size + 36) << 32 | get32(d + i * size + 32);
		last = (uint64_t)get32(d + i * size + 44) << 32 | get32(d + i * size + 40);
		if (first == 0 || last < first) {
			return 0;
		}
		ufs_lun = lun;
		ufs_bs = bs;
		ufs_lba = first;
		ufs_lu_blocks = last - first + 1;
		return 1;
	}
	return 0;
}

static bool
find_partition(void)
{
	uint32_t lun;
	uint8_t sb[64];
	uint64_t at;

	if (PE_parse_boot_argn("ufs_lun", &lun, sizeof(lun)) && lun < 8 && gpt_find(lun) == 1) {
		/* where the loader found it */
	} else {
		for (lun = 0; lun < 8 && gpt_find(lun) != 1; lun++) {
		}
		if (lun == 8) {
			printf("ufs: no " UFS_PART " partition in any LU's GPT (%s %#x)\n", ufs_failed ? ufs_failed : "-", ufs_failed_status);
			return false;
		}
	}
	/* as long as its APFS containers, back to back, as the eMMC driver and the loader measure it */
	for (at = 0; at < ufs_lu_blocks * ufs_bs;) {
		uint32_t bs;
		uint64_t n;

		if (at % ufs_bs || !read_part(at, sizeof(sb), sb) || memcmp(sb + 32, "NXSB", 4) != 0) {
			break;
		}
		memcpy(&bs, sb + 36, 4);
		memcpy(&n, sb + 40, 8);
		if (bs < 512 || bs > 65536 || n == 0 || n * bs > ufs_lu_blocks * ufs_bs - at) {
			break;
		}
		at += n * bs;
	}
	if (at == 0) {
		printf("ufs: no APFS container in " UFS_PART " (tools/system-flash.sh writes it)\n");
		return false;
	}
	ufs_bytes = at & ~511ull;
	printf("ufs: " UFS_PART " on LU %u at block %llu (%u-byte blocks), %llu MB of %llu MB in its APFS containers\n",
	    ufs_lun, ufs_lba, ufs_bs, ufs_bytes >> 20, (ufs_lu_blocks * ufs_bs) >> 20);
	return true;
}

/* ---- the devices ---- */

static int
ufs_open(__unused dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	return (flags & FWRITE) ? EROFS : 0;
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
ufs_strategy(buf_t bp)
{
	uint64_t off = (uint64_t)buf_blkno(bp) * 512;
	uint32_t count = buf_count(bp), done = 0;
	uint8_t *data = (uint8_t *)buf_dataptr(bp);
	int error = 0;

	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;                      /* this device never writes */
		goto out;
	}
	if (off >= ufs_bytes) {
		buf_setresid(bp, count);
		error = off == ufs_bytes ? 0 : EINVAL;
		goto out;
	}
	if (count > ufs_bytes - off) {
		count = (uint32_t)(ufs_bytes - off);
	}
	lck_mtx_lock(&ufs_lock);
	while (done < count) {
		uint32_t n = count - done;

		if (n > UFS_MAX_BYTES - ufs_bs) {
			n = UFS_MAX_BYTES - ufs_bs;
		}
		if (!read_part(off + done, n, data + done)) {
			error = EIO;
			break;
		}
		done += n;
	}
	lck_mtx_unlock(&ufs_lock);
	buf_setresid(bp, buf_count(bp) - done);
	if (error) {
		printf("ufs: byte %llu, %u bytes: %s (%#x)\n", off, count, ufs_failed, ufs_failed_status);
	}
out:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == 512 ? 0 : EINVAL;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = ufs_bs;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = ufs_bytes / 512;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = (UFS_MAX_BYTES - 4096) / 512;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_size(__unused dev_t dev)
{
	return 512;
}

static int
ufs_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(ufs_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))ufs_size, uio, 512);
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_size,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_open,
	.d_close      = ufs_close,
	.d_read       = ufs_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = ufs_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

int
redwood_ufs_root(dev_t *root)
{
	uint64_t t0, ns;
	uint8_t *probe;

	if (ufs_bmajor >= 0) {
		*root = makedev(ufs_bmajor, 0);
		return 0;
	}
	if (!attach()) {
		printf("ufs: %s (%#x)\n", ufs_failed, ufs_failed_status);
		return ENXIO;
	}
	printf("ufs: request list %#llx, slot %u, window %#llx\n", ufs_list_pa, ufs_slot, ufs_dma_pa);
	if (!find_partition()) {
		return ENXIO;
	}
	/* how fast: the first 4 MB, which the root's mount reads anyway */
	probe = kalloc_data(UFS_MAX_BYTES, Z_WAITOK);
	if (probe != NULL) {
		t0 = mach_absolute_time();
		for (uint64_t at = 0; at < (4u << 20) && at + UFS_MAX_BYTES / 2 <= ufs_bytes; at += UFS_MAX_BYTES / 2) {
			if (!read_part(at, UFS_MAX_BYTES / 2, probe)) {
				break;
			}
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("ufs: 4 MB in %llu ms\n", ns / 1000000);
		kfree_data(probe, UFS_MAX_BYTES);
	}
	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	if (ufs_bmajor < 0 || (ufs_cmajor = cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor)) < 0) {
		printf("ufs: no room among the devices (%d, %d)\n", ufs_bmajor, ufs_cmajor);
		return ENXIO;
	}
	*root = makedev(ufs_bmajor, 0);
	return 0;
}

void
redwood_ufs_devfs(void)
{
	if (ufs_bmajor < 0 || ufs_cmajor < 0) {
		return;
	}
	if (devfs_make_node(makedev(ufs_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufs0") == NULL ||
	    devfs_make_node(makedev(ufs_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufs0") == NULL) {
		printf("ufs: no /dev nodes\n");
	}
}

/* sysctl kern.redwood.ufs: how much it has read, and its errors */
static int
ufs_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[160];

	snprintf(line, sizeof(line), "ufs: LU %u, %llu reads, %llu MB, %llu errors%s%s", ufs_lun, ufs_reads, ufs_read_bytes >> 20,
	    ufs_errors, ufs_failed ? "; last: " : "", ufs_failed ? ufs_failed : "");
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, ufs, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, ufs_sysctl, "A",
    "the UFS's reads so far");
