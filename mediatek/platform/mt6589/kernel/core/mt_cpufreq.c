/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/earlysuspend.h>
#include <linux/spinlock.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/xlog.h>
#include <linux/jiffies.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#include "mach/mt_typedefs.h"
#include "mach/mt_clkmgr.h"
#include "mach/mt_cpufreq.h"
#include "mach/sync_write.h"

/**************************************************
* enable for DVFS random test
***************************************************/
//#define MT_DVFS_RANDOM_TEST

/**************************************************
* enable this option to adjust buck voltage
***************************************************/
#define MT_BUCK_ADJUST

/***************************
* debug message
****************************/
#define dprintk(fmt, args...)                                       \
do {                                                                \
    if (mt_cpufreq_debug) {                                         \
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", fmt, ##args);   \
    }                                                               \
} while(0)

#define ARRAY_AND_SIZE(x)	(x), ARRAY_SIZE(x)

#ifdef CONFIG_HAS_EARLYSUSPEND
static struct early_suspend mt_cpufreq_early_suspend_handler =
{
    .level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 200,
    .suspend = NULL,
    .resume  = NULL,
};
#endif

#define DVFS_F1     (1209000)   // KHz
#define DVFS_F2     ( 988000)   // KHz
#define DVFS_F3     ( 754000)   // KHz
#define DVFS_F4     ( 497250)   // KHz

#define DVFS_V1     (1200)  // mV
#define DVFS_V2     (1150)  // mV
#define DVFS_V3     (1050)  // mV
#define DVFS_V4     ( 950)  // mV

/*****************************************
* PMIC settle time, should not be changed
******************************************/
#define PMIC_SETTLE_TIME (40) // us

/*****************************************
* PLL settle time, should not be changed
******************************************/
#define PLL_SETTLE_TIME (30) // us

/***********************************************
* RMAP DOWN TIMES to postpone frequency degrade
************************************************/
#define RAMP_DOWN_TIMES (2)

/**********************************
* Available Clock Source for CPU
***********************************/
#define TOP_CKMUXSEL_CLKSQ   0x0
#define TOP_CKMUXSEL_ARMPLL  0x1
#define TOP_CKMUXSEL_MAINPLL 0x2
#define TOP_CKMUXSEL_MMPLL   0x3

/**************************************************
* enable DVFS function
***************************************************/
static int g_dvfs_disable_count = 0;

static unsigned int g_cur_freq;
static unsigned int g_limited_max_ncpu;
static unsigned int g_limited_max_freq;
static unsigned int g_limited_min_freq;

static int g_ramp_down_count = 0;

static bool mt_cpufreq_debug = false;
static bool mt_cpufreq_ready = false;
static bool mt_cpufreq_pause = false;
static bool mt_cpufreq_ptpod_disable = false;

static DEFINE_SPINLOCK(mt_cpufreq_lock);

/***************************
* Operate Point Definition
****************************/
#define OP(khz, volt)       \
{                           \
    .cpufreq_khz = khz,     \
    .cpufreq_volt = volt,   \
}

struct mt_cpu_freq_info
{
    unsigned int cpufreq_khz;
    unsigned int cpufreq_volt;
};

struct mt_cpu_power_info
{
    unsigned int cpufreq_khz;
    unsigned int cpufreq_ncpu;
    unsigned int cpufreq_power;
};

/***************************
* MT6589 E1 DVFS Table
****************************/
static struct mt_cpu_freq_info mt6589_freqs_e1[] = {
    OP(DVFS_F1, DVFS_V1),
    OP(DVFS_F2, DVFS_V2),
    OP(DVFS_F3, DVFS_V3),
    OP(DVFS_F4, DVFS_V4),
};

static unsigned int mt_cpu_freqs_num;
static struct mt_cpu_freq_info *mt_cpu_freqs = NULL;
static struct cpufreq_frequency_table *mt_cpu_freqs_table;
static struct mt_cpu_power_info *mt_cpu_power = NULL;

/******************************
* Extern Function Declaration
*******************************/
extern int spm_dvfs_ctrl_volt(u32 value);
extern int mtk_cpufreq_register(struct mt_cpu_power_info *freqs, int num);
extern void hp_limited_cpu_num(int num);

/***********************************************
* MT6589 E1 Raw Data: 1.3Ghz @ 1.15V @ TT 125C
************************************************/
#define P_MCU_L         (357)   // MCU Leakage Power
#define P_MCU_T         (1228)  // MCU Total Power
#define P_CA7_L         (56)    // CA7 Leakage Power
#define P_CA7_T         (256)   // Single CA7 Core Power

#define P_MCL99_105C_L  (632)   // MCL99 Leakage Power @ 105C
#define P_MCL99_25C_L   (54)    // MCL99 Leakage Power @ 25C
#define P_MCL50_105C_L  (211)   // MCL50 Leakage Power @ 105C
#define P_MCL50_25C_L   (18)    // MCL50 Leakage Power @ 25C

#define T_105           (105)   // Temperature 105C
#define T_60            (60)    // Temperature 60C
#define T_25            (25)    // Temperature 25C

#define P_MCU_D ((P_MCU_T - P_MCU_L) - 4 * (P_CA7_T - P_CA7_L)) // MCU dynamic power except of CA7 cores

#define P_TOTAL_CORE_L (P_MCL99_25C_L + (((((P_MCL99_105C_L - P_MCL99_25C_L) * 100) / (T_105 - T_25)) * (T_60 - T_25)) / 100)) // Total leakage at T_60
#define P_EACH_CORE_L  ((P_TOTAL_CORE_L * ((P_CA7_L * 1000) / P_MCU_L)) / 1000) // 1 core leakage at T_60

#define P_CA7_D_1_CORE ((P_CA7_T - P_CA7_L) * 1) // CA7 dynamic power for 1 cores turned on
#define P_CA7_D_2_CORE ((P_CA7_T - P_CA7_L) * 2) // CA7 dynamic power for 2 cores turned on
#define P_CA7_D_3_CORE ((P_CA7_T - P_CA7_L) * 3) // CA7 dynamic power for 3 cores turned on
#define P_CA7_D_4_CORE ((P_CA7_T - P_CA7_L) * 4) // CA7 dynamic power for 4 cores turned on

#define A_1_CORE (P_MCU_D + P_CA7_D_1_CORE) // MCU dynamic power for 1 cores turned on
#define A_2_CORE (P_MCU_D + P_CA7_D_2_CORE) // MCU dynamic power for 2 cores turned on
#define A_3_CORE (P_MCU_D + P_CA7_D_3_CORE) // MCU dynamic power for 3 cores turned on
#define A_4_CORE (P_MCU_D + P_CA7_D_4_CORE) // MCU dynamic power for 4 cores turned on


/* Look for MAX frequency in number of DVS. */
unsigned int mt_cpufreq_max_frequency_by_DVS(unsigned int num)
{
    int voltage_change_num = 0;
	int i = 0;
	
    /* Assume mt6589_freqs_e1 voltage will be put in order, and freq will be put from high to low.*/
    if(num == voltage_change_num)
    {
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "PTPOD0:num = %d, frequency= %d\n", num, mt_cpu_freqs[0].cpufreq_khz);
        return mt_cpu_freqs[0].cpufreq_khz;	
    }
	
    for (i = 1; i < mt_cpu_freqs_num; i++)
    {
        if(mt_cpu_freqs[i].cpufreq_volt != mt_cpu_freqs[i-1].cpufreq_volt)
            voltage_change_num++;
		
        if(num == voltage_change_num)
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "PTPOD1:num = %d, frequency= %d\n", num, mt_cpu_freqs[i].cpufreq_khz);
			return mt_cpu_freqs[i].cpufreq_khz;
        }
    }

    xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "PTPOD2:num = %d, NOT found! return 0!\n", num);
    return 0;
}
EXPORT_SYMBOL(mt_cpufreq_max_frequency_by_DVS);


static void mt_setup_power_table(int num)
{
    int i = 0, j = 0, p_dynamic = 0, p_leakage = 0, freq_ratio = 0, volt_square_ratio = 0;
    struct mt_cpu_power_info temp_power_info;

    dprintk("P_MCU_D = %d\n", P_MCU_D);  

    dprintk("P_CA7_D_1_CORE = %d, P_CA7_D_2_CORE = %d, P_CA7_D_3_CORE = %d, P_CA7_D_4_CORE = %d\n", 
             P_CA7_D_1_CORE, P_CA7_D_2_CORE, P_CA7_D_3_CORE, P_CA7_D_4_CORE);

    dprintk("P_TOTAL_CORE_L = %d, P_EACH_CORE_L = %d\n", 
             P_TOTAL_CORE_L, P_EACH_CORE_L);

    dprintk("A_1_CORE = %d, A_2_CORE = %d, A_3_CORE = %d, A_4_CORE = %d\n", 
             A_1_CORE, A_2_CORE, A_3_CORE, A_4_CORE);

    mt_cpu_power = kzalloc((num * 4) * sizeof(struct mt_cpu_power_info), GFP_KERNEL);

    for (i = 0; i < num; i++)
    {
        volt_square_ratio = (((mt_cpu_freqs[i].cpufreq_volt * 100) / 1150) * ((mt_cpu_freqs[i].cpufreq_volt * 100) / 1150)) / 100;
        freq_ratio = (mt_cpu_freqs[i].cpufreq_khz / 1300);
        dprintk("freq_ratio = %d, volt_square_ratio %d\n", freq_ratio, volt_square_ratio);

        // 1 core
        p_dynamic = (((A_1_CORE * freq_ratio) / 1000) * volt_square_ratio) / 100;
        p_leakage = ((P_TOTAL_CORE_L * ((mt_cpu_freqs[i].cpufreq_volt * 100) / 1150)) / 100) - 3 * P_EACH_CORE_L;
        dprintk("p_dynamic = %d, p_leakage = %d\n", p_dynamic, p_leakage);

        mt_cpu_power[i * 4 + 0].cpufreq_ncpu = 1;
        mt_cpu_power[i * 4 + 0].cpufreq_khz = mt_cpu_freqs[i].cpufreq_khz;
        mt_cpu_power[i * 4 + 0].cpufreq_power = p_dynamic + p_leakage;
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpu_power[%d]: cpufreq_ncpu = %d, cpufreq_khz = %d, cpufreq_power = %d\n", (i * 4 + 0), 
                                                     mt_cpu_power[(i * 4 + 0)].cpufreq_ncpu, mt_cpu_power[(i * 4 + 0)].cpufreq_khz, mt_cpu_power[(i * 4 + 0)].cpufreq_power);

        // 2 core
        p_dynamic = (((A_2_CORE * freq_ratio) / 1000) * volt_square_ratio) / 100;
        p_leakage = ((P_TOTAL_CORE_L * ((mt_cpu_freqs[i].cpufreq_volt * 100) / 1150)) / 100) - 2 * P_EACH_CORE_L;
        dprintk("p_dynamic = %d, p_leakage = %d\n", p_dynamic, p_leakage);

        mt_cpu_power[i * 4 + 1].cpufreq_ncpu = 2;
        mt_cpu_power[i * 4 + 1].cpufreq_khz = mt_cpu_freqs[i].cpufreq_khz;
        mt_cpu_power[i * 4 + 1].cpufreq_power = p_dynamic + p_leakage;
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpu_power[%d]: cpufreq_ncpu = %d, cpufreq_khz = %d, cpufreq_power = %d\n", (i * 4 + 1), 
                                                     mt_cpu_power[(i * 4 + 1)].cpufreq_ncpu, mt_cpu_power[(i * 4 + 1)].cpufreq_khz, mt_cpu_power[(i * 4 + 1)].cpufreq_power);

        // 3 core
        p_dynamic = (((A_3_CORE * freq_ratio) / 1000) * volt_square_ratio) / 100;
        p_leakage = ((P_TOTAL_CORE_L * ((mt_cpu_freqs[i].cpufreq_volt * 100) / 1150)) / 100) - 1 * P_EACH_CORE_L;
        dprintk("p_dynamic = %d, p_leakage = %d\n", p_dynamic, p_leakage);

        mt_cpu_power[i * 4 + 2].cpufreq_ncpu = 3;
        mt_cpu_power[i * 4 + 2].cpufreq_khz = mt_cpu_freqs[i].cpufreq_khz;
        mt_cpu_power[i * 4 + 2].cpufreq_power = p_dynamic + p_leakage;
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpu_power[%d]: cpufreq_ncpu = %d, cpufreq_khz = %d, cpufreq_power = %d\n", (i * 4 + 2), 
                                                     mt_cpu_power[(i * 4 + 2)].cpufreq_ncpu, mt_cpu_power[(i * 4 + 2)].cpufreq_khz, mt_cpu_power[(i * 4 + 2)].cpufreq_power);

        // 4 core
        p_dynamic = (((A_4_CORE * freq_ratio) / 1000) * volt_square_ratio) / 100;
        p_leakage = (P_TOTAL_CORE_L * ((mt_cpu_freqs[i].cpufreq_volt * 100) / 1150)) / 100;
        dprintk("p_dynamic = %d, p_leakage = %d\n", p_dynamic, p_leakage);

        mt_cpu_power[i * 4 + 3].cpufreq_ncpu = 4;
        mt_cpu_power[i * 4 + 3].cpufreq_khz = mt_cpu_freqs[i].cpufreq_khz;
        mt_cpu_power[i * 4 + 3].cpufreq_power = p_dynamic + p_leakage;
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpu_power[%d]: cpufreq_ncpu = %d, cpufreq_khz = %d, cpufreq_power = %d\n", (i * 4 + 3), 
                                                     mt_cpu_power[(i * 4 + 3)].cpufreq_ncpu, mt_cpu_power[(i * 4 + 3)].cpufreq_khz, mt_cpu_power[(i * 4 + 3)].cpufreq_power);
    }

    for (i = (num * 4 - 1); i > 0; i--)
    {
        for (j = 1; j <= i; j++)
        {
            if (mt_cpu_power[j - 1].cpufreq_power < mt_cpu_power[j].cpufreq_power)
            {
                temp_power_info.cpufreq_khz = mt_cpu_power[j - 1].cpufreq_khz;
                temp_power_info.cpufreq_ncpu = mt_cpu_power[j - 1].cpufreq_ncpu;
                temp_power_info.cpufreq_power = mt_cpu_power[j - 1].cpufreq_power;

                mt_cpu_power[j - 1].cpufreq_khz = mt_cpu_power[j].cpufreq_khz;
                mt_cpu_power[j - 1].cpufreq_ncpu = mt_cpu_power[j].cpufreq_ncpu;
                mt_cpu_power[j - 1].cpufreq_power = mt_cpu_power[j].cpufreq_power;

                mt_cpu_power[j].cpufreq_khz = temp_power_info.cpufreq_khz;
                mt_cpu_power[j].cpufreq_ncpu = temp_power_info.cpufreq_ncpu;
                mt_cpu_power[j].cpufreq_power = temp_power_info.cpufreq_power;
            }
        }
    }

    for (i = 0; i < (num * 4); i++)
    {
        dprintk("mt_cpu_power[%d].cpufreq_khz = %d, ", i, mt_cpu_power[i].cpufreq_khz);
        dprintk("mt_cpu_power[%d].cpufreq_ncpu = %d, ", i, mt_cpu_power[i].cpufreq_ncpu);
        dprintk("mt_cpu_power[%d].cpufreq_power = %d\n", i, mt_cpu_power[i].cpufreq_power);
    }

    #ifdef CONFIG_THERMAL
        mtk_cpufreq_register(mt_cpu_power, (num * 4));
    #endif
}

/***********************************************
* register frequency table to cpufreq subsystem
************************************************/
static int mt_setup_freqs_table(struct cpufreq_policy *policy, struct mt_cpu_freq_info *freqs, int num)
{
    struct cpufreq_frequency_table *table;
    int i, ret;

    table = kzalloc((num + 1) * sizeof(*table), GFP_KERNEL);
    if (table == NULL)
        return -ENOMEM;

    for (i = 0; i < num; i++) {
        table[i].index = i;
        table[i].frequency = freqs[i].cpufreq_khz;
    }
    table[num].frequency = i;
    table[num].frequency = CPUFREQ_TABLE_END;

    mt_cpu_freqs = freqs;
    mt_cpu_freqs_num = num;
    mt_cpu_freqs_table = table;

    ret = cpufreq_frequency_table_cpuinfo(policy, table);
    if (!ret)
        cpufreq_frequency_table_get_attr(mt_cpu_freqs_table, policy->cpu);

    if (mt_cpu_power == NULL)
        mt_setup_power_table(num);

    return 0;
}

/*****************************
* set CPU DVFS status
******************************/
int mt_cpufreq_state_set(int enabled)
{
    if (enabled)
    {
        if (!mt_cpufreq_pause)
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "cpufreq already enabled\n");
            return 0;
        }

        /*************
        * enable DVFS
        **************/
        g_dvfs_disable_count--;
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "enable DVFS: g_dvfs_disable_count = %d\n", g_dvfs_disable_count);

        /***********************************************
        * enable DVFS if no any module still disable it
        ************************************************/
        if (g_dvfs_disable_count <= 0)
        {
            mt_cpufreq_pause = false;
        }
        else
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "someone still disable cpufreq, cannot enable it\n");
        }
    }
    else
    {
        /**************
        * disable DVFS
        ***************/
        g_dvfs_disable_count++;
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "disable DVFS: g_dvfs_disable_count = %d\n", g_dvfs_disable_count);

        if (mt_cpufreq_pause)
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "cpufreq already disabled\n");
            return 0;
        }

        mt_cpufreq_pause = true;
    }

    return 0;
}
EXPORT_SYMBOL(mt_cpufreq_state_set);

static int mt_cpufreq_verify(struct cpufreq_policy *policy)
{
    dprintk("call mt_cpufreq_verify!\n");
    return cpufreq_frequency_table_verify(policy, mt_cpu_freqs_table);
}

static unsigned int mt_cpufreq_get(unsigned int cpu)
{
    dprintk("call mt_cpufreq_get: %d!\n", g_cur_freq);
    return g_cur_freq;
}

static void mt_cpu_clock_switch(unsigned int sel)
{
    unsigned int ckmuxsel = 0;

    ckmuxsel = DRV_Reg32(TOP_CKMUXSEL) & ~0xC;

    switch (sel)
    {
        case TOP_CKMUXSEL_CLKSQ:
            mt65xx_reg_sync_writel((ckmuxsel | 0x00), TOP_CKMUXSEL);
            break;
        case TOP_CKMUXSEL_ARMPLL:
            mt65xx_reg_sync_writel((ckmuxsel | 0x04), TOP_CKMUXSEL);
            break;
        case TOP_CKMUXSEL_MAINPLL:
            mt65xx_reg_sync_writel((ckmuxsel | 0x08), TOP_CKMUXSEL);
            break;
        case TOP_CKMUXSEL_MMPLL:
            mt65xx_reg_sync_writel((ckmuxsel | 0x0C), TOP_CKMUXSEL);
            break;
        default:
            break;
    }
}

static void mt_cpufreq_volt_set(unsigned int target_volt)
{
    switch (target_volt)
    {
        case DVFS_V1:
            dprintk("switch to DVS0: %d mV\n", DVFS_V1);
            spm_dvfs_ctrl_volt(0);
            break;
        case DVFS_V2:
            dprintk("switch to DVS1: %d mV\n", DVFS_V2);
            spm_dvfs_ctrl_volt(1);
            break;
        case DVFS_V3:
            dprintk("switch to DVS2: %d mV\n", DVFS_V3);
            spm_dvfs_ctrl_volt(2);
            break;
        case DVFS_V4:
            dprintk("switch to DVS3: %d mV\n", DVFS_V4);
            spm_dvfs_ctrl_volt(3);
            break;
        default:
            break;
    }
}

/*****************************************
* frequency ramp up and ramp down handler
******************************************/
/***********************************************************
* [note]
* 1. frequency ramp up need to wait voltage settle
* 2. frequency ramp down do not need to wait voltage settle
************************************************************/
static void mt_cpufreq_set(unsigned int freq_old, unsigned int freq_new, unsigned int target_volt)
{
    unsigned int armpll = 0;

    if (freq_new >= 1001000)
    {
        armpll = 0x8009A000;
        armpll = armpll + ((freq_new - 1001000) / 13000) * 0x2000;
    }
    else if (freq_new >= 520000)
    {
        armpll = 0x810A0000;
        armpll = armpll + ((freq_new - 520000) / 6500) * 0x2000;
    }
    else
    {
        armpll = 0x820A0000;
        armpll = armpll + ((freq_new - 260000) / 3250) * 0x2000;
    }

    enable_pll(MAINPLL, "CPU_DVFS");
    if (freq_new > freq_old)
    {
        #ifdef MT_BUCK_ADJUST
        mt_cpufreq_volt_set(target_volt);
        udelay(PMIC_SETTLE_TIME);
        #endif

        mt65xx_reg_sync_writel(0x0A, TOP_CKDIV1);
        mt_cpu_clock_switch(TOP_CKMUXSEL_MAINPLL);

        mt65xx_reg_sync_writel(armpll, ARMPLL_CON1);

        mb();
        udelay(PLL_SETTLE_TIME);

        mt_cpu_clock_switch(TOP_CKMUXSEL_ARMPLL);
        mt65xx_reg_sync_writel(0x00, TOP_CKDIV1);
    }
    else
    {
        mt65xx_reg_sync_writel(0x0A, TOP_CKDIV1);
        mt_cpu_clock_switch(TOP_CKMUXSEL_MAINPLL);

        mt65xx_reg_sync_writel(armpll, ARMPLL_CON1);

        mb();
        udelay(PLL_SETTLE_TIME);

        mt_cpu_clock_switch(TOP_CKMUXSEL_ARMPLL);
        mt65xx_reg_sync_writel(0x00, TOP_CKDIV1);

        #ifdef MT_BUCK_ADJUST
        mt_cpufreq_volt_set(target_volt);
        #endif
    }
    disable_pll(MAINPLL, "CPU_DVFS");

    g_cur_freq = freq_new;

    dprintk("ARMPLL_CON0 = 0x%x, ARMPLL_CON1 = 0x%x, g_cur_freq = %d\n", DRV_Reg32(ARMPLL_CON0), DRV_Reg32(ARMPLL_CON1), g_cur_freq);
}

/**************************************
* check if maximum frequency is needed
***************************************/
static int mt_cpufreq_keep_max_freq(unsigned int freq_old, unsigned int freq_new)
{
    if (mt_cpufreq_pause)
        return 1;

    /* check if system is going to ramp down */
    if (freq_new < freq_old)
        g_ramp_down_count++;
    else
        g_ramp_down_count = 0;

    if (g_ramp_down_count < RAMP_DOWN_TIMES)
        return 1;

    return 0;
}

#ifdef MT_DVFS_RANDOM_TEST
static int mt_cpufreq_idx_get(int num)
{
    int random = 0, mult = 0, idx;
    random = jiffies & 0xF;

    while (1)
    {
        if ((mult * num) >= random)
        {
            idx = (mult * num) - random;
            break;
        }
        mult++;
    }
    return idx;
}
#endif

static unsigned int mt_thermal_limited_verify(unsigned int target_freq)
{
    int i = 0, index = 0;

    for (i = 0; i < (mt_cpu_freqs_num * 4); i++)
    {
        if (mt_cpu_power[i].cpufreq_ncpu == g_limited_max_ncpu && mt_cpu_power[i].cpufreq_khz == g_limited_max_freq)
        {
            index = i;
            break;
        }
    }

    for (index = i; index < (mt_cpu_freqs_num * 4); index++)
    {
        if (mt_cpu_power[i].cpufreq_ncpu == num_online_cpus())
        {
            if (target_freq >= mt_cpu_power[i].cpufreq_khz)
            {
                dprintk("target_freq = %d, ncpu = %d\n", mt_cpu_power[i].cpufreq_khz, num_online_cpus());
                target_freq = mt_cpu_power[i].cpufreq_khz;
                break;
            }
        }
    }

    return target_freq;
}

/**********************************
* cpufreq target callback function
***********************************/
/*************************************************
* [note]
* 1. handle frequency change request
* 2. call mt_cpufreq_set to set target frequency
**************************************************/
static int mt_cpufreq_target(struct cpufreq_policy *policy, unsigned int target_freq, unsigned int relation)
{
    int i, idx;
    unsigned int cpu;
    unsigned long flags;

    struct mt_cpu_freq_info next;
    struct cpufreq_freqs freqs;

    if (!mt_cpufreq_ready)
        return -ENOSYS;

    if (policy->cpu >= num_possible_cpus())
        return -EINVAL;

    /******************************
    * look up the target frequency
    *******************************/
    if (cpufreq_frequency_table_target(policy, mt_cpu_freqs_table, target_freq, relation, &idx))
    {
        return -EINVAL;
    }

    #ifdef MT_DVFS_RANDOM_TEST
    idx = mt_cpufreq_idx_get(4);
    #endif

    next.cpufreq_khz = mt6589_freqs_e1[idx].cpufreq_khz;

    #ifdef MT_DVFS_RANDOM_TEST
    dprintk("idx = %d, freqs.old = %d, freqs.new = %d\n", idx, policy->cur, next.cpufreq_khz);
    #endif

    freqs.old = policy->cur;
    freqs.new = next.cpufreq_khz;
    freqs.cpu = policy->cpu;

    #ifndef MT_DVFS_RANDOM_TEST
    if (mt_cpufreq_keep_max_freq(freqs.old, freqs.new))
    {
        freqs.new = policy->max;
    }

    freqs.new = mt_thermal_limited_verify(freqs.new);

    if (freqs.new < g_limited_min_freq)
    {
        dprintk("cannot switch CPU frequency to %d Mhz due to voltage limitation\n", g_limited_min_freq / 1000);
        freqs.new = g_limited_min_freq;
    }
    #endif

    /************************************************
    * DVFS keep at 1001Mhz/1.15V when PTPOD initial
    *************************************************/
    if (mt_cpufreq_ptpod_disable)
    {
        freqs.new = DVFS_F2;
    }

    /************************************************
    * target frequency == existing frequency, skip it
    *************************************************/
    if (freqs.old == freqs.new)
    {
        dprintk("CPU frequency from %d MHz to %d MHz (skipped) due to same frequency\n", freqs.old / 1000, freqs.new / 1000);
        return 0;
    }

    /**************************************
    * search for the corresponding voltage
    ***************************************/
    next.cpufreq_volt = 0;

    for (i = 0; i < mt_cpu_freqs_num; i++)
    {
        dprintk("freqs.new = %d, mt_cpu_freqs[%d].cpufreq_khz = %d\n", freqs.new, i, mt_cpu_freqs[i].cpufreq_khz);
        if (freqs.new == mt_cpu_freqs[i].cpufreq_khz)
        {
            next.cpufreq_volt = mt_cpu_freqs[i].cpufreq_volt;
            dprintk("next.cpufreq_volt = %d, mt_cpu_freqs[%d].cpufreq_volt = %d\n", next.cpufreq_volt, i, mt_cpu_freqs[i].cpufreq_volt);
            break;
        }
    }

    if (next.cpufreq_volt == 0)
    {
        dprintk("Error!! Cannot find corresponding voltage at %d Mhz\n", freqs.new / 1000);
        return 0;
    }

    for_each_online_cpu(cpu)
    {
        freqs.cpu = cpu;
        cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);
    }

    spin_lock_irqsave(&mt_cpufreq_lock, flags);

    /******************************
    * set to the target freeuency
    *******************************/
    mt_cpufreq_set(freqs.old, freqs.new, next.cpufreq_volt);

    spin_unlock_irqrestore(&mt_cpufreq_lock, flags);

    for_each_online_cpu(cpu)
    {
        freqs.cpu = cpu;
        cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);
    }

    return 0;
}

/*********************************************************
* set up frequency table and register to cpufreq subsystem
**********************************************************/
static int mt_cpufreq_init(struct cpufreq_policy *policy)
{
    int ret = -EINVAL;

    if (policy->cpu >= num_possible_cpus())
        return -EINVAL;

    policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
    cpumask_setall(policy->cpus);

    /*******************************************************
    * 1 us, assumed, will be overwrited by min_sampling_rate
    ********************************************************/
    policy->cpuinfo.transition_latency = 1000;

    /*********************************************
    * set default policy and cpuinfo, unit : Khz
    **********************************************/
    policy->cpuinfo.max_freq = DVFS_F1;
    policy->cpuinfo.min_freq = DVFS_F4;

    policy->cur = DVFS_F2;
    policy->max = DVFS_F1;
    policy->min = DVFS_F4;
    ret = mt_setup_freqs_table(policy, ARRAY_AND_SIZE(mt6589_freqs_e1));

    if (ret) {
        xlog_printk(ANDROID_LOG_ERROR, "Power/DVFS", "failed to setup frequency table\n");
        return ret;
    }

    return 0;
}

static struct cpufreq_driver mt_cpufreq_driver = {
    .verify = mt_cpufreq_verify,
    .target = mt_cpufreq_target,
    .init   = mt_cpufreq_init,
    .get    = mt_cpufreq_get,
    .name   = "mt-cpufreq",
};

/*********************************
* early suspend callback function
**********************************/
void mt_cpufreq_early_suspend(struct early_suspend *h)
{
    #ifndef MT_DVFS_RANDOM_TEST

    mt_cpufreq_state_set(0);

    #endif

    return;
}

/*******************************
* late resume callback function
********************************/
void mt_cpufreq_late_resume(struct early_suspend *h)
{
    #ifndef MT_DVFS_RANDOM_TEST

    mt_cpufreq_state_set(1);

    #endif

    return;
}

/************************************************
* API to switch back default voltage setting for PTPOD disabled
*************************************************/
void mt_cpufreq_return_default_DVS_by_ptpod(void)
{
    mt65xx_reg_sync_writel(0x50, PMIC_WRAP_DVFS_WDATA0); // 1.20V VPROC
    mt65xx_reg_sync_writel(0x48, PMIC_WRAP_DVFS_WDATA1); // 1.15V VPROC
    mt65xx_reg_sync_writel(0x38, PMIC_WRAP_DVFS_WDATA2); // 1.05V VPROC
    mt65xx_reg_sync_writel(0x28, PMIC_WRAP_DVFS_WDATA3); // 0.95V VPROC
    mt65xx_reg_sync_writel(0x18, PMIC_WRAP_DVFS_WDATA4); // 0.85V VPROC
    mt65xx_reg_sync_writel(0x38, PMIC_WRAP_DVFS_WDATA5); // 1.05V VCORE
    mt65xx_reg_sync_writel(0x28, PMIC_WRAP_DVFS_WDATA6); // 0.95V VCORE
    mt65xx_reg_sync_writel(0x18, PMIC_WRAP_DVFS_WDATA7); // 0.85V VCORE
    xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpufreq return default DVS by ptpod\n");
}
EXPORT_SYMBOL(mt_cpufreq_return_default_DVS_by_ptpod);

/************************************************
* DVFS enable API for PTPOD
*************************************************/
void mt_cpufreq_enable_by_ptpod(void)
{
    mt_cpufreq_ptpod_disable = false;
    xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpufreq enabled by ptpod\n");
}
EXPORT_SYMBOL(mt_cpufreq_enable_by_ptpod);

/************************************************
* DVFS disable API for PTPOD
*************************************************/
unsigned int mt_cpufreq_disable_by_ptpod(void)
{
    struct cpufreq_policy *policy;

    mt_cpufreq_ptpod_disable = true;

    policy = cpufreq_cpu_get(0);

    if (!policy)
        goto no_policy;

    cpufreq_driver_target(policy, DVFS_F2, CPUFREQ_RELATION_L);

    xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpufreq disabled by ptpod, limited freq. at %d\n", DVFS_F2);

    cpufreq_cpu_put(policy);

no_policy:
    return g_cur_freq;
}
EXPORT_SYMBOL(mt_cpufreq_disable_by_ptpod);

/************************************************
* frequency adjust interface for thermal protect
*************************************************/
/******************************************************
* parameter: target power
*******************************************************/
void mt_cpufreq_thermal_protect(unsigned int limited_power)
{
    int i = 0, ncpu = 0, found = 0;

    struct cpufreq_policy *policy;

    policy = cpufreq_cpu_get(0);

    if (!policy)
        goto no_policy;

    ncpu = num_possible_cpus();

    if (limited_power == 0)
    {
        g_limited_max_ncpu = num_possible_cpus();
        g_limited_max_freq = DVFS_F1;

        cpufreq_driver_target(policy, g_limited_max_freq, CPUFREQ_RELATION_L);
        hp_limited_cpu_num(g_limited_max_ncpu);

        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "thermal limit g_limited_max_freq = %d, g_limited_max_ncpu = %d\n", g_limited_max_freq, g_limited_max_ncpu);
    }
    else
    {
        while (ncpu)
        {
            for (i = 0; i < (mt_cpu_freqs_num * 4); i++)
            {
                if (mt_cpu_power[i].cpufreq_ncpu == ncpu)
                {
                    if (mt_cpu_power[i].cpufreq_power <= limited_power)
                    {
                        g_limited_max_ncpu = mt_cpu_power[i].cpufreq_ncpu;
                        g_limited_max_freq = mt_cpu_power[i].cpufreq_khz;

                        found = 1;
                        break;
                    }
                }
            }

            if (found)
                break;

            ncpu--;
        }

        if (!found)
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "thermal limit fail, not found suitable DVFS OPP\n");
        }
        else
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "thermal limit g_limited_max_freq = %d, g_limited_max_ncpu = %d\n", g_limited_max_freq, g_limited_max_ncpu);

            cpufreq_driver_target(policy, g_limited_max_freq, CPUFREQ_RELATION_L);

            hp_limited_cpu_num(g_limited_max_ncpu);

            if (num_online_cpus() > g_limited_max_ncpu)
            {
                for (i = num_online_cpus(); i > g_limited_max_ncpu; i--)
                {
                    xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "turn off CPU%d due to thermal protection\n", (i - 1));
                    cpu_down((i - 1));
                }
            }
        }
    }

    cpufreq_cpu_put(policy);

no_policy:
    return;
}
EXPORT_SYMBOL(mt_cpufreq_thermal_protect);

/***************************
* show current DVFS stauts
****************************/
static int mt_cpufreq_state_read(char *buf, char **start, off_t off, int count, int *eof, void *data)
{
    int len = 0;
    char *p = buf;

    if (!mt_cpufreq_pause)
        p += sprintf(p, "DVFS enabled\n");
    else
        p += sprintf(p, "DVFS disabled\n");

    len = p - buf;
    return len;
}

/************************************
* set DVFS stauts by sysfs interface
*************************************/
static ssize_t mt_cpufreq_state_write(struct file *file, const char *buffer, unsigned long count, void *data)
{
    int enabled = 0;

    if (sscanf(buffer, "%d", &enabled) == 1)
    {
        if (enabled == 1)
        {
            mt_cpufreq_state_set(1);
        }
        else if (enabled == 0)
        {
            mt_cpufreq_state_set(0);
        }
        else
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "bad argument!! argument should be \"1\" or \"0\"\n");
        }
    }
    else
    {
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "bad argument!! argument should be \"1\" or \"0\"\n");
    }

    return count;
}

/****************************
* show current limited freq
*****************************/
static int mt_cpufreq_limited_power_read(char *buf, char **start, off_t off, int count, int *eof, void *data)
{
    int len = 0;
    char *p = buf;

    p += sprintf(p, "g_limited_max_freq = %d, g_limited_max_ncpu = %d\n", g_limited_max_freq, g_limited_max_ncpu);

    len = p - buf;
    return len;
}

/**********************************
* limited power for thermal protect
***********************************/
static ssize_t mt_cpufreq_limited_power_write(struct file *file, const char *buffer, unsigned long count, void *data)
{
    unsigned int power = 0;

    if (sscanf(buffer, "%u", &power) == 1)
    {
        mt_cpufreq_thermal_protect(power);
        return count;
    }
    else
    {
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "bad argument!! please provide the maximum limited power\n");
    }

    return -EINVAL;
}

/***************************
* show current debug status
****************************/
static int mt_cpufreq_debug_read(char *buf, char **start, off_t off, int count, int *eof, void *data)
{
    int len = 0;
    char *p = buf;

    if (mt_cpufreq_debug)
        p += sprintf(p, "cpufreq debug enabled\n");
    else
        p += sprintf(p, "cpufreq debug disabled\n");

    len = p - buf;
    return len;
}

/***********************
* enable debug message
************************/
static ssize_t mt_cpufreq_debug_write(struct file *file, const char *buffer, unsigned long count, void *data)
{
    int debug = 0;

    if (sscanf(buffer, "%d", &debug) == 1)
    {
        if (debug == 0) 
        {
            mt_cpufreq_debug = 0;
            return count;
        }
        else if (debug == 1)
        {
            mt_cpufreq_debug = 1;
            return count;
        }
        else
        {
            xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "bad argument!! should be 0 or 1 [0: disable, 1: enable]\n");
        }
    }
    else
    {
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "bad argument!! should be 0 or 1 [0: disable, 1: enable]\n");
    }

    return -EINVAL;
}

/***************************
* show cpufreq power info
****************************/
static int mt_cpufreq_power_dump_read(char *buf, char **start, off_t off, int count, int *eof, void *data)
{
    int i = 0, len = 0;
    char *p = buf;

    for (i = 0; i < (mt_cpu_freqs_num * 4); i++)
    {
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpu_power[%d].cpufreq_khz = %d\n", i, mt_cpu_power[i].cpufreq_khz);
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpu_power[%d].cpufreq_ncpu = %d\n", i, mt_cpu_power[i].cpufreq_ncpu);
        xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mt_cpu_power[%d].cpufreq_power = %d\n", i, mt_cpu_power[i].cpufreq_power);
    }

    p += sprintf(p, "done\n");

    len = p - buf;
    return len;
}

/*******************************************
* cpufrqe platform driver callback function
********************************************/
static int mt_cpufreq_pdrv_probe(struct platform_device *pdev)
{
    #ifdef CONFIG_HAS_EARLYSUSPEND
    mt_cpufreq_early_suspend_handler.suspend = mt_cpufreq_early_suspend;
    mt_cpufreq_early_suspend_handler.resume = mt_cpufreq_late_resume;
    register_early_suspend(&mt_cpufreq_early_suspend_handler);
    #endif

    /************************************************
    * voltage scaling need to wait PMIC driver ready
    *************************************************/
    mt_cpufreq_ready = true;

    g_cur_freq = DVFS_F2;
    g_limited_max_freq = DVFS_F1;
    g_limited_min_freq = DVFS_F4;

    xlog_printk(ANDROID_LOG_INFO, "Power/DVFS", "mediatek cpufreq initialized\n");

    spm_dvfs_ctrl_volt(1); // default set to 1.15V

    mt65xx_reg_sync_writel(0x021A, PMIC_WRAP_DVFS_ADR0);
    mt65xx_reg_sync_writel(0x021A, PMIC_WRAP_DVFS_ADR1);
    mt65xx_reg_sync_writel(0x021A, PMIC_WRAP_DVFS_ADR2);
    mt65xx_reg_sync_writel(0x021A, PMIC_WRAP_DVFS_ADR3);
    mt65xx_reg_sync_writel(0x021A, PMIC_WRAP_DVFS_ADR4);
    mt65xx_reg_sync_writel(0x026C, PMIC_WRAP_DVFS_ADR5);
    mt65xx_reg_sync_writel(0x026C, PMIC_WRAP_DVFS_ADR6);
    mt65xx_reg_sync_writel(0x026C, PMIC_WRAP_DVFS_ADR7);

    mt65xx_reg_sync_writel(0x50, PMIC_WRAP_DVFS_WDATA0); // 1.20V VPROC
    mt65xx_reg_sync_writel(0x48, PMIC_WRAP_DVFS_WDATA1); // 1.15V VPROC
    mt65xx_reg_sync_writel(0x38, PMIC_WRAP_DVFS_WDATA2); // 1.05V VPROC
    mt65xx_reg_sync_writel(0x28, PMIC_WRAP_DVFS_WDATA3); // 0.95V VPROC
    mt65xx_reg_sync_writel(0x18, PMIC_WRAP_DVFS_WDATA4); // 0.85V VPROC
    mt65xx_reg_sync_writel(0x38, PMIC_WRAP_DVFS_WDATA5); // 1.05V VCORE
    mt65xx_reg_sync_writel(0x28, PMIC_WRAP_DVFS_WDATA6); // 0.95V VCORE
    mt65xx_reg_sync_writel(0x18, PMIC_WRAP_DVFS_WDATA7); // 0.85V VCORE

    return cpufreq_register_driver(&mt_cpufreq_driver);
}

/***************************************
* this function should never be called
****************************************/
static int mt_cpufreq_pdrv_remove(struct platform_device *pdev)
{
    return 0;
}

static struct platform_driver mt_cpufreq_pdrv = {
    .probe      = mt_cpufreq_pdrv_probe,
    .remove     = mt_cpufreq_pdrv_remove,
    .suspend    = NULL,
    .resume     = NULL,
    .driver     = {
        .name   = "mt-cpufreq",
        .owner  = THIS_MODULE,
    },
};

/***********************************************************
* cpufreq initialization to register cpufreq platform driver
************************************************************/
static int __init mt_cpufreq_pdrv_init(void)
{
    int ret = 0;

    struct proc_dir_entry *mt_entry = NULL;
    struct proc_dir_entry *mt_cpufreq_dir = NULL;

    mt_cpufreq_dir = proc_mkdir("cpufreq", NULL);
    if (!mt_cpufreq_dir)
    {
        pr_err("[%s]: mkdir /proc/cpufreq failed\n", __FUNCTION__);
    }
    else
    {
        mt_entry = create_proc_entry("cpufreq_debug", S_IRUGO | S_IWUSR | S_IWGRP, mt_cpufreq_dir);
        if (mt_entry)
        {
            mt_entry->read_proc = mt_cpufreq_debug_read;
            mt_entry->write_proc = mt_cpufreq_debug_write;
        }

        mt_entry = create_proc_entry("cpufreq_limited_power", S_IRUGO | S_IWUSR | S_IWGRP, mt_cpufreq_dir);
        if (mt_entry)
        {
            mt_entry->read_proc = mt_cpufreq_limited_power_read;
            mt_entry->write_proc = mt_cpufreq_limited_power_write;
        }

        mt_entry = create_proc_entry("cpufreq_state", S_IRUGO | S_IWUSR | S_IWGRP, mt_cpufreq_dir);
        if (mt_entry)
        {
            mt_entry->read_proc = mt_cpufreq_state_read;
            mt_entry->write_proc = mt_cpufreq_state_write;
        }

        mt_entry = create_proc_entry("cpufreq_power_dump", S_IRUGO | S_IWUSR | S_IWGRP, mt_cpufreq_dir);
        if (mt_entry)
        {
            mt_entry->read_proc = mt_cpufreq_power_dump_read;
        }
    }

    ret = platform_driver_register(&mt_cpufreq_pdrv);
    if (ret)
    {
        xlog_printk(ANDROID_LOG_ERROR, "Power/DVFS", "failed to register cpufreq driver\n");
        return ret;
    }
    else
    {
        xlog_printk(ANDROID_LOG_ERROR, "Power/DVFS", "cpufreq driver registration done\n");
        return 0;
    }
}

static void __exit mt_cpufreq_pdrv_exit(void)
{
    cpufreq_unregister_driver(&mt_cpufreq_driver);
}

module_init(mt_cpufreq_pdrv_init);
module_exit(mt_cpufreq_pdrv_exit);

MODULE_DESCRIPTION("MediaTek CPU Frequency Scaling driver");
MODULE_LICENSE("GPL");