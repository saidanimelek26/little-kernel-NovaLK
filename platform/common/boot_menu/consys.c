/*
 * NovaOS Wi-Fi, stage 1: power up the connectivity block (CONNSYS) of the
 * MT6765 and read its identification registers.
 *
 * This is an EXPERIMENT driven by an explicit button in the Wi-Fi app.  It
 * does only what is needed to answer "is the block alive?" and then powers
 * it down again.  Every wait is bounded, and every step leaves a line of
 * text that the app shows on screen, because there is no other log.
 *
 * Sources (all from MediaTek's open connectivity modules for MT6765 and the
 * Realme C21 kernel):
 *   common_main/platform/mt6765.c            consys_hw_*, polling_consys_chipid
 *   common_main/platform/include/mt6765.h    register offsets and values
 *   drivers/clk/mediatek/clk-mt6765-pg.c     CONN power domain sequence
 *
 * What is deliberately NOT done yet, because the sources do not show it
 * well enough to copy blindly:
 *   - the XO_WCN clock buffer (co-clock mode, WMT_SOC.cfg co_clock_flag=1)
 *   - the VCN33 (Wi-Fi/BT) rails
 *   - EMI region mapping / MPU, ROM patch download, CPU release
 * If the ID registers do not answer, the report says which readings were
 * seen, and that tells the next step what to add.
 */

#include <stdarg.h>
#include <string.h>
#include <printf.h>
#include <platform/mt_typedefs.h>
#include <platform/mt_reg_base.h>
#include <platform/sync_write.h>
#include <platform/timer.h>
#include <platform/upmu_common.h>
#include <platform/boot_mode.h>
#include <arch/ops.h>
#include <mblock.h>
#include <boot_menu.h>

#define SPM_BASE		0x10006000
#define INFRA_BASE		0x10001000
#define RGU_BASE		0x10007000
#define PINMUX_BASE		0x10005000
#define CONN_MCU_BASE		0x18002000
#define CONN_TOP_MISC_OFF_BASE	0x180b1000	/* DT: CONN_TOP_MISC_OFF_BASE */

#define SPM_POWERON_CONFIG_EN	(SPM_BASE + 0x000)
#define SPM_PWR_STATUS		(SPM_BASE + 0x180)
#define SPM_PWR_STATUS_2ND	(SPM_BASE + 0x184)
#define SPM_CONN_PWR_CON	(SPM_BASE + 0x32c)

#define PWR_RST_B		(1u << 0)
#define PWR_ISO			(1u << 1)
#define PWR_ON			(1u << 2)
#define PWR_ON_2ND		(1u << 3)
#define PWR_CLK_DIS		(1u << 4)
#define CONN_PWR_STA		(1u << 1)

#define INFRA_PROT_EN		(INFRA_BASE + 0x220)
#define INFRA_PROT_STA1		(INFRA_BASE + 0x228)
#define INFRA_PROT_SET		(INFRA_BASE + 0x2a0)
#define INFRA_PROT_CLR		(INFRA_BASE + 0x2a4)
#define INFRA_PROT1_EN		(INFRA_BASE + 0x250)
#define INFRA_PROT1_STA1	(INFRA_BASE + 0x258)
#define INFRA_PROT1_SET		(INFRA_BASE + 0x2a8)
#define INFRA_PROT1_CLR		(INFRA_BASE + 0x2ac)

#define CONN_PROT_STEP1_0	(1u << 13)
#define CONN_PROT_STEP1_1	(1u << 18)
#define CONN_PROT_STEP2_0	((1u << 14) | (1u << 16))

#define RGU_CPU_SW_RST		(RGU_BASE + 0x018)
#define CPU_SW_RST_BIT		(1u << 12)
#define SW_RST_BIT		(1u << 9)
#define RST_KEY			(0x88u << 24)

#define CONN_IP_VER		(CONN_TOP_MISC_OFF_BASE + 0x10)
#define CONN_CONF_ID		(CONN_TOP_MISC_OFF_BASE + 0x1c)
#define CONN_HW_ID		(CONN_MCU_BASE + 0x000)
#define CONN_FW_ID		(CONN_MCU_BASE + 0x004)
#define CONN_VER_ID_0x600	(CONN_MCU_BASE + 0x600)
#define EXPECT_IP_VER		0x10020300

#define RD(a)		(*(volatile u32 *)(a))
#define WR(a, v)	mt_reg_sync_writel((u32)(v), (volatile unsigned int *)(a))

#define LINE_MAX_CH	46

struct report {
	char (*line)[LINE_MAX_CH];
	int max;
	int n;
};

static void say(struct report *r, const char *fmt, ...)
{
	va_list ap;

	if (r->n >= r->max)
		return;

	va_start(ap, fmt);
	vsnprintf(r->line[r->n], LINE_MAX_CH, fmt, ap);
	va_end(ap);
	r->n++;
}

/* Wait until (RD(addr) & mask) == want, at most us microseconds. */
static int wait_bits(u32 addr, u32 mask, u32 want, int us)
{
	while (us-- > 0) {
		if ((RD(addr) & mask) == want)
			return 0;
		udelay(1);
	}

	return (RD(addr) & mask) == want ? 0 : -1;
}

/* clk-mt6765-pg.c: spm_mtcmos_ctrl_conn_pwr(STA_POWER_ON) */
static int conn_domain_on(struct report *r)
{
	WR(SPM_POWERON_CONFIG_EN, (RD(SPM_POWERON_CONFIG_EN) & 0x0000ffff) |
	   0x0b160001);

	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) | PWR_ON);
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) | PWR_ON_2ND);

	if (wait_bits(SPM_PWR_STATUS, CONN_PWR_STA, CONN_PWR_STA, 20000) ||
	    wait_bits(SPM_PWR_STATUS_2ND, CONN_PWR_STA, CONN_PWR_STA, 20000)) {
		say(r, "CONN domain: TIMEOUT %08x/%08x", RD(SPM_PWR_STATUS),
		    RD(SPM_PWR_STATUS_2ND));
		return -1;
	}

	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) & ~PWR_CLK_DIS);
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) & ~PWR_ISO);
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) | PWR_RST_B);

	say(r, "CONN domain: ON  pwrcon=%08x", RD(SPM_CONN_PWR_CON));

	return 0;
}

/* spm_mtcmos_ctrl_conn_pwr(STA_POWER_DOWN) */
static void conn_domain_off(struct report *r)
{
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) | PWR_ISO);
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) | PWR_CLK_DIS);
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) & ~PWR_RST_B);
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) & ~PWR_ON);
	WR(SPM_CONN_PWR_CON, RD(SPM_CONN_PWR_CON) & ~PWR_ON_2ND);

	if (wait_bits(SPM_PWR_STATUS, CONN_PWR_STA, 0, 20000))
		say(r, "CONN domain off: status %08x", RD(SPM_PWR_STATUS));
}

/* spm_mtcmos_ctrl_conn_bus_prot(STA_POWER_ON): release the bus protection. */
static void conn_bus_prot_release(struct report *r)
{
	WR(INFRA_PROT_CLR, CONN_PROT_STEP2_0);
	WR(INFRA_PROT_CLR, CONN_PROT_STEP1_0);
	WR(INFRA_PROT1_CLR, CONN_PROT_STEP1_1);

	say(r, "bus prot: en %08x/%08x", RD(INFRA_PROT_EN), RD(INFRA_PROT1_EN));
}

/* spm_mtcmos_ctrl_conn_bus_prot(STA_POWER_DOWN) with bounded waits. */
static void conn_bus_prot_set(struct report *r)
{
	WR(INFRA_PROT_SET, CONN_PROT_STEP1_0);
	wait_bits(INFRA_PROT_STA1, CONN_PROT_STEP1_0, CONN_PROT_STEP1_0, 5000);
	WR(INFRA_PROT1_SET, CONN_PROT_STEP1_1);
	wait_bits(INFRA_PROT1_STA1, CONN_PROT_STEP1_1, CONN_PROT_STEP1_1, 5000);
	WR(INFRA_PROT_SET, CONN_PROT_STEP2_0);
	wait_bits(INFRA_PROT_STA1, CONN_PROT_STEP2_0, CONN_PROT_STEP2_0, 5000);
	(void)r;
}

/* consys_set_if_pinmux(ENABLE): mt6765.h CONSYS_IF_PINMUX_* values. */
static void if_pinmux(int on)
{
	u32 a = PINMUX_BASE + 0x3f0;
	u32 b = PINMUX_BASE + 0x3e0;

	if (on) {
		WR(a, (RD(a) & 0xffffff88) | 0x00000011);
		WR(b, (RD(b) & 0x8888888f) | 0x11111110);
	} else {
		WR(a, RD(a) & 0xffffff88);
		WR(b, RD(b) & 0x8888888f);
	}
}

/*
 * Run the whole experiment.  lines[] receives the report; the return value
 * is 1 when the expected IP version was read, 0 otherwise.
 */
int novalk_consys_probe(char (*lines)[LINE_MAX_CH], int max, int *count)
{
	struct report r;
	u32 ip = 0;
	int alive = 0;
	int i;

	r.line = lines;
	r.max = max;
	r.n = 0;

	say(&r, "before: pwr %08x/%08x", RD(SPM_PWR_STATUS),
	    RD(SPM_PWR_STATUS_2ND));

	/* 1. VCN18 rail (consys_hw_vcn18_ctrl) */
	pmic_set_register_value(PMIC_RG_LDO_VCN18_EN, 1);
	say(&r, "VCN18 en=%d", pmic_get_register_value(PMIC_RG_LDO_VCN18_EN));

	/* 2. interface pinmux */
	if_pinmux(1);
	udelay(150);

	/* 3. hold the MCU in reset (consys_hw_reset_bit_set(ENABLE)) */
	WR(RGU_CPU_SW_RST, RD(RGU_CPU_SW_RST) | CPU_SW_RST_BIT | RST_KEY);

	/* 4. power domain, then release the AHB/AXI bus protection */
	if (conn_domain_on(&r) == 0) {
		WR(RGU_CPU_SW_RST, (RD(RGU_CPU_SW_RST) & ~SW_RST_BIT) | RST_KEY);
		conn_bus_prot_release(&r);
		udelay(10);

		/* 5. polling_consys_chipid(): 10 tries, 20 ms apart */
		for (i = 0; i < 10; i++) {
			ip = RD(CONN_IP_VER);
			if (ip == EXPECT_IP_VER) {
				alive = 1;
				break;
			}
			mdelay(20);
		}

		say(&r, "IP ver [+0x10]=%08x (want %08x)", ip, EXPECT_IP_VER);
		say(&r, "conf id [+0x1c]=%08x", RD(CONN_CONF_ID));
		say(&r, "HW id=%04x  FW id=%04x", RD(CONN_HW_ID) & 0xffff,
		    RD(CONN_FW_ID) & 0xffff);
		say(&r, "mcu[+0x600]=%08x", RD(CONN_VER_ID_0x600));

		/* 6. put everything back */
		conn_bus_prot_set(&r);
		conn_domain_off(&r);
	}

	if_pinmux(0);
	pmic_set_register_value(PMIC_RG_LDO_VCN18_EN, 0);

	say(&r, "after:  pwr %08x/%08x", RD(SPM_PWR_STATUS),
	    RD(SPM_PWR_STATUS_2ND));
	say(&r, alive ? "RESULT: CONNSYS ANSWERS" : "RESULT: NO VALID ID");

	*count = r.n;

	return alive;
}


/* ================================================================== */
/* Stage 2: ROM patches into the shared DRAM block, release the MCU    */
/* ================================================================== */
/*
 * Order of operations follows mtk_wcn_consys_hw_reg_ctrl() and
 * polling_consys_chipid() of the MT6765 module:
 *
 *   rails + pinmux -> hold MCU in reset -> power domain on -> read IP id
 *   -> ROM patches into the DRAM block -> ROM delsel, co-clock ack,
 *   SW IRQ bits -> release the MCU -> wait for mcu[+0x600] == 0x1d1e
 *
 * The DRAM block is what the kernel calls "consys-reserve-memory": 4 MiB,
 * 16 MiB aligned, below 3 GiB; the MCU sees it at 0xf0000000 through the
 * remap register at infracfg + 0x380.
 *
 * NOT done: the XO_WCN clock buffer (the MT6765 clkbuf code is not in the
 * open kernel sources).  The state of the PMIC XO buffers is therefore
 * only dumped, so a failure here shows what is missing.
 */

extern const u8 consys_patch_mcu[], consys_patch_mcu_end[];
extern const u8 consys_patch_wifi[], consys_patch_wifi_end[];

#define EMI_SIZE		0x400000
#define EMI_ALIGN		0x1000000
#define EMI_LIMIT		0xc0000000ULL
#define EMI_REMAP_REG		(INFRA_BASE + 0x380)
#define EMI_PERI_REMAP_REG	(INFRA_BASE + 0x388)
#define MCU_ROM_DELSEL		(CONN_MCU_BASE + 0x114)
#define MCU_SW_IRQ		(CONN_MCU_BASE + 0x150)
#define MCU_CPU_STATE		(CONN_MCU_BASE + 0x16c)
#define EMI_CTRL_VALUE		((1u << 0) | (1u << 15) | (1u << 21))
#define COCLOCK_ACK_REG		(0x180c1200 + 0x4)
#define MCU_READY_ID		0x1d1e
#define PATCH_HDR_SIZE		32
#define BUILDTIME_SIZE		16
#define BUILDTIME_OFF_WIFI	0x68310
#define BUILDTIME_OFF_MCU	0x68320

static u64 emi_base;		/* reserved once per boot */
/*
 * The EMI address word of a patch header looks like 0xf0140011.  The kernel
 * adds up its three low bytes (so +0x11), but the layout of the block is
 * clearly built on 64 KB steps.  Which one the MCU expects is not visible in
 * the sources, so every run of stage 2 alternates between the two readings
 * and says which one it used.
 */
static int off_mode;		/* 0: aligned (0x140000)  1: raw (0x140011) */

/* One patch image: strip the header, copy to its EMI offset. */
static int install_patch(struct report *r, const char *name, const u8 *img,
			 const u8 *end, u32 buildtime_off)
{
	u32 len = (u32)(end - img);
	u32 addr_word;
	u32 type;
	u32 off;
	u32 body;

	if (len <= PATCH_HDR_SIZE || img[16] != 'A' || img[17] != 'L' ||
	    img[18] != 'P' || img[19] != 'S') {
		say(r, "%s: bad header", name);
		return -1;
	}

	addr_word = img[24] | (img[25] << 8) | (img[26] << 16) |
		    ((u32)img[27] << 24);
	type = img[28] | (img[29] << 8) | (img[30] << 16) | ((u32)img[31] << 24);

	/* MCU view 0xf0xxxxxx */
	off = off_mode ? (addr_word & 0x00ffffff) : (addr_word & 0x00ffff00);
	body = len - PATCH_HDR_SIZE;

	if (off + body >= EMI_SIZE) {
		say(r, "%s: too big for EMI (%x+%x)", name, off, body);
		return -1;
	}

	memcpy((void *)(unsigned long)(emi_base + off), img + PATCH_HDR_SIZE,
	       body);
	memcpy((void *)(unsigned long)(emi_base + buildtime_off), img,
	       BUILDTIME_SIZE - 1);

	say(r, "%s: type %u -> EMI+%06x  %u KB", name, type, off, body / 1024);

	return 0;
}

static void xo_dump(struct report *r)
{
	say(r, "XO en_m 1:%d 2:%d 3:%d 4:%d 6:%d 7:%d",
	    pmic_get_register_value(PMIC_XO_EXTBUF1_EN_M),
	    pmic_get_register_value(PMIC_XO_EXTBUF2_EN_M),
	    pmic_get_register_value(PMIC_XO_EXTBUF3_EN_M),
	    pmic_get_register_value(PMIC_XO_EXTBUF4_EN_M),
	    pmic_get_register_value(PMIC_XO_EXTBUF6_EN_M),
	    pmic_get_register_value(PMIC_XO_EXTBUF7_EN_M));
	say(r, "XO mode 1:%d 2:%d 3:%d 4:%d 6:%d 7:%d",
	    pmic_get_register_value(PMIC_XO_EXTBUF1_MODE),
	    pmic_get_register_value(PMIC_XO_EXTBUF2_MODE),
	    pmic_get_register_value(PMIC_XO_EXTBUF3_MODE),
	    pmic_get_register_value(PMIC_XO_EXTBUF4_MODE),
	    pmic_get_register_value(PMIC_XO_EXTBUF6_MODE),
	    pmic_get_register_value(PMIC_XO_EXTBUF7_MODE));
}

int novalk_consys_stage2(char (*lines)[LINE_MAX_CH], int max, int *count)
{
	struct report r;
	u32 ip = 0;
	u32 v;
	int ready = 0;
	int i;

	r.line = lines;
	r.max = max;
	r.n = 0;

	/* 1. the shared DRAM block (reserved once, kept for later runs) */
	if (!emi_base)
		emi_base = mblock_reserve_ext(&g_boot_arg->mblock_info, EMI_SIZE,
					      EMI_ALIGN, EMI_LIMIT, 0,
					      "novalk-consys");
	if (!emi_base || emi_base < 0x40000000ULL ||
	    emi_base + EMI_SIZE > EMI_LIMIT) {
		say(&r, "EMI block: FAILED (%x)", (u32)emi_base);
		emi_base = 0;
		*count = r.n;
		return 0;
	}
	say(&r, "EMI block %08x +4MB  offsets:%s", (u32)emi_base,
	    off_mode ? "raw" : "aligned");

	memset((void *)(unsigned long)emi_base, 0, EMI_SIZE);

	/* 2. rails, pinmux; co-clock mode = VCN28 hardware mode off */
	pmic_set_register_value(PMIC_RG_LDO_VCN18_EN, 1);
	pmic_set_register_value(PMIC_RG_LDO_VCN28_HW0_OP_EN, 0);
	pmic_set_register_value(PMIC_RG_LDO_VCN28_HW0_OP_CFG, 0);
	if_pinmux(1);
	udelay(150);
	xo_dump(&r);

	/* 3. hold the MCU, power domain, bus */
	WR(RGU_CPU_SW_RST, RD(RGU_CPU_SW_RST) | CPU_SW_RST_BIT | RST_KEY);

	if (conn_domain_on(&r) == 0) {
		WR(RGU_CPU_SW_RST, (RD(RGU_CPU_SW_RST) & ~SW_RST_BIT) | RST_KEY);
		conn_bus_prot_release(&r);
		udelay(10);

		for (i = 0; i < 10; i++) {
			ip = RD(CONN_IP_VER);
			if (ip == EXPECT_IP_VER)
				break;
			mdelay(20);
		}
		say(&r, "IP ver %08x %s", ip, ip == EXPECT_IP_VER ? "ok" : "BAD");

		if (ip == EXPECT_IP_VER) {
			/* 4. patches, remap, the rest of polling_consys_chipid */
			install_patch(&r, "mcu ", consys_patch_mcu,
				      consys_patch_mcu_end,
				      BUILDTIME_OFF_MCU);
			install_patch(&r, "wifi", consys_patch_wifi,
				      consys_patch_wifi_end,
				      BUILDTIME_OFF_WIFI);
			arch_clean_invalidate_cache_range(
				(addr_t)emi_base, EMI_SIZE);

			v = RD(EMI_REMAP_REG);
			WR(EMI_REMAP_REG, (v & 0xfffff000) |
			   (u32)((emi_base >> 24) & 0xfff));
			v = RD(EMI_PERI_REMAP_REG);
			WR(EMI_PERI_REMAP_REG, (v & 0xf000ffff) |
			   (((0x10003000 >> 24) & 0xfff) << 16));
			say(&r, "remap %08x/%08x", RD(EMI_REMAP_REG),
			    RD(EMI_PERI_REMAP_REG));

			v = RD(MCU_ROM_DELSEL);
			WR(MCU_ROM_DELSEL, (v & 0xffff0000) | 0x17);

			v = RD(COCLOCK_ACK_REG);
			WR(COCLOCK_ACK_REG, v & ~1u);

			WR(MCU_SW_IRQ, RD(MCU_SW_IRQ) | EMI_CTRL_VALUE);

			/* 5. release the MCU and wait for its ready id */
			WR(RGU_CPU_SW_RST,
			   (RD(RGU_CPU_SW_RST) & ~CPU_SW_RST_BIT) | RST_KEY);

			for (i = 0; i < 11; i++) {
				v = RD(CONN_VER_ID_0x600);
				if (v == MCU_READY_ID) {
					ready = 1;
					break;
				}
				mdelay(20);
			}
			say(&r, "mcu[+0x600]=%04x (want %04x)", v & 0xffff,
			    MCU_READY_ID);
			say(&r, "mcu state[+0x16c]=%08x", RD(MCU_CPU_STATE));
			say(&r, "EMI head %08x %08x",
			    *(volatile u32 *)(unsigned long)(emi_base + 0x68320),
			    *(volatile u32 *)(unsigned long)(emi_base + 0x68000));
		}

		/* 6. stop the MCU again and power everything down */
		WR(RGU_CPU_SW_RST, RD(RGU_CPU_SW_RST) | CPU_SW_RST_BIT | RST_KEY);
		conn_bus_prot_set(&r);
		conn_domain_off(&r);
	}

	if_pinmux(0);
	pmic_set_register_value(PMIC_RG_LDO_VCN18_EN, 0);

	say(&r, ready ? "RESULT: MCU RUNNING" : "RESULT: MCU NOT READY");
	*count = r.n;
	off_mode ^= 1;	/* next run uses the other reading */

	return ready;
}
