/*
 * What the loader needs to know of the phone, read out of the device tree LK hands over (x0 at entry)
 * instead of the addresses the Hot 20 had written in: where DRAM is, what in it is spoken for
 * (reserved-memory, /memreserve/), where Android's ramoops zones are, the UFS host, the display
 * blocks. Then every range of DRAM this program means to write is checked against that map before
 * the first write: a range that lies outside DRAM or on anything reserved is refused, said in the
 * log, and the run does not happen (board_ok). Reads only.
 *
 * The Hot 60 Pro (X6885, MT6789), as its own device tree has it (a recovery tree's prebuilt dtb):
 * GICv3 at 0x0c000000/0x0c040000, UART0 0x11002000 and TOPRGU 0x10007000 as on the MT6768, the PMIC
 * wrapper at 0x10026000 (an MT6366, the MT6358's sibling), UFS at 0x11270000 (mediatek,mt8183-ufshci),
 * MSDC0 0x11230000 and MSDC1 (microSD) 0x11240000, OVL0 0x14005000, DSI0 0x14013000, the Mali-G57 at
 * 0x13000000. The memory map itself is filled in by LK at boot, so it is only known from x0.
 *
 * Linux's ramoops (fs/pstore/ram.c) lays its zones out in this order: the dump records
 * (count = dump_size / record-size of dump_size / count bytes each), console, pmsg, ftrace.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#define FDT_MAGIC_BE        0xd00dfeedu
#define FDT_BEGIN_NODE      1
#define FDT_END_NODE        2
#define FDT_PROP            3
#define FDT_NOP             4
#define FDT_END             9

#define BOARD_MAX_MEM       16
#define BOARD_MAX_RSV       96
#define BOARD_NAME_LEN      32

struct board_range {
	uint64_t base, size;
	char name[BOARD_NAME_LEN];
};

struct board_info {
	int dt_ok;
	uint32_t nmem, nrsv;
	struct board_range mem[BOARD_MAX_MEM];
	struct board_range rsv[BOARD_MAX_RSV];
	/* ramoops, if the tree has it */
	uint64_t ramoops_base, ramoops_size;
	uint32_t record_size, console_size, pmsg_size, ftrace_size, ecc_size;
	uint64_t console_pa, pmsg_pa;
	/* blocks found by name or compatible (0: not in the tree; the defaults stay) */
	uint64_t ufs_base, ovl0_base, dsi0_base, mutex_base, mali_base, msdc1_base, keypad_base, pwrap_base;
	char bootargs[256];
};

static struct board_info board;

static inline uint32_t fdt_be32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline uint64_t fdt_be64(const volatile uint8_t *p)
{
	return (uint64_t)fdt_be32(p) << 32 | fdt_be32(p + 4);
}

static int board_streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int board_prefix(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/* Does a compatible list (NUL-separated strings) hold this one? */
static int board_compat(const volatile uint8_t *v, uint32_t len, const char *want)
{
	uint32_t at = 0;

	while (at < len) {
		uint32_t k = 0;
		while (want[k] && at + k < len && v[at + k] == (uint8_t)want[k]) {
			k++;
		}
		if (!want[k] && at + k < len && v[at + k] == 0) {
			return 1;
		}
		while (at < len && v[at]) {
			at++;
		}
		at++;
	}
	return 0;
}

static void board_copy_name(char *dst, const char *src)
{
	int i = 0;
	for (; i < BOARD_NAME_LEN - 1 && src[i]; i++) {
		dst[i] = src[i];
	}
	dst[i] = 0;
}

static void board_add(struct board_range *list, uint32_t *n, uint32_t max, uint64_t base, uint64_t size, const char *name)
{
	if (*n >= max || size == 0) {
		return;
	}
	list[*n].base = base;
	list[*n].size = size;
	board_copy_name(list[*n].name, name);
	(*n)++;
}

/* A node's interesting properties, gathered until its end, then acted on. */
struct board_node {
	const char *name;
	const volatile uint8_t *reg, *compat;
	uint32_t reg_len, compat_len;
	uint32_t record, console, pmsg, ftrace, ecc;
};

/* #address-cells = #size-cells = 2 everywhere on this SoC's tree (root, soc, reserved-memory). */
static uint64_t board_reg0(const struct board_node *n, uint64_t *size)
{
	if (!n->reg || n->reg_len < 16) {
		*size = 0;
		return 0;
	}
	*size = fdt_be64(n->reg + 8);
	return fdt_be64(n->reg);
}

static void board_node_done(const struct board_node *n, int depth, int in_rsv, int in_memory)
{
	uint64_t size, base = board_reg0(n, &size);

	if (in_memory && n->reg) {
		for (uint32_t at = 0; at + 16 <= n->reg_len; at += 16) {
			board_add(board.mem, &board.nmem, BOARD_MAX_MEM, fdt_be64(n->reg + at), fdt_be64(n->reg + at + 8), n->name);
		}
		return;
	}
	if (in_rsv && depth == 2) {
		for (uint32_t at = 0; n->reg && at + 16 <= n->reg_len; at += 16) {
			board_add(board.rsv, &board.nrsv, BOARD_MAX_RSV, fdt_be64(n->reg + at), fdt_be64(n->reg + at + 8), n->name);
		}
		if ((n->compat && board_compat(n->compat, n->compat_len, "ramoops")) || board_prefix(n->name, "ramoops")) {
			board.ramoops_base = base;
			board.ramoops_size = size;
			board.record_size = n->record;
			board.console_size = n->console;
			board.pmsg_size = n->pmsg;
			board.ftrace_size = n->ftrace;
			board.ecc_size = n->ecc;
		}
		return;
	}
	if (!n->reg || !size) {
		return;
	}
	if (board_prefix(n->name, "ufshci@") || (n->compat && board_compat(n->compat, n->compat_len, "mediatek,mt8183-ufshci"))) {
		board.ufs_base = base;
	} else if (board_prefix(n->name, "disp_ovl0@")) {
		board.ovl0_base = base;
	} else if (board_prefix(n->name, "dsi@") && !board.dsi0_base) {
		board.dsi0_base = base;
	} else if (board_prefix(n->name, "disp_mutex@")) {
		board.mutex_base = base;
	} else if (board_prefix(n->name, "mali@")) {
		board.mali_base = base;
	} else if (board_prefix(n->name, "mmc@") && n->compat && board_compat(n->compat, n->compat_len, "mediatek,mt6789-mmc") &&
	    base != 0x11230000ULL) {
		board.msdc1_base = base;
	} else if (board_prefix(n->name, "kp@")) {
		board.keypad_base = base;
	} else if (board_prefix(n->name, "pwrap@")) {
		board.pwrap_base = base;
	}
}

/* Linux's arithmetic for the zones (ramoops_probe, ramoops_init_przs). */
static void board_ramoops_zones(void)
{
	uint64_t dump, used = 0;

	if (!board.ramoops_base || board.ecc_size) {
		board.console_pa = board.pmsg_pa = 0;
		return;
	}
	dump = board.ramoops_size - board.console_size - board.ftrace_size - board.pmsg_size;
	if (dump > board.ramoops_size) {
		return; /* the sizes do not fit: leave ramoops alone */
	}
	if (board.record_size && dump >= board.record_size) {
		uint64_t cnt = dump / board.record_size;
		used = dump / cnt * cnt;
	}
	board.console_pa = board.console_size ? board.ramoops_base + used : 0;
	board.pmsg_pa = board.pmsg_size ? board.ramoops_base + used + board.console_size : 0;
}

/*
 * Without a ramoops node, the kernel's command line may carry it instead (ramoops.mem_address=,
 * ramoops.mem_size=, ramoops.console_size=, ramoops.pmsg_size=, ramoops.record_size=).
 */
static uint64_t board_arg(const char *key)
{
	const char *s = board.bootargs;

	for (; *s; s++) {
		if ((s == board.bootargs || s[-1] == ' ') && board_prefix(s, key)) {
			const char *v = s;
			uint64_t x = 0;
			while (*v && *v != '=') {
				v++;
			}
			if (*v++ != '=') {
				return 0;
			}
			int hex = v[0] == '0' && (v[1] == 'x' || v[1] == 'X');
			for (v += hex ? 2 : 0; *v && *v != ' '; v++) {
				uint32_t d = (*v >= '0' && *v <= '9') ? (uint32_t)(*v - '0') :
				    (*v >= 'a' && *v <= 'f') ? (uint32_t)(*v - 'a' + 10) :
				    (*v >= 'A' && *v <= 'F') ? (uint32_t)(*v - 'A' + 10) : 99;
				if (d >= (hex ? 16u : 10u)) {
					break;
				}
				x = x * (hex ? 16 : 10) + d;
			}
			return x;
		}
	}
	return 0;
}

static int board_read(uint64_t dtb)
{
	const volatile uint8_t *fdt = (const volatile uint8_t *)dtb;
	struct board_node stack[8];
	int depth = 0, in_rsv = 0, in_memory = 0;

	board.dt_ok = 0;
	board.nmem = board.nrsv = 0;
	if (!dtb || (dtb & 3) || fdt_be32(fdt) != FDT_MAGIC_BE) {
		return 0;
	}
	uint32_t total = fdt_be32(fdt + 4), off_struct = fdt_be32(fdt + 8), off_strings = fdt_be32(fdt + 12),
	    off_rsvmap = fdt_be32(fdt + 16), size_struct = fdt_be32(fdt + 36);
	if (total > (16u << 20) || off_struct >= total || off_strings >= total || off_rsvmap >= total) {
		return 0;
	}
	for (const volatile uint8_t *r = fdt + off_rsvmap; r + 16 <= fdt + total; r += 16) {
		uint64_t a = fdt_be64(r), s = fdt_be64(r + 8);
		if (!a && !s) {
			break;
		}
		board_add(board.rsv, &board.nrsv, BOARD_MAX_RSV, a, s, "/memreserve/");
	}

	const volatile uint8_t *p = fdt + off_struct, *end = p + size_struct;
	const char *strings = (const char *)(fdt + off_strings);
	while (p + 4 <= end) {
		uint32_t tok = fdt_be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			uint32_t n = 0;
			while (p[n]) {
				n++;
			}
			p += (n + 4) & ~3u;
			if (depth < 8) {
				struct board_node *nd = &stack[depth];
				nd->name = name;
				nd->reg = nd->compat = 0;
				nd->reg_len = nd->compat_len = 0;
				nd->record = nd->console = nd->pmsg = nd->ftrace = nd->ecc = 0;
			}
			depth++;
			if (depth == 2) {
				in_rsv = board_streq(name, "reserved-memory");
				in_memory = board_prefix(name, "memory") && (name[6] == 0 || name[6] == '@');
			}
		} else if (tok == FDT_END_NODE) {
			if (depth <= 0) {
				return 0;
			}
			if (depth <= 8) {
				board_node_done(&stack[depth - 1], depth - 1, in_rsv, in_memory && depth == 2);
			}
			depth--;
			if (depth == 1) {
				in_rsv = in_memory = 0;
			}
		} else if (tok == FDT_PROP) {
			uint32_t len = fdt_be32(p), nameoff = fdt_be32(p + 4);
			const volatile uint8_t *v = p + 8;
			const char *pname = strings + nameoff;
			p += 8 + ((len + 3) & ~3u);
			if (depth < 1 || depth > 8) {
				continue;
			}
			struct board_node *nd = &stack[depth - 1];
			if (board_streq(pname, "reg")) {
				nd->reg = v;
				nd->reg_len = len;
			} else if (board_streq(pname, "compatible")) {
				nd->compat = v;
				nd->compat_len = len;
			} else if (len == 4 && board_streq(pname, "record-size")) {
				nd->record = fdt_be32(v);
			} else if (len == 4 && board_streq(pname, "console-size")) {
				nd->console = fdt_be32(v);
			} else if (len == 4 && board_streq(pname, "pmsg-size")) {
				nd->pmsg = fdt_be32(v);
			} else if (len == 4 && board_streq(pname, "ftrace-size")) {
				nd->ftrace = fdt_be32(v);
			} else if (len == 4 && board_streq(pname, "ecc-size")) {
				nd->ecc = fdt_be32(v);
			} else if (depth == 2 && board_streq(nd->name, "chosen") && board_streq(pname, "bootargs")) {
				uint32_t k = 0;
				for (; k < len && k < sizeof(board.bootargs) - 1 && v[k]; k++) {
					board.bootargs[k] = (char)v[k];
				}
				board.bootargs[k] = 0;
			}
		} else if (tok == FDT_NOP) {
			continue;
		} else if (tok == FDT_END) {
			break;
		} else {
			return 0;
		}
	}
	if (!board.ramoops_base && board_arg("ramoops.mem_address")) {
		board.ramoops_base = board_arg("ramoops.mem_address");
		board.ramoops_size = board_arg("ramoops.mem_size");
		board.record_size = (uint32_t)board_arg("ramoops.record_size");
		board.console_size = (uint32_t)board_arg("ramoops.console_size");
		board.pmsg_size = (uint32_t)board_arg("ramoops.pmsg_size");
		board.ftrace_size = (uint32_t)board_arg("ramoops.ftrace_size");
	}
	board_ramoops_zones();
	board.dt_ok = board.nmem > 0;
	return board.dt_ok;
}

/* In DRAM (one bank holds all of it) and on nothing reserved; else the reserved range's index + 1, or -1. */
static int board_range_free(uint64_t base, uint64_t size, const char **why)
{
	int in_dram = 0;

	*why = 0;
	for (uint32_t i = 0; i < board.nmem; i++) {
		if (base >= board.mem[i].base && base + size <= board.mem[i].base + board.mem[i].size) {
			in_dram = 1;
		}
	}
	if (!in_dram) {
		*why = "not in DRAM";
		return -1;
	}
	for (uint32_t i = 0; i < board.nrsv; i++) {
		if (base < board.rsv[i].base + board.rsv[i].size && board.rsv[i].base < base + size) {
			*why = board.rsv[i].name;
			return (int)i + 1;
		}
	}
	return 0;
}

#endif /* BOARD_H */
