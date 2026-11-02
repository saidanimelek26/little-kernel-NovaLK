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
#define CONN_HIF_ON_BASE	0x18007000
#define MCU_CPUPCR		(CONN_HIF_ON_BASE + 0x104)
#define CONN_HIF_PDMA_AXI_RREADY	(CONN_HIF_ON_BASE + 0x154)

/* time line of the MCU after release (stage 2 observation run) */
#define OBS_MAX			24
struct obs {
	u32 v600[OBS_MAX];
	u32 pc[OBS_MAX];
	int n;
};
#define PATCH_HDR_SIZE		48	/* RE verified from mtk_wcn_soc_rom_patch_dwn: sub r0, r0, #0x30 */
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

	/* MCU view 0xf0xxxxxx: vendor driver always masks with 0x00ffffff */
	off = addr_word & 0x00ffffff;
	body = len - PATCH_HDR_SIZE;

	if (off + body >= EMI_SIZE) {
		say(r, "%s: too big for EMI (%x+%x)", name, off, body);
		return -1;
	}

	memcpy((void *)(unsigned long)(emi_base + off), img + PATCH_HDR_SIZE,
	       body);
	memcpy((void *)(unsigned long)(emi_base + buildtime_off), img,
	       BUILDTIME_SIZE);

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

/* PMIC XO buffer n (1,2,3,4,6,7): software mode, output on. */
static void xo_get(int n, u32 *mode, u32 *en)
{
	switch (n) {
	case 1: *mode = pmic_get_register_value(PMIC_XO_EXTBUF1_MODE);
		*en = pmic_get_register_value(PMIC_XO_EXTBUF1_EN_M); break;
	case 2: *mode = pmic_get_register_value(PMIC_XO_EXTBUF2_MODE);
		*en = pmic_get_register_value(PMIC_XO_EXTBUF2_EN_M); break;
	case 3: *mode = pmic_get_register_value(PMIC_XO_EXTBUF3_MODE);
		*en = pmic_get_register_value(PMIC_XO_EXTBUF3_EN_M); break;
	case 4: *mode = pmic_get_register_value(PMIC_XO_EXTBUF4_MODE);
		*en = pmic_get_register_value(PMIC_XO_EXTBUF4_EN_M); break;
	case 6: *mode = pmic_get_register_value(PMIC_XO_EXTBUF6_MODE);
		*en = pmic_get_register_value(PMIC_XO_EXTBUF6_EN_M); break;
	default: *mode = pmic_get_register_value(PMIC_XO_EXTBUF7_MODE);
		*en = pmic_get_register_value(PMIC_XO_EXTBUF7_EN_M); break;
	}
}

static void xo_set(int n, u32 mode, u32 en)
{
	switch (n) {
	case 1: pmic_set_register_value(PMIC_XO_EXTBUF1_MODE, mode);
		pmic_set_register_value(PMIC_XO_EXTBUF1_EN_M, en); break;
	case 2: pmic_set_register_value(PMIC_XO_EXTBUF2_MODE, mode);
		pmic_set_register_value(PMIC_XO_EXTBUF2_EN_M, en); break;
	case 3: pmic_set_register_value(PMIC_XO_EXTBUF3_MODE, mode);
		pmic_set_register_value(PMIC_XO_EXTBUF3_EN_M, en); break;
	case 4: pmic_set_register_value(PMIC_XO_EXTBUF4_MODE, mode);
		pmic_set_register_value(PMIC_XO_EXTBUF4_EN_M, en); break;
	case 6: pmic_set_register_value(PMIC_XO_EXTBUF6_MODE, mode);
		pmic_set_register_value(PMIC_XO_EXTBUF6_EN_M, en); break;
	default: pmic_set_register_value(PMIC_XO_EXTBUF7_MODE, mode);
		pmic_set_register_value(PMIC_XO_EXTBUF7_EN_M, en); break;
	}
}

/*
 * One complete power-up / MCU start / power-down cycle.
 *   xo       0 = leave the clock buffers alone, else the PMIC buffer
 *            (1,2,3,4,6,7) that is forced on in software mode
 *   mode     patch offset reading, see off_mode
 * *v600 / *v16c receive the last MCU status words.  Returns 1 when the
 * MCU reached its ready id.
 */
static int stage2_cycle(struct report *r, int xo, int mode, u32 *v600,
			u32 *v16c, struct obs *o, void (*hook)(void *), void *harg)
{
	u32 sv_mode = 0, sv_en = 0;
	u32 ip = 0;
	u32 v;
	int ready = 0;
	int i;

	off_mode = mode;
	*v600 = 0;
	*v16c = 0;

	memset((void *)(unsigned long)emi_base, 0, EMI_SIZE);

	/*
	 * consys_hw_vcn18_ctrl(): VCN18, and VCN33_BT unless an external LDO
	 * is used.  Then, in co-clock mode, the VCN28 hardware mode is turned
	 * off and mtk_wcn_consys_hw_efuse_paldo_ctrl() switches VCN28 on.
	 */
	pmic_set_register_value(PMIC_RG_LDO_VCN18_EN, 1);
	pmic_set_register_value(PMIC_RG_LDO_VCN33_EN_0, 1);
	if_pinmux(1);
	udelay(150);
	pmic_set_register_value(PMIC_RG_LDO_VCN28_HW0_OP_EN, 0);
	pmic_set_register_value(PMIC_RG_LDO_VCN28_HW0_OP_CFG, 0);
	pmic_set_register_value(PMIC_RG_LDO_VCN28_EN, 1);

	if (xo) {
		xo_get(xo, &sv_mode, &sv_en);
		xo_set(xo, 0, 1);	/* software mode, on */
	}
	udelay(150);

	WR(RGU_CPU_SW_RST, RD(RGU_CPU_SW_RST) | CPU_SW_RST_BIT | RST_KEY);

	if (conn_domain_on(r) == 0) {
		WR(RGU_CPU_SW_RST, (RD(RGU_CPU_SW_RST) & ~SW_RST_BIT) | RST_KEY);
		conn_bus_prot_release(r);
		udelay(10);

		for (i = 0; i < 10; i++) {
			ip = RD(CONN_IP_VER);
			if (ip == EXPECT_IP_VER)
				break;
			mdelay(20);
		}

		if (ip == EXPECT_IP_VER) {
			int saved = r->n;

			install_patch(r, "mcu ", consys_patch_mcu,
				      consys_patch_mcu_end, BUILDTIME_OFF_MCU);
			install_patch(r, "wifi", consys_patch_wifi,
				      consys_patch_wifi_end,
				      BUILDTIME_OFF_WIFI);
			r->n = saved;	/* keep the report short */

			arch_clean_invalidate_cache_range((addr_t)emi_base,
							  EMI_SIZE);

			v = RD(EMI_REMAP_REG);
			WR(EMI_REMAP_REG, (v & 0xfffff000) |
			   (u32)((emi_base >> 24) & 0xfff));
			v = RD(EMI_PERI_REMAP_REG);
			WR(EMI_PERI_REMAP_REG, (v & 0xf000ffff) |
			   (((0x10003000 >> 24) & 0xfff) << 16));

			v = RD(MCU_ROM_DELSEL);
			WR(MCU_ROM_DELSEL, (v & 0xffff0000) | 0x17);
			v = RD(COCLOCK_ACK_REG);
			WR(COCLOCK_ACK_REG, v & ~1u);
			WR(MCU_SW_IRQ, RD(MCU_SW_IRQ) | EMI_CTRL_VALUE);

			/*
			 * MT6765 vendor consys_set_pdma_axi_rready_force_high(1):
			 * assert bit 1 of 0x18007154 so PDMA AXI transactions do not stall.
			 */
			WR(CONN_HIF_PDMA_AXI_RREADY, RD(CONN_HIF_PDMA_AXI_RREADY) | (1u << 1));

			WR(RGU_CPU_SW_RST,
			   (RD(RGU_CPU_SW_RST) & ~CPU_SW_RST_BIT) | RST_KEY);

			/* Clear PDMA AXI force high once MCU is released */
			WR(CONN_HIF_PDMA_AXI_RREADY, RD(CONN_HIF_PDMA_AXI_RREADY) & ~(1u << 1));

			for (i = 0; i < 11; i++) {
				*v600 = RD(CONN_VER_ID_0x600);
				if (*v600 == MCU_READY_ID) {
					ready = 1;
					break;
				}
				mdelay(20);
			}
			*v16c = RD(MCU_CPU_STATE);

			/* observation: 100 ms steps for about two seconds */
			for (i = 0; o && !ready && i < OBS_MAX; i++) {
				o->v600[i] = RD(CONN_VER_ID_0x600);
				o->pc[i] = RD(MCU_CPUPCR);
				o->n = i + 1;
				if (o->v600[i] == MCU_READY_ID) {
					ready = 1;
					*v600 = o->v600[i];
					break;
				}
				mdelay(100);
			}

			/* stage 3: talk to the running MCU while it is up */
			if (hook) {
				mdelay(300);
				hook(harg);
			}
		} else {
			*v600 = 0xdead0000 | (ip & 0xffff);
		}

		WR(RGU_CPU_SW_RST, RD(RGU_CPU_SW_RST) | CPU_SW_RST_BIT | RST_KEY);
		conn_bus_prot_set(r);
		conn_domain_off(r);
	}

	if (xo)
		xo_set(xo, sv_mode, sv_en);	/* put the buffer back */
	if_pinmux(0);
	pmic_set_register_value(PMIC_RG_LDO_VCN28_EN, 0);
	pmic_set_register_value(PMIC_RG_LDO_VCN33_EN_0, 0);
	pmic_set_register_value(PMIC_RG_LDO_VCN18_EN, 0);

	return ready;
}

/*
 * Stage 2: two quick tries (patch offsets "aligned" and "raw"), then one
 * observation run that follows the MCU for about two seconds and prints how
 * its status word (+0x600) and program counter change with time.
 */
int novalk_consys_stage2(char (*lines)[LINE_MAX_CH], int max, int *count)
{
	struct report r;
	struct obs o;
	u32 v600, v16c;
	u32 last600 = 0xffffffff, lastpc = 0xffffffff;
	int ready = 0;
	int shown = 0;
	int m, i;

	r.line = lines;
	r.max = max;
	r.n = 0;
	memset(&o, 0, sizeof(o));

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
	say(&r, "EMI block %08x +4MB", (u32)emi_base);

	for (m = 0; m < 2 && !ready; m++) {
		struct report quiet = r;
		char scratch[8][LINE_MAX_CH];

		quiet.line = scratch;
		quiet.max = 8;
		quiet.n = 0;

		ready = stage2_cycle(&quiet, 0, m, &v600, &v16c, NULL, NULL, NULL);
		say(&r, "try %s: 600=%04x 16c=%x%s", m ? "raw" : "aln",
		    v600 & 0xffff, v16c & 0xffff, ready ? " READY" : "");
	}

	if (!ready) {
		struct report quiet = r;
		char scratch[8][LINE_MAX_CH];

		quiet.line = scratch;
		quiet.max = 8;
		quiet.n = 0;

		/* the raw reading wrote last, repeat the aligned one */
		ready = stage2_cycle(&quiet, 0, 0, &v600, &v16c, &o, NULL, NULL);

		say(&r, "observe %d samples (100 ms apart):", o.n);
		for (i = 0; i < o.n && shown < 9; i++) {
			if (o.v600[i] == last600 && o.pc[i] == lastpc &&
			    i != o.n - 1)
				continue;
			say(&r, " t%02d 600=%04x pc=%08x", i,
			    o.v600[i] & 0xffff, o.pc[i]);
			last600 = o.v600[i];
			lastpc = o.pc[i];
			shown++;
		}
	}

	{
		/* distinct program counter values seen during observation */
		int distinct = 0;
		int j, k;

		for (j = 0; j < o.n; j++) {
			for (k = 0; k < j; k++)
				if (o.pc[k] == o.pc[j])
					break;
			if (k == j)
				distinct++;
		}
		if (!ready && distinct >= 3) {
			say(&r, "PC visited %d addresses: MCU RUNS", distinct);
			ready = 1;
		}
	}

	say(&r, ready ? "RESULT: MCU RUNNING" : "RESULT: MCU NOT READY");
	*count = r.n;

	return ready;
}


/* ================================================================== */
/* Stage 3: BTIF + STP + WMT, first words with the running MCU         */
/* ================================================================== */
/*
 * BTIF is the serial-like link between the AP and the MCU.  This uses plain
 * register polling (PIO), no DMA and no interrupts.  Register layout and the
 * init order are those of drivers/misc/mediatek/btif/common/btif_plat.c
 * (hal_btif_hw_init, hal_btif_send_data, hal_btif_raise_wak_sig).
 *
 * STP frames, from stp_core.c (stp_send_data_no_ps):
 *   MAND mode : 80 | (type<<4)|(len>>8) | len&ff | 00 | payload | 00 00
 *   FULL mode : (80 + seq<<3 + ack) | same | (b0+b1+b2)&ff | payload | crc16
 * The probe sends the WMT command "query STP" ({01 04 01 00 04}, type 0) in
 * both framings and prints whatever comes back.
 */

#define BTIF_BASE_ADDR		0x1100c000	/* same as BTIF_BASE of the platform */
#define BTIF_RBR		(BTIF_BASE_ADDR + 0x00)
#define BTIF_THR		(BTIF_BASE_ADDR + 0x00)
#define BTIF_IER		(BTIF_BASE_ADDR + 0x04)
#define BTIF_FIFOCTRL		(BTIF_BASE_ADDR + 0x08)
#define BTIF_FAKELCR		(BTIF_BASE_ADDR + 0x0c)
#define BTIF_LSR		(BTIF_BASE_ADDR + 0x14)
#define BTIF_DMA_EN		(BTIF_BASE_ADDR + 0x4c)
#define BTIF_TRI_LVL		(BTIF_BASE_ADDR + 0x60)
#define BTIF_WAK		(BTIF_BASE_ADDR + 0x64)
#define BTIF_HANDSHAKE		(BTIF_BASE_ADDR + 0x6c)
#define BTIF_LSR_DR		(1u << 0)
#define BTIF_LSR_THRE		(1u << 5)
#define INFRA_CG2_CLR		(INFRA_BASE + 0x84)	/* IFR2 clear register */
#define INFRA_CG2_BTIF		(1u << 31)

#define STP_TYPE_BT		0
#define STP_TYPE_FM		1
#define STP_TYPE_GPS		2
#define STP_TYPE_WIFI		3
#define STP_TYPE_WMT		4	/* WMT_TASK_INDX (Wireless Management Task) */

static u16 crc16_arc(const u8 *p, u32 n)
{
	u32 crc = 0;
	u32 i;
	int b;

	for (i = 0; i < n; i++) {
		crc ^= p[i];
		for (b = 0; b < 8; b++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
	}

	return (u16)crc;
}

static void btif_init(void)
{
	WR(INFRA_CG2_CLR, INFRA_CG2_BTIF);	/* ungate the BTIF clock */
	udelay(20);

	WR(BTIF_FAKELCR, 0);			/* normal mode */
	WR(BTIF_HANDSHAKE, RD(BTIF_HANDSHAKE) | 1);	/* new handshake */

	WR(BTIF_FIFOCTRL, 0x02);		/* clear RX fifo */
	WR(BTIF_FIFOCTRL, 0x00);
	WR(BTIF_FIFOCTRL, 0x04);		/* clear TX fifo */
	WR(BTIF_FIFOCTRL, 0x00);

	WR(BTIF_TRI_LVL, (8u & 0xf) | ((1u & 0x7) << 4));
	WR(BTIF_DMA_EN, (RD(BTIF_DMA_EN) & ~3u) | (1u << 2));	/* PIO */
	WR(BTIF_IER, 0);			/* polling only */
}

/* wake-up pulse for the MCU side of the link */
static void btif_wake(void)
{
	WR(BTIF_WAK, RD(BTIF_WAK) & ~1u);
	udelay(150);
	WR(BTIF_WAK, RD(BTIF_WAK) | 1u);
	udelay(300);
}

static int btif_tx(const u8 *p, int n)
{
	int i;
	int us;

	for (i = 0; i < n; i++) {
		for (us = 0; us < 20000; us++) {
			if (RD(BTIF_LSR) & BTIF_LSR_THRE)
				break;
			udelay(1);
		}
		if (us >= 20000)
			return i;
		WR(BTIF_THR, p[i]);
	}

	return n;
}

static int btif_rx(u8 *buf, int max, int ms)
{
	int n = 0;
	int t;

	for (t = 0; t < ms * 10 && n < max; t++) {
		if (RD(BTIF_LSR) & BTIF_LSR_DR)
			buf[n++] = (u8)RD(BTIF_RBR);
		else
			udelay(100);
	}

	return n;
}

struct stage3_arg {
	struct report *r;
	int replies;
};

static void hexline(struct report *r, const char *tag, const u8 *b, int n)
{
	char s[LINE_MAX_CH];
	int i;
	int o = 0;

	o += snprintf(s + o, sizeof(s) - o, "%s", tag);
	for (i = 0; i < n && o < (int)sizeof(s) - 4; i++)
		o += snprintf(s + o, sizeof(s) - o, "%02x ", b[i]);
	say(r, "%s", s);
}

static void stage3_probe(void *argp)
{
	struct stage3_arg *a = argp;
	static const u8 wmt_cmd[5] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	u8 frame[24];
	u8 rx[24];
	int mode;
	int len;
	int sent;
	int got;

	btif_init();
	say(a->r, "BTIF lsr=%02x hs=%x tri=%02x", RD(BTIF_LSR),
	    RD(BTIF_HANDSHAKE) & 0xff, RD(BTIF_TRI_LVL) & 0xff);

	for (mode = 0; mode < 2; mode++) {
		u16 crc;

		len = 0;
		if (mode == 0) {
			/* MAND framing */
			frame[len++] = 0x80;
			frame[len++] = (STP_TYPE_WMT << 4) | 0x00;
			frame[len++] = sizeof(wmt_cmd);
			frame[len++] = 0x00;
			memcpy(frame + len, wmt_cmd, sizeof(wmt_cmd));
			len += sizeof(wmt_cmd);
			frame[len++] = 0x00;
			frame[len++] = 0x00;
		} else {
			/* FULL framing, sequence 0, ack 0 */
			frame[len++] = 0x80;
			frame[len++] = (STP_TYPE_WMT << 4) | 0x00;
			frame[len++] = sizeof(wmt_cmd);
			frame[len++] = (u8)(frame[0] + frame[1] + frame[2]);
			memcpy(frame + len, wmt_cmd, sizeof(wmt_cmd));
			len += sizeof(wmt_cmd);
			crc = crc16_arc(wmt_cmd, sizeof(wmt_cmd));
			frame[len++] = (u8)(crc & 0xff);
			frame[len++] = (u8)(crc >> 8);
		}

		btif_wake();
		(void)btif_rx(rx, sizeof(rx), 5);	/* drain old bytes */

		sent = btif_tx(frame, len);
		got = btif_rx(rx, sizeof(rx), 400);

		say(a->r, "%s tx %d/%d rx %d lsr=%02x", mode ? "FULL" : "MAND",
		    sent, len, got, RD(BTIF_LSR));
		if (got) {
			hexline(a->r, " ", rx, got > 12 ? 12 : got);
			a->replies++;
		}
	}
}

/*
 * Stage 3: power the block, load the ROM patches, start the MCU (exactly as
 * stage 2), then run the BTIF/STP/WMT probe while it is up.
 */
int novalk_consys_stage3(char (*lines)[LINE_MAX_CH], int max, int *count)
{
	struct report r;
	struct stage3_arg a;
	u32 v600, v16c;

	r.line = lines;
	r.max = max;
	r.n = 0;
	a.r = &r;
	a.replies = 0;

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
	say(&r, "EMI block %08x +4MB", (u32)emi_base);

	{
		struct report quiet = r;
		char scratch[8][LINE_MAX_CH];

		quiet.line = scratch;
		quiet.max = 8;
		quiet.n = 0;

		(void)stage2_cycle(&quiet, 0, 0, &v600, &v16c, NULL,
				   stage3_probe, &a);
	}

	say(&r, "mcu 600=%04x", v600 & 0xffff);
	say(&r, a.replies ? "RESULT: MCU ANSWERED" : "RESULT: NO ANSWER YET");
	*count = r.n;

	return a.replies ? 1 : 0;
}


/* ================================================================== */
/* Stage 4: Wi-Fi Core Power-On (WMT FUNC_ON command)                 */
/* ================================================================== */
/*
 * Reverse-engineered from wmt_drv.ko (wmt_core_func_ctrl_cmd):
 * Opcode: 0x06 (WMT_CMD_FUNC_CTRL)
 * Payload: { 0x03 (Wi-Fi), 0x01 (ON) }
 * Full WMT Packet: { 0x01, 0x06, 0x02, 0x00, 0x03, 0x01 }
 * Sent via STP Type 4 (STP_TYPE_WMT).
 * Expected MCU event response: { 0x02, 0x06, 0x01, 0x00, 0x00 } (0x00 = SUCCESS)
 */
struct stage4_arg {
	struct report *r;
	int ok;
};

static void stage4_probe(void *argp)
{
	struct stage4_arg *a = argp;
	static const u8 wmt_wifi_on[6] = { 0x01, 0x06, 0x02, 0x00, 0x03, 0x01 };
	u8 frame[24];
	u8 rx[32];
	int mode;
	int len;
	int sent;
	int got;
	int i;

	btif_init();
	say(a->r, "BTIF ready, sending WMT FUNC_ON (Wi-Fi)...");

	for (mode = 0; mode < 2 && !a->ok; mode++) {
		u16 crc;
		len = 0;
		if (mode == 0) {
			/* MAND framing */
			frame[len++] = 0x80;
			frame[len++] = (STP_TYPE_WMT << 4) | 0x00;
			frame[len++] = sizeof(wmt_wifi_on);
			frame[len++] = 0x00;
			memcpy(frame + len, wmt_wifi_on, sizeof(wmt_wifi_on));
			len += sizeof(wmt_wifi_on);
			frame[len++] = 0x00;
			frame[len++] = 0x00;
		} else {
			/* FULL framing */
			frame[len++] = 0x80;
			frame[len++] = (STP_TYPE_WMT << 4) | 0x00;
			frame[len++] = sizeof(wmt_wifi_on);
			frame[len++] = (u8)(frame[0] + frame[1] + frame[2]);
			memcpy(frame + len, wmt_wifi_on, sizeof(wmt_wifi_on));
			len += sizeof(wmt_wifi_on);
			crc = crc16_arc(wmt_wifi_on, sizeof(wmt_wifi_on));
			frame[len++] = (u8)(crc & 0xff);
			frame[len++] = (u8)(crc >> 8);
		}

		btif_wake();
		(void)btif_rx(rx, sizeof(rx), 5);

		sent = btif_tx(frame, len);
		got = btif_rx(rx, sizeof(rx), 500);

		say(a->r, "%s tx %d/%d rx %d", mode ? "FULL" : "MAND", sent, len, got);
		if (got > 0) {
			hexline(a->r, " ", rx, got > 12 ? 12 : got);
			for (i = 0; i <= got - 5; i++) {
				if (rx[i] == 0x02 && rx[i + 1] == 0x06 &&
				    rx[i + 2] == 0x01 && rx[i + 3] == 0x00 &&
				    rx[i + 4] == 0x00) {
					a->ok = 1;
					say(a->r, "WMT Wi-Fi FUNC_ON: SUCCESS (0x00)");
					break;
				}
			}
		}
	}
}

int novalk_consys_stage4(char (*lines)[LINE_MAX_CH], int max, int *count)
{
	struct report r;
	struct stage4_arg a;
	u32 v600, v16c;

	r.line = lines;
	r.max = max;
	r.n = 0;
	a.r = &r;
	a.ok = 0;

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
	say(&r, "EMI block %08x +4MB", (u32)emi_base);

	{
		struct report quiet = r;
		char scratch[8][LINE_MAX_CH];

		quiet.line = scratch;
		quiet.max = 8;
		quiet.n = 0;

		(void)stage2_cycle(&quiet, 0, 1, &v600, &v16c, NULL,
				   stage4_probe, &a);
	}

	say(&r, "mcu 600=%04x", v600 & 0xffff);
	say(&r, a.ok ? "RESULT: WI-FI CORE POWERED ON" : "RESULT: FUNC_ON FAILED");
	*count = r.n;

	return a.ok;
}
