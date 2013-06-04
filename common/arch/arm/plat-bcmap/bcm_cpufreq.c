/*******************************************************************************
* Copyright 2010,2011 Broadcom Corporation.  All rights reserved.
*
*	@file	arch/arm/plat-bcmap/bcm_cpufreq.c
*
* Unless you and Broadcom execute a separate written software license agreement
* governing use of this software, this software is licensed to you under the
* terms of the GNU General Public License version 2, available at
* http://www.gnu.org/copyleft/gpl.html (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a license
* other than the GPL, without Broadcom's express prior written consent.
*******************************************************************************/

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/cpufreq.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <plat/bcm_cpufreq_drv.h>
#include <mach/reg_clkpwr.h>

/* Per-CPU private data */
struct bcm_cpufreq {
	struct clk *cpu_clk;
	struct clk *appspll_en_clk;
	struct cpufreq_frequency_table *bcm_freqs_table;
	struct regulator *cpu_regulator;
	struct cpufreq_policy *policy;
	struct bcm_cpufreq_drv_plat *plat;
};
static struct bcm_cpufreq *bcm_cpufreq;

/*********************************************************************
 *                             DEBUG CODE                            *
 *********************************************************************/

/* Enable pm driver debug code */
#define CONFIG_BCM215XX_PM_DEBUG

#ifdef CONFIG_BCM215XX_PM_DEBUG

/* CPUFREQ driver debug interface is accessed via:
 * /sys/module/bcm_cpufreq/parameters/debug
 */

/* Enable/disable debug logs */
enum {
	/* Disable all logging */
	CPUFREQ_DEBUG_DISABLE = 0U,
	/* Enable debug prints */
	CPUFREQ_DEBUG_FLOW = (1U << 0),
};

#define DEFAULT_LOG_LVL    CPUFREQ_DEBUG_FLOW

struct debug {
	int log_lvl;
};

/*
 * Sysfs interface for cpufreq_dbg structure.
 */
#define __param_check_debug(name, p, type) \
	static inline struct type *__check_##name(void) { return (p); }

#define param_check_debug(name, p) \
	__param_check_debug(name, p, debug)

static int param_set_debug(const char *val, struct kernel_param *kp);
static int param_get_debug(char *buffer, struct kernel_param *kp);

static struct debug debug = {
	.log_lvl = DEFAULT_LOG_LVL,
};
module_param_named(debug, debug, debug, S_IRUGO | S_IWUSR | S_IWGRP);

/* Helpers */
#define IS_FLOW_DBG_ENABLED (debug.log_lvl & CPUFREQ_DEBUG_FLOW)

/*
 * Command handlers
 */
static void cmd_show_stats(const char *p)
{
	pr_info("log_lvl: %x\n", debug.log_lvl);
}

static void cmd_set_log_lvl(const char *p)
{
	sscanf(p, "%x", &debug.log_lvl);
}

static void cmd_show_bogomips(const char *p)
{
/* re-used from init/calibrate.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */
#define LPS_PREC 8

	unsigned long lpj;
	unsigned long ticks, loopbit;
	int lps_precision = LPS_PREC;

	lpj = (1<<12);
	while ((lpj <<= 1) != 0) {
		/* wait for "start of" clock tick */
		ticks = jiffies;
		while (ticks == jiffies)
			/* nothing */;
		/* Go .. */
		ticks = jiffies;
		__delay(lpj);
		ticks = jiffies - ticks;
		if (ticks)
			break;
	}

	/*
	 * Do a binary approximation to get lpj set to
	 * equal one clock (up to lps_precision bits)
	 */
	lpj >>= 1;
	loopbit = lpj;
	while (lps_precision-- && (loopbit >>= 1)) {
		lpj |= loopbit;
		ticks = jiffies;
		while (ticks == jiffies)
			/* nothing */;
		ticks = jiffies;
		__delay(lpj);
		if (jiffies != ticks)	/* longer than 1 tick */
			lpj &= ~loopbit;
	}

	pr_info("%lu.%02lu BogoMIPS (lpj=%lu)\n", lpj/(500000/HZ),
		(lpj/(5000/HZ)) % 100, lpj);
}

/* List of commands supported */
enum {
	CMD_SHOW_STATS = 's',
	CMD_SHOW_BOGOMIPS = 'b',
	CMD_SET_LOG_LVL = 'l',
};

static void cmd_show_usage(void)
{
	const char usage[] = "Usage:\n"
	  "echo 'cmd string' > /sys/module/bcm_cpufreq/parameters/debug\n"
	  "'cmd string' must be constructed as follows:\n"
	  "Update log level: l 0x01\n"
	  "Show stats: s\n"
	  "Show bogomips: b\n";

	pr_info("%s", usage);
}

static int param_set_debug(const char *val, struct kernel_param *kp)
{
	const char *p;

	if (!val)
		return -EINVAL;

	/* Command is only one character followed by a space. Arguments,
	 * if any, starts from offset 2 in val.
	 */
	p = &val[2];

	switch (val[0]) {
	case CMD_SHOW_STATS:
		cmd_show_stats(p);
		break;
	case CMD_SHOW_BOGOMIPS:
		cmd_show_bogomips(p);
		break;
	case CMD_SET_LOG_LVL:
		cmd_set_log_lvl(p);
		break;
	default:
		cmd_show_usage();
		break;
	}
	return 0;
}

static int param_get_debug(char *buffer, struct kernel_param *kp)
{
	return 0;
}

#else /* CONFIG_BCM215XX_PM_DEBUG */

/* Helpers */
#define IS_FLOW_DBG_ENABLED (false)

#endif /* CONFIG_BCM215XX_PM_DEBUG */

/*********************************************************************
 *                   CPUFREQ TABLE MANIPULATION                      *
 *********************************************************************/

/* Create and populate cpu freqs table. The memory for the table must
 * be statically allocated.
 */
static int bcm_create_cpufreqs_table(struct cpufreq_policy *policy,
	struct cpufreq_frequency_table **table)
{
	struct bcm_cpufreq *b = &bcm_cpufreq[policy->cpu];
	struct cpufreq_frequency_table *t;
	struct bcm_cpu_info *info = NULL;
	int i, num;

	info = &b->plat->info[policy->cpu];
	num = info->num_freqs;
	t = kzalloc(sizeof(*t) * (num + 1), GFP_KERNEL);
	if (!t) {
		pr_info("%s: kzalloc failed for cpufreq table\n", __func__);
		return -ENOMEM;
	}
	*table = t;

	for (i = 0; i < num; i++) {
		t[i].index = i;
		t[i].frequency = info->freq_tbl[i].cpu_freq * 1000;
	}
	t[num].index = i;
	t[num].frequency = CPUFREQ_TABLE_END;

	return 0;
}

/* This function expects the frequency parameter to be in MHz and returns
 * the voltage in uV (suitable to be used with regulator consumer API).
 */
static int bcm_get_cpuvoltage(int cpu, int freq)
{
	struct bcm_cpufreq *b = &bcm_cpufreq[cpu];
	struct bcm_cpu_info *info = NULL;
	int i;
	int voltage = -EINVAL;

	info = &b->plat->info[cpu];
	for (i = 0; i < info->num_freqs; i++) {
		if (info->freq_tbl[i].cpu_freq == freq) {
			voltage = info->freq_tbl[i].cpu_voltage;
			break;
		}
	}

	return voltage;
}

/*********************************************************************
 *                       CPUFREQ CORE INTERFACE                      *
 *********************************************************************/

static unsigned int bcm_cpufreq_get_speed(unsigned int cpu)
{
	struct bcm_cpufreq *b = &bcm_cpufreq[cpu];
	unsigned int rate;

	/* cpufreq core expects clock frequency in kHz */
	rate = clk_get_rate(b->cpu_clk) / 1000;

	if (IS_FLOW_DBG_ENABLED)
		pr_debug("%s: cur cpu speed: %u\n", __func__, rate);

	return rate;
}

static int bcm_cpufreq_verify_speed(struct cpufreq_policy *policy)
{
	struct bcm_cpufreq *b = &bcm_cpufreq[policy->cpu];
	int ret = -EINVAL;

	if (b->bcm_freqs_table)
		ret = cpufreq_frequency_table_verify(policy,
			b->bcm_freqs_table);

	if (IS_FLOW_DBG_ENABLED) {
		pr_debug("%s: after cpufreq verify: min:%d->max:%d kHz\n",
			__func__, policy->min, policy->max);
	}

	return ret;
}

static int wait_for_pll_on(void)
{
	int ret = -EIO;
	int cnt = 10;

	/* Poll for PLL_ON state (CLK_DEBUG_MON2[7:4]==4b0101) */
	do {
		u32 val = readl(ADDR_CLKPWR_CLK_SYSCLK_DEBUG_MON2);
		val &= APPL_MON_APLL_SWITCH_STATE;
		val >>= APPL_MON_APLL_SWITCH_STATE_SHIFT;
		if (val == APLL_MON_PLL_ON) {
			ret = 0;
			break;
		}
		udelay(1);
	} while (cnt--);

	return ret;
}

static int bcm_cpufreq_set_speed(struct cpufreq_policy *policy,
	unsigned int target_freq,
	unsigned int relation)
{
	struct cpufreq_freqs freqs;
	struct bcm_cpufreq *b = &bcm_cpufreq[policy->cpu];
	struct bcm_cpu_info *info = &b->plat->info[policy->cpu];
	unsigned int freq_makwuz, index_makwuz; //add eve oc
	unsigned int freq_ngenos, index_ngenos; //add eve oc
	unsigned int freq_turbo, index_turbo;
	unsigned int freq_kenceng, index_kenceng; //add eve oc
	unsigned int freq_kebut, index_kebut; //add eve oc
	unsigned int freq_normal, index_normal; //add eve oc
	unsigned int freq_lelet, index_lelet; //add eve oc
	unsigned int freq_lemot, index_lemot; //add eve oc
	int activate = 0; //add eve oc
	int volt_new; //add eve oc
	int volt_old; //add eve oc
	int index;
	int ret;

	/* Lookup the next frequency */
	if (cpufreq_frequency_table_target(policy, b->bcm_freqs_table,
		target_freq, relation, &index)) {
		return -EINVAL;
	}

//	orig from here =======================================

/*	freqs.cpu = 0;
	freqs.old = bcm_cpufreq_get_speed(0);
	freqs.new = b->bcm_freqs_table[index].frequency;

	if (freqs.old == freqs.new)
		return 0;

	pr_info("%s: cpu freq change: %u --> %u\n", __func__, freqs.old,
		freqs.new);

	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	local_irq_disable();
*/
	/* If we are switching to a higher frequency, we may have to increase
	 * the core voltage first before changing the frequency.
	 */
//	if (freqs.new > freqs.old) {
		/* bcm_get_cpuvoltage expects frequency in MHz, cpufreq core
		 * gives the frequency in kHz. Hence the kHz to MHz conversion
		 * below.
		 */
/*		int cpu = policy->cpu;
		int volt_new = bcm_get_cpuvoltage(cpu, freqs.new / 1000);
		int volt_old = bcm_get_cpuvoltage(cpu, freqs.old / 1000);

		if (volt_new != volt_old) {
			pr_info("%s: cpu volt change: %d --> %d\n", __func__,
				volt_old, volt_new);
			regulator_set_voltage(b->cpu_regulator, volt_new,
				volt_new);
		}
	}
*/
	/* Get the turbo mode frequency. Switching to and from turbo mode
	 * needs special handling.
	 */
/*	index_turbo = info->index_turbo;
	freq_turbo = info->freq_tbl[index_turbo].cpu_freq * 1000;
*/
	/* Set APPS PLL enable bit when entering to turbo mode */
/*	if (freqs.new == freq_turbo) *ori line 
		clk_enable(b->appspll_en_clk);
*/
	/* freq.new will be in kHz. convert it to Hz for clk_set_rate */
/*	ret = wait_for_pll_on();
	if (!ret)
		ret = clk_set_rate(b->cpu_clk, freqs.new * 1000);
*/
	/* Clear APPS PLL enable bit when entering to normal mode */
/*	if (freqs.new != freq_turbo)
		clk_disable(b->appspll_en_clk);
*/
	/* If we are switching to a lower frequency, we can potentially
	 * decrease the core voltage after changing the frequency.
	 */
//	if (!ret && freqs.new < freqs.old) {
		/* bcm_get_cpuvoltage expects frequency in MHz, cpufreq core
		 * gives the frequency in kHz. Hence the kHz to MHz conversion
		 * below.
		 */
/*		int cpu = policy->cpu;
		int volt_new = bcm_get_cpuvoltage(cpu, freqs.new / 1000);
		int volt_old = bcm_get_cpuvoltage(cpu, freqs.old / 1000);

		if (volt_new != volt_old) {
			pr_info("%s: cpu volt change: %d --> %d\n", __func__,
				volt_old, volt_new);
			regulator_set_voltage(b->cpu_regulator, volt_new,
				volt_new);
		}
	}

	local_irq_enable();
	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	if (unlikely(ret))
		pr_info("%s: setting cpu clock failed : %d\n", __func__, ret);

	return ret;
}
*/

//	orig until here  =======================================

//	add eve oc form here =======================================

	freqs.old = bcm_cpufreq_get_speed(0);
	freqs.new = b->bcm_freqs_table[index].frequency;

	if (freqs.new > policy->max) {
#ifdef CONFIG_CPU_FREQ_DEBUG
		printk(KERN_DEBUG "cpufreq set to : %u --> %u we don't support higher\n",
			freqs.new, policy->max);
#endif
		freqs.new = policy->max;
	}
	if (freqs.new < policy->min) {
#ifdef CONFIG_CPU_FREQ_DEBUG
		printk(KERN_DEBUG "cpufreq set to : %u --> %u we don't support lower\n",
			freqs.new, policy->min);
#endif
		freqs.new = policy->min;
	}
	if (freqs.new == freqs.old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
		printk(KERN_DEBUG "cpufreq is same : %u <-> %u\n",
			freqs.old, freqs.new);
#endif
		activate = 0;
		return 0;
	}
	if (freqs.new < freqs.old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
		printk(KERN_DEBUG "cpufreq is lower : %u --> %u\n",
			freqs.old, freqs.new);
#endif
		activate = 0;
	}
	if (freqs.new > freqs.old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
		printk(KERN_DEBUG "cpufreq is higher : %u --> %u\n",
			freqs.old, freqs.new);
#endif
		activate = 1;
	}

	cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
	local_irq_disable();

	/* If we are switching to a higher frequency, we may have to increase
	 * the core voltage first before changing the frequency.
	 */

	if (freqs.new != freqs.old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
		printk(KERN_DEBUG "cpufreq transition : %u --> %u\n",
			freqs.old, freqs.new);
#endif
		if (activate==1) {
			volt_new = bcm_get_cpuvoltage(policy->cpu, freqs.new / 1000);
			volt_old = bcm_get_cpuvoltage(policy->cpu, freqs.old / 1000);

			if (volt_new == volt_old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
				printk(KERN_DEBUG "cpu_volt is same: %d <-> %d\n",
					volt_old, volt_new);
#endif
			}
			else if (volt_new != volt_old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
				printk(KERN_DEBUG "cpu_volt change: %d --> %d\n",
					volt_old, volt_new);
#endif
				regulator_set_voltage(b->cpu_regulator, volt_new,
					volt_new);
			}
		}

	/* We set the nedded declarations for all found freqs */
	index_makwuz	= info->index_makwuz;
	freq_makwuz 	= info->freq_tbl[index_makwuz].cpu_freq * 1000;
	index_ngenos	= info->index_ngenos;
	freq_ngenos	= info->freq_tbl[index_ngenos].cpu_freq * 1000;
	index_turbo	= info->index_turbo;
	freq_turbo	= info->freq_tbl[index_turbo].cpu_freq * 1000;
	index_kenceng	= info->index_kenceng;
	freq_kenceng	= info->freq_tbl[index_kenceng].cpu_freq * 1000;
	index_kebut	= info->index_kebut;
	freq_kebut	= info->freq_tbl[index_kebut].cpu_freq * 1000;
	index_normal	= info->index_normal;
	freq_normal	= info->freq_tbl[index_normal].cpu_freq * 1000;
	index_lelet	= info->index_lelet;
	freq_lelet	= info->freq_tbl[index_lelet].cpu_freq * 1000;
	index_lemot	= info->index_lemot;
	freq_lemot	= info->freq_tbl[index_lemot].cpu_freq * 1000;

		/* Height Frequencies Need's special hundling :) */
		if ((freqs.new == freq_makwuz)	||
			(freqs.new == freq_ngenos)	||
			(freqs.new == freq_turbo)	||
			(freqs.new == freq_kenceng))
		{
			clk_enable(b->appspll_en_clk);
			ret = wait_for_pll_on();
			if (!ret)
				ret = clk_set_rate(b->cpu_clk, freqs.new * 1000);
		}
		else if ((freqs.new == freq_kebut) ||
				(freqs.new == freq_normal)	 ||
				(freqs.new == freq_lelet)	 ||
				(freqs.new == freq_lemot))
		{
			clk_disable(b->appspll_en_clk);
		}

		if (activate==0) {
			volt_new = bcm_get_cpuvoltage(policy->cpu, freqs.new / 1000);
			volt_old = bcm_get_cpuvoltage(policy->cpu, freqs.old / 1000);

			if (volt_new == volt_old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
				printk(KERN_DEBUG "cpu_volt is same: %d <-> %d\n",
					volt_old, volt_new);
#endif
			}
			else if (volt_new != volt_old) {
#ifdef CONFIG_CPU_FREQ_DEBUG
				printk(KERN_DEBUG "cpu_volt change: %d --> %d\n",
					volt_old, volt_new);
#endif
				regulator_set_voltage(b->cpu_regulator, volt_new,
					volt_new);
			}
			ret = clk_set_rate(b->cpu_clk, freqs.new * 1000);
		}
	}

	local_irq_enable();
	cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	if (unlikely(ret))
#ifdef CONFIG_CPU_FREQ_DEBUG
		printk(KERN_DEBUG "setting cpu clock failed : %d\n", ret);
#endif
	return ret;
}

//	add eve oc until here =======================================

static int bcm_cpufreq_init(struct cpufreq_policy *policy)
{
	struct bcm_cpufreq *b = NULL;
	struct bcm_cpu_info *info = NULL;
	int ret;

	pr_info("%s\n", __func__);

	/* Get handle to cpu private data */
	b = &bcm_cpufreq[policy->cpu];
	info = &b->plat->info[policy->cpu];

	/* Get cpu clock handle */
	b->cpu_clk = clk_get(NULL, info->cpu_clk);
	if (IS_ERR(b->cpu_clk)) {
		pr_info("%s: cpu clk_get failed\n", __func__);
		ret = PTR_ERR(b->cpu_clk);
		goto err_clk_get_cpu_clk;
	}

	/* Get cpu clock handle */
	b->appspll_en_clk = clk_get(NULL, info->appspll_en_clk);
	if (IS_ERR(b->appspll_en_clk)) {
		pr_info("%s: appspll_en clk_get failed\n", __func__);
		ret = PTR_ERR(b->appspll_en_clk);
		goto err_clk_get_appspll_en_clk;
	}

	/* Get cpu regulator handle */
	b->cpu_regulator = regulator_get(NULL, info->cpu_regulator);
	if (IS_ERR(b->cpu_regulator)) {
		pr_info("%s: cpu regulator_get failed\n", __func__);
		ret = PTR_ERR(b->cpu_regulator);
		goto err_regulator_get;
	}

	/* Set default policy and cpuinfo */
	policy->cur = bcm_cpufreq_get_speed(0);
	/* FIXME: Tune this value */
	policy->cpuinfo.transition_latency = CPUFREQ_ETERNAL;

	ret = bcm_create_cpufreqs_table(policy, &(b->bcm_freqs_table));
	if (ret) {
		pr_info("%s: setup_cpufreqs_table failed: %d\n",
			__func__, ret);
		goto err_cpufreqs_table;
	}

	ret = cpufreq_frequency_table_cpuinfo(policy, b->bcm_freqs_table);
	if (ret) {
		pr_info("%s: cpufreq_frequency_table_cpuinfo failed\n",
			__func__);
		goto err_cpuinfo;
	}
	cpufreq_frequency_table_get_attr(b->bcm_freqs_table, policy->cpu); //add eve oc
	b->policy = policy;

	return 0;

err_cpuinfo:
	kfree(b->bcm_freqs_table);
err_cpufreqs_table:
	regulator_put(b->cpu_regulator);
err_regulator_get:
	clk_put(b->appspll_en_clk);
err_clk_get_appspll_en_clk:
	clk_put(b->cpu_clk);
err_clk_get_cpu_clk:
	return ret;
}

static int bcm_cpufreq_exit(struct cpufreq_policy *policy)
{
	struct bcm_cpufreq *b = &bcm_cpufreq[policy->cpu];
	pr_info("%s\n", __func__);
	cpufreq_frequency_table_put_attr(policy->cpu); //add eve oc

	kfree(b->bcm_freqs_table);
	regulator_put(b->cpu_regulator);
	clk_put(b->cpu_clk);

	return 0;
}

/*********************************************************************
 *                              INIT CODE                            *
 *********************************************************************/

static struct freq_attr *bcm_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver bcm_cpufreq_driver = {
	.name   = "bcm",
	.flags  = 0,
	.init   = bcm_cpufreq_init,
	.verify = bcm_cpufreq_verify_speed,
	.target = bcm_cpufreq_set_speed,
	.get    = bcm_cpufreq_get_speed,
	.exit   = bcm_cpufreq_exit,
	.attr   = bcm_cpufreq_attr,
	.owner  = THIS_MODULE,
};

static int cpufreq_drv_probe(struct platform_device *pdev)
{
	struct bcm_cpufreq_drv_plat *plat = pdev->dev.platform_data;
	int i;

	pr_info("%s\n", __func__);

	/* allocate memory for per-cpu data for all cpus */
	bcm_cpufreq = kzalloc(plat->nr_cpus * sizeof(struct bcm_cpufreq),
		GFP_KERNEL);
	if (!bcm_cpufreq) {
		pr_info("%s: kzalloc failed for bcm_cpufreq\n", __func__);
		return -ENOMEM;
	}

	for (i = 0; i < plat->nr_cpus; i++)
		bcm_cpufreq->plat = plat;
	platform_set_drvdata(pdev, bcm_cpufreq);

	return cpufreq_register_driver(&bcm_cpufreq_driver);
}

static int __devexit cpufreq_drv_remove(struct platform_device *pdev)
{
	if (cpufreq_unregister_driver(&bcm_cpufreq_driver) != 0)
		pr_info("%s: cpufreq unregister failed\n", __func__);
	kfree(bcm_cpufreq);
	bcm_cpufreq = NULL;

	return 0;
}

static struct platform_driver cpufreq_drv = {
	.probe = cpufreq_drv_probe,
	.remove = __devexit_p(cpufreq_drv_remove),
	.driver = {
		.name = "bcm21553-cpufreq-drv",
	},
};

static int __init cpufreq_drv_init(void)
{
	return platform_driver_register(&cpufreq_drv);
}
module_init(cpufreq_drv_init);

static void __exit cpufreq_drv_exit(void)
{
	platform_driver_unregister(&cpufreq_drv);
}
module_exit(cpufreq_drv_exit);

MODULE_ALIAS("platform:bcm_cpufreq_drv");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CPU Frequency Driver for Broadcom Chipsets");
