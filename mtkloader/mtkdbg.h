/*
 * Debug output for bare-metal code on the Hot 60 Pro (X6885, MT6789) that runs with the MMU off:
 *
 *  - plog_*: the ramoops console zone that Android's pstore keeps across a warm reset. What is
 *    written here shows up after the next Android boot as /sys/fs/pstore/console-ramoops-0.
 *    Where the zone is comes from LK's device tree (board.h). Header: sig "DBGC", start (next
 *    write), size.
 *  - fbcon_*: text on the panel. The panel is video mode (see PANEL_W below): OVL0 layer 0
 *    scans LK's framebuffer continuously, so pixels written there appear on the next frame.
 *    If LK enables per-pixel alpha on the layer, every pixel needs alpha 0xff (it is written so).
 *
 * Everything here is device memory (MMU off): no unaligned accesses, no SIMD, no dc zva.
 * Build with -mgeneral-regs-only -mstrict-align -fno-builtin.
 */
#ifndef MTKDBG_H
#define MTKDBG_H

#include <stdint.h>
#include "font8x16.h"

/*
 * Android's ramoops zones. Their place is the device tree's (board.h reads it out of LK's tree at
 * entry and sets these); until then, and on a phone whose tree has no ramoops, they point at a
 * buffer of our own, so that nothing is ever written to DRAM nobody said was ours.
 */
#define RAMOOPS_SIG          0x43474244u
#define RAMOOPS_SCRATCH      0x4000u
static uint8_t ramoops_scratch[2][RAMOOPS_SCRATCH] __attribute__((aligned(4096)));
static uint64_t ramoops_console = (uint64_t)ramoops_scratch[0];
static uint32_t ramoops_console_size = RAMOOPS_SCRATCH;
static uint64_t ramoops_pmsg = (uint64_t)ramoops_scratch[1];
static uint32_t ramoops_pmsg_size = RAMOOPS_SCRATCH;
#define RAMOOPS_CONSOLE      ramoops_console
#define RAMOOPS_CONSOLE_SIZE ramoops_console_size
/* the pmsg zone: Android's request for one XNU boot comes in it (boot-xnu.sh), the kernel's lines for the
 * host go out in it (kern.redwood.host_keep) */
#define RAMOOPS_PMSG         ramoops_pmsg
#define RAMOOPS_PMSG_SIZE    ramoops_pmsg_size

#define TOPRGU_BASE          0x10007000ULL      /* the same on the MT6789 (mediatek,mt6789-wdt) */
#define TOPRGU_MODE          (TOPRGU_BASE + 0x00)
#define TOPRGU_LENGTH        (TOPRGU_BASE + 0x04)
#define TOPRGU_RESTART       (TOPRGU_BASE + 0x08)
#define TOPRGU_SWRST         (TOPRGU_BASE + 0x14)
#define TOPRGU_MODE_KEY      0x22000000u
#define TOPRGU_RESTART_KEY   0x1971u
#define TOPRGU_SWRST_KEY     0x1209u

/*
 * The panel: the X6885's AMOLED, 1224 x 2720, a Novatek NT37706A or a Visionox VTDR6126A
 * (Android's panel drivers for this phone are nt37706a_fhdp_dsi_vdo_dsc_vxn_vxn_144hz_x6885 and
 * vtdr6126a_fhdp_dsi_vdo_dsc_tm_tm_144hz_x6885): video mode, with DSC after the OVL. So, as on
 * the Hot 20, OVL0 keeps scanning LK's framebuffer and what is written there is shown on the next
 * frame. The size is read back from OVL0's ROI (display_find); these are what it should say.
 */
#define PANEL_W_DEFAULT      1224u
#define PANEL_H_DEFAULT      2720u
static uint32_t panel_w = PANEL_W_DEFAULT, panel_h = PANEL_H_DEFAULT;
#define PANEL_W              panel_w
#define PANEL_H              panel_h

/* MT6789 display blocks (Android's tree: disp_mutex@14001000, disp_ovl0@14005000, dsi@14013000) */
#define DISP_MUTEX_BASE      0x14001000ULL
#define DISP_MUTEX_EN        (DISP_MUTEX_BASE + 0x20)
#define DISP_MUTEX_RUN       (DISP_MUTEX_BASE + 0x24)

#define OVL0_BASE_DEFAULT    0x14005000ULL
static uint64_t ovl0_base = OVL0_BASE_DEFAULT;
#define OVL0_BASE            ovl0_base
#define OVL_STA              0x000
#define OVL_EN               0x00c
#define OVL_ROI_SIZE         0x020      /* height << 16 | width */
#define OVL_SRC_CON          0x02c      /* a bit per layer the hardware composes */
#define OVL_L0_CON           0x030
#define OVL_L0_SRC_SIZE      0x038
#define OVL_L0_PITCH         0x044
#define OVL_L0_ADDR          0xf40

#define DSI0_BASE_DEFAULT    0x14013000ULL
#define DSI_START            0x000
#define DSI_MODE_CTRL        0x014      /* bits 1:0: 0 command mode, 1 sync pulse, 2 sync event, 3 burst video */

static inline uint32_t rd32(uint64_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* ---- time ---- */

static inline uint64_t dbg_ticks(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static inline uint64_t dbg_freq(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 13000000u;
}

static inline void dbg_delay_us(uint32_t us)
{
	uint64_t end = dbg_ticks() + (dbg_freq() / 1000000u) * us + 1;

	while (dbg_ticks() < end) {
	}
}

static inline void dbg_delay_ms(uint32_t ms)
{
	uint64_t end = dbg_ticks() + dbg_freq() / 1000 * ms;
	while (dbg_ticks() < end) {
	}
}

/* ---- watchdog ---- */

static inline void wdt_disable(void)
{
	wr32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
	wr32(TOPRGU_MODE, TOPRGU_MODE_KEY);
}

/* A warm reset, as Linux's mtk_wdt_restart does it: DRAM (and so pstore) survives. */
static inline void __attribute__((noreturn)) wdt_reset(void)
{
	wr32(TOPRGU_SWRST, TOPRGU_SWRST_KEY);
	for (;;) {
		__asm__ volatile("wfe");
	}
}

/* ---- ramoops console ---- */

struct ramoops_hdr {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
};

#define PLOG_HDR  ((volatile struct ramoops_hdr *)RAMOOPS_CONSOLE)
#define PLOG_DATA ((volatile char *)(RAMOOPS_CONSOLE + sizeof(struct ramoops_hdr)))
#define PLOG_CAP  (RAMOOPS_CONSOLE_SIZE - sizeof(struct ramoops_hdr))

static inline void plog_reset(void)
{
	PLOG_HDR->sig = RAMOOPS_SIG;
	PLOG_HDR->start = 0;
	PLOG_HDR->size = 0;
}

static inline void plog_putc(char c)
{
	uint32_t at = PLOG_HDR->start, size = PLOG_HDR->size;

	if (PLOG_HDR->sig != RAMOOPS_SIG || at >= PLOG_CAP || size > PLOG_CAP) {
		plog_reset();
		at = size = 0;
	}
	PLOG_DATA[at] = c;
	if (++at == PLOG_CAP) {
		at = 0;
	}
	if (size < PLOG_CAP) {
		size++;
	}
	PLOG_HDR->start = at;
	PLOG_HDR->size = size;
}

/* ---- framebuffer text ---- */

struct fbcon {
	uint64_t base;
	uint32_t pitch, w, h;
	uint32_t x, y, top, scale;
	uint32_t fg, bg;
};

static struct fbcon fbc;

static inline void fb_px(uint32_t x, uint32_t y, uint32_t c)
{
	*(volatile uint32_t *)(fbc.base + (uint64_t)y * fbc.pitch + (uint64_t)x * 4) = c;
}

static inline void fb_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	if (!fbc.base) {
		return;
	}
	for (uint32_t y = y0; y < y0 + h && y < fbc.h; y++) {
		for (uint32_t x = x0; x < x0 + w && x < fbc.w; x++) {
			fb_px(x, y, c);
		}
	}
}

static inline void fbcon_init(uint64_t base, uint32_t pitch, uint32_t top, uint32_t scale)
{
	fbc.base = base;
	fbc.pitch = pitch;
	fbc.w = PANEL_W;
	fbc.h = PANEL_H;
	fbc.x = 0;
	fbc.y = fbc.top = top;
	fbc.scale = scale;
	fbc.fg = 0xffffffffu;
	fbc.bg = 0xff000000u;
}

static inline void fbcon_newline(void)
{
	uint32_t lh = 16 * fbc.scale;

	fbc.x = 0;
	fbc.y += lh;
	if (fbc.y + lh > fbc.h) {
		fbc.y = fbc.top;
	}
	/* clear the line about to be written, and a marker line under it */
	fb_rect(0, fbc.y, fbc.w, lh, fbc.bg);
	if (fbc.y + 2 * lh <= fbc.h) {
		fb_rect(0, fbc.y + lh, fbc.w, 2, 0xffff3030u);
	}
}

static inline void fbcon_putc(char c)
{
	uint32_t cw = 8 * fbc.scale;

	if (!fbc.base) {
		return;
	}
	if (c == '\n') {
		fbcon_newline();
		return;
	}
	if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) {
		c = '?';
	}
	if (fbc.x + cw > fbc.w) {
		fbcon_newline();
	}
	const unsigned char *g = font8x16[(unsigned char)c - 0x20];
	for (uint32_t row = 0; row < 16 * fbc.scale; row++) {
		unsigned char bits = g[row / fbc.scale];
		for (uint32_t col = 0; col < cw; col++) {
			fb_px(fbc.x + col, fbc.y + row, (bits & (0x80 >> (col / fbc.scale))) ? fbc.fg : fbc.bg);
		}
	}
	fbc.x += cw;
}

/* ---- both ---- */

static inline void dbg_putc(char c)
{
	plog_putc(c);
	fbcon_putc(c);
}

static inline void dbg_puts(const char *s)
{
	while (*s) {
		dbg_putc(*s++);
	}
}

static inline void dbg_hex(uint64_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";
	for (int i = (digits - 1) * 4; i >= 0; i -= 4) {
		dbg_putc(hex[(v >> i) & 0xf]);
	}
}

static inline void dbg_dec(uint64_t v)
{
	char buf[21];
	int n = 0;
	do {
		buf[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n) {
		dbg_putc(buf[--n]);
	}
}

/* "name+off: w0 w1 w2 w3" lines for a register block; only the log gets these when quiet. */
static inline void dbg_dump(const char *name, uint64_t base, uint32_t from, uint32_t to, int quiet)
{
	uint64_t fb = fbc.base;

	if (quiet) {
		fbc.base = 0;
	}
	for (uint32_t off = from; off < to; off += 16) {
		dbg_puts(name);
		dbg_putc('+');
		dbg_hex(off, 3);
		dbg_putc(':');
		for (uint32_t i = 0; i < 16 && off + i < to; i += 4) {
			dbg_putc(' ');
			dbg_hex(rd32(base + off + i), 8);
		}
		dbg_putc('\n');
	}
	fbc.base = fb;
}

#endif /* MTKDBG_H */
