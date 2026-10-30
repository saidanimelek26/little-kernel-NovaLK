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
#include <boot_menu.h>

#define SPM_BASE		0x10006000
#define INFRA_BASE		0x10001000
#define RGU_BASE		0x10007000
#define PINMUX_BASE		0x10005000
#define CONN_MCU_BASE		0x18002000
#define CONN_TOP_MISC_OFF_BASE	0x18070000

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
#define CONN_IP_VER_ALT		(CONN_TOP_MISC_OFF_BASE + 0x08)
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
	u32 ip = 0, ip_alt = 0;
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
		ip_alt = RD(CONN_IP_VER_ALT);

		say(&r, "IP ver [+0x10]=%08x (want %08x)", ip, EXPECT_IP_VER);
		say(&r, "IP ver [+0x08]=%08x", ip_alt);
		say(&r, "HW id=%08x  FW id=%08x", RD(CONN_HW_ID), RD(CONN_FW_ID));
		say(&r, "mcu[+0x600]=%08x (want 1d1e)", RD(CONN_VER_ID_0x600));

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
