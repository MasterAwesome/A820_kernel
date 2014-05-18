#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include <linux/types.h>
#include <linux/string.h>

#include <asm/system_misc.h>

#include <mach/mt_typedefs.h>
#include <mach/sync_write.h>
#include <mach/mt_clkmgr.h>
#include <mach/mt_dcm.h>
#include <mach/mt_gpt.h>
#include <mach/mt_spm_sleep.h>
#include <mach/hotplug.h>

#define USING_XLOG

#ifdef USING_XLOG 
#include <linux/xlog.h>

#define TAG     "Power/swap"

#define idle_err(fmt, args...)       \
    xlog_printk(ANDROID_LOG_ERROR, TAG, fmt, ##args)
#define idle_warn(fmt, args...)      \
    xlog_printk(ANDROID_LOG_WARN, TAG, fmt, ##args)
#define idle_info(fmt, args...)      \
    xlog_printk(ANDROID_LOG_INFO, TAG, fmt, ##args)
#define idle_dbg(fmt, args...)       \
    xlog_printk(ANDROID_LOG_DEBUG, TAG, fmt, ##args)
#define idle_ver(fmt, args...)       \
    xlog_printk(ANDROID_LOG_VERBOSE, TAG, fmt, ##args)

#else /* !USING_XLOG */

#define TAG     "[Power/swap] "

#define idle_err(fmt, args...)       \
    printk(KERN_ERR TAG);           \
    printk(KERN_CONT fmt, ##args) 
#define idle_warn(fmt, args...)      \
    printk(KERN_WARNING TAG);       \
    printk(KERN_CONT fmt, ##args)
#define idle_info(fmt, args...)      \
    printk(KERN_NOTICE TAG);        \
    printk(KERN_CONT fmt, ##args)
#define idle_dbg(fmt, args...)       \
    printk(KERN_INFO TAG);          \
    printk(KERN_CONT fmt, ##args)
#define idle_ver(fmt, args...)       \
    printk(KERN_DEBUG TAG);         \
    printk(KERN_CONT fmt, ##args)

#endif


#define idle_readl(addr) \
    DRV_Reg32(addr)

#define idle_writel(addr, val)   \
    mt65xx_reg_sync_writel(val, addr)

#define idle_setl(addr, val) \
    mt65xx_reg_sync_writel(idle_readl(addr) | (val), addr)

#define idle_clrl(addr, val) \
    mt65xx_reg_sync_writel(idle_readl(addr) & ~(val), addr)


bool __attribute__((weak)) 
clkmgr_idle_can_enter(unsigned int *condition_mask, unsigned int *block_mask)
{
    return false;
}

enum {
    IDLE_TYPE_MC = 0,
    IDLE_TYPE_DP = 1,
    IDLE_TYPE_SL = 2,
    IDLE_TYPE_RG = 3,
    NR_TYPES = 4,
};

enum {
    BY_CPU = 0,
    BY_CLK = 1,
    BY_OTH = 2,
    NR_REASONS = 3,
};

static const char *idle_name[NR_TYPES] = {
    "mcidle",
    "dpidle",
    "slidle",
    "rgidle",
};

static const char *reason_name[NR_REASONS] = {
    "by_cpu",
    "by_clk",
    "by_oth",
};

static int idle_switch[NR_TYPES] = {
    0,  //mcidle switch 
    1,  //dpidle switch
    1,  //slidle switch
    1,  //rgidle switch
};

/************************************************
 * multi-core idle part
 ************************************************/
bool mcidle_can_enter(void)
{
    return false;
}

static void mcidle_before_wfi(int cpu)
{
}

static void mcidle_after_wfi(int cpu)
{
}

static void go_to_mcidle(int cpu)
{
    mcidle_before_wfi(cpu);

    dsb();
    __asm__ __volatile__("wfi" ::: "memory");

    mcidle_after_wfi(cpu);
}


/************************************************
 * deep idle part
 ************************************************/
static unsigned int dpidle_condition_mask[NR_GRPS] = {
    0xFDC40DC1, //PERI0: i2c5~0, uart2~0, aphif, usb1~0, pwm7~5, nfi
    0x00000009, //PERI1: spi1, i2c6
    0x00000000, //INFRA:
    0x00000000, //TOPCK:
    0x01FFFFFF, //DISP0: all
    0x000003FF, //DISP1: all
    //0x00003FF5, //IMAGE: all
    0x00000000, //IMAGE: none
    0x0000000F, //MFG:   all
    0x00000040, //AUDIO: i2s
    0x00000001, //VDEC0: all
    0x00000001, //VDEC1: all
    0x00000001, //VENC:  all
};

static unsigned int dpidle_block_mask[NR_GRPS] = {0x0};

static unsigned long dpidle_cnt[NR_CPUS] = {0};
static unsigned long dpidle_block_cnt[NR_REASONS] = {0};

static DEFINE_MUTEX(dpidle_locked);
#define INVALID_GRP_ID(grp) (grp < 0 || grp >= NR_GRPS)

static void enable_dpidle_by_mask(int grp, unsigned int mask)
{
    mutex_lock(&dpidle_locked);
    dpidle_condition_mask[grp] &= ~mask;
    mutex_unlock(&dpidle_locked);
}

static void disable_dpidle_by_mask(int grp, unsigned int mask)
{
    mutex_lock(&dpidle_locked);
    dpidle_condition_mask[grp] |= mask;
    mutex_unlock(&dpidle_locked);
}

void enable_dpidle_by_bit(int id)
{
    int grp = id / 32;
    unsigned int mask = 1U << (id % 32);
    BUG_ON(INVALID_GRP_ID(grp));
    enable_dpidle_by_mask(grp, mask);
}
EXPORT_SYMBOL(enable_dpidle_by_bit);

void disable_dpidle_by_bit(int id)
{
    int grp = id / 32;
    unsigned int mask = 1U << (id % 32);
    BUG_ON(INVALID_GRP_ID(grp));
    disable_dpidle_by_mask(grp, mask);
}
EXPORT_SYMBOL(disable_dpidle_by_bit);

extern unsigned long localtimer_get_counter(void);
extern int localtimer_set_next_event(unsigned long evt);

static unsigned int timer_left;
static unsigned int timer_left2;
static unsigned int time_critera = 26000;

static bool dpidle_can_enter(void)
{
    int reason = NR_REASONS;
    //if ((smp_processor_id() != 0) || (num_online_cpus() != 1)) {
    if (atomic_read(&hotplug_cpu_count) != 1) {
        reason = BY_CPU;
        goto out;
    }

    memset(dpidle_block_mask, 0, NR_GRPS * sizeof(unsigned int));
    if (!clkmgr_idle_can_enter(dpidle_condition_mask, dpidle_block_mask)) {
        reason = BY_CLK;
        goto out;
    }

    timer_left = localtimer_get_counter();
    if (timer_left < time_critera || ((int)timer_left) < 0) {
        reason = BY_OTH;
        goto out;
    }

out:
    if (reason < NR_REASONS) {
        dpidle_block_cnt[reason]++;
        return false;
    } else {
        return true;
    }
}

bool spm_dpidle_can_enter(void)
{
    return true;
}

static unsigned int clk_cfg_1 = 0;

#define faudintbus_pll2sq() \
do {    \
    clk_cfg_1 = idle_readl(CLK_CFG_1);\
    idle_writel(CLK_CFG_1, clk_cfg_1 & 0xFFFFFCFF);  \
} while (0);

#define faudintbus_sq2pll() \
do {    \
    idle_writel(CLK_CFG_1, clk_cfg_1);  \
} while (0);


void spm_dpidle_before_wfi(void)
{
    faudintbus_pll2sq();

#if 0
    timer_left = localtimer_get_counter();
    gpt_set_cmp(GPT4, timer_left);
#else
    timer_left2 = localtimer_get_counter();
    gpt_set_cmp(GPT4, timer_left2);
#endif
    start_gpt(GPT4);
}

void spm_dpidle_after_wfi(void)
{
//    idle_info("[Sophie]timer_left=%u, timer_left2=%u, delta=%u\n", timer_left, timer_left2, timer_left-timer_left2);
    //if (gpt_check_irq(GPT4)) {
    if (gpt_check_and_ack_irq(GPT4)) {
        /* waked up by WAKEUP_GPT */
        localtimer_set_next_event(1);
    } else {
        /* waked up by other wakeup source */
        unsigned int cnt, cmp;
        gpt_get_cnt(GPT4, &cnt);
        gpt_get_cmp(GPT4, &cmp);
        if (unlikely(cmp < cnt)) {
            idle_err("[%s]GPT%d: counter = %10u, compare = %10u\n", __func__, 
                    GPT4 + 1, cnt, cmp);
            BUG();
        }

        localtimer_set_next_event(cmp-cnt);
        stop_gpt(GPT4);
        //GPT_ClearCount(WAKEUP_GPT);
    }

    faudintbus_sq2pll();

    dpidle_cnt[0]++;
}


/************************************************
 * slow idle part
 ************************************************/
static unsigned int slidle_condition_mask[NR_GRPS] = {
    0xFC001000, //PERI0: i2c5~0, dma 
    0x00000009, //PERI1: spi1, i2c6
    0x00000000, //INFRA:
    0x00000000, //TOPCK:
    0x00000000, //DISP0:
    0x00000000, //DISP1:
    0x00000000, //IMAGE:
    0x00000000, //MFG:
    0x00000004, //AUDIO: afe
    0x00000000, //VDEC0:
    0x00000000, //VDEC1:
    0x00000000, //VENC:
};

static unsigned int slidle_block_mask[NR_GRPS] = {0x0};

static unsigned long slidle_cnt[NR_CPUS] = {0};
static unsigned long slidle_block_cnt[NR_REASONS] = {0};
//static unsigned int slidle_block_mask[NR_GRPS];

static DEFINE_MUTEX(slidle_locked);


static void enable_slidle_by_mask(int grp, unsigned int mask)
{
    mutex_lock(&slidle_locked);
    slidle_condition_mask[grp] &= ~mask;
    mutex_unlock(&slidle_locked);
}

static void disable_slidle_by_mask(int grp, unsigned int mask)
{
    mutex_lock(&slidle_locked);
    slidle_condition_mask[grp] |= mask;
    mutex_unlock(&slidle_locked);
}

void enable_slidle_by_bit(int id)
{
    int grp = id / 32;
    unsigned int mask = 1U << (id % 32);
    BUG_ON(INVALID_GRP_ID(grp));
    enable_slidle_by_mask(grp, mask);
}
EXPORT_SYMBOL(enable_slidle_by_bit);

void disable_slidle_by_bit(int id)
{
    int grp = id / 32;
    unsigned int mask = 1U << (id % 32);
    BUG_ON(INVALID_GRP_ID(grp));
    disable_slidle_by_mask(grp, mask);
}
EXPORT_SYMBOL(disable_slidle_by_bit);

extern u32 ptp_data[3]; 
static bool slidle_can_enter(void)
{
    int reason = NR_REASONS;
    //if ((smp_processor_id() != 0) || (num_online_cpus() != 1)) {
    if (atomic_read(&hotplug_cpu_count) != 1) {
        reason = BY_CPU;
        goto out;
    }

    memset(slidle_block_mask, 0, NR_GRPS * sizeof(unsigned int));
    if (!clkmgr_idle_can_enter(slidle_condition_mask, slidle_block_mask)) {
        reason = BY_CLK;
        goto out;
    }
    
    if (ptp_data[0]) {
        reason = BY_OTH;
        goto out;
    }

out:
    if (reason < NR_REASONS) {
        slidle_block_cnt[reason]++;
        return false;
    } else {
        return true;
    }
}

static void slidle_before_wfi(int cpu)
{
    bus_dcm_enable();
}

static void slidle_after_wfi(int cpu)
{
    bus_dcm_disable();
    slidle_cnt[cpu]++;
}

static void go_to_slidle(int cpu)
{
    slidle_before_wfi(cpu);

    dsb();
    __asm__ __volatile__("wfi" ::: "memory");

    slidle_after_wfi(cpu);
}


/************************************************
 * regular idle part
 ************************************************/
static unsigned long rgidle_cnt[NR_CPUS] = {0};

static void rgidle_before_wfi(int cpu)
{
}

static void rgidle_after_wfi(int cpu)
{
    rgidle_cnt[cpu]++;
}

static void noinline go_to_rgidle(int cpu)
{
    rgidle_before_wfi(cpu);

    dsb();
    __asm__ __volatile__("wfi" ::: "memory");

    rgidle_after_wfi(cpu);
}

/************************************************
 * idle task flow part
 ************************************************/

/*
 * xxidle_handler return 1 if enter and exit the low power state
 */
static inline int mcidle_handler(int cpu)
{
    if (idle_switch[IDLE_TYPE_MC]) {
        if (mcidle_can_enter()) {
            go_to_mcidle(cpu);
            return 1;
        }
    } 

    return 0;
}

static int dpidle_cpu_pdn = 1;

static inline int dpidle_handler(int cpu)
{
    int ret = 0;
    if (idle_switch[IDLE_TYPE_DP]) {
        if (dpidle_can_enter()) {
            spm_go_to_dpidle(dpidle_cpu_pdn, 0);
            ret = 1;
        }
    }

    return ret;
}

static inline int slidle_handler(int cpu)
{
    int ret = 0;
    if (idle_switch[IDLE_TYPE_SL]) {
        if (slidle_can_enter()) {
            go_to_slidle(cpu);
            ret = 1;
        }
    }

    return ret;
}

static inline int rgidle_handler(int cpu)
{
    int ret = 0;
    if (idle_switch[IDLE_TYPE_RG]) {
        go_to_rgidle(cpu);
        ret = 1;
    }

    return ret;
}

static int (*idle_handlers[NR_TYPES])(int) = {
    mcidle_handler,
    dpidle_handler,
    slidle_handler,
    rgidle_handler,
};


void arch_idle(void)
{
    int cpu = smp_processor_id();
    int i;

    for (i = 0; i < NR_TYPES; i++) {
        if (idle_handlers[i](cpu))
            break;
    }
}

#define idle_attr(_name)                         \
static struct kobj_attribute _name##_attr = {   \
    .attr = {                                   \
        .name = __stringify(_name),             \
        .mode = 0644,                           \
    },                                          \
    .show = _name##_show,                       \
    .store = _name##_store,                     \
}

extern struct kobject *power_kobj;

static ssize_t dpidle_state_show(struct kobject *kobj, 
                struct kobj_attribute *attr, char *buf)
{
    int len = 0;
    char *p = buf;

    int i;

    p += sprintf(p, "*********** deep idle state ************\n");
    p += sprintf(p, "dpidle_cpu_pdn = %d\n", dpidle_cpu_pdn);
    for (i = 0; i < NR_REASONS; i++) {
        p += sprintf(p, "[%d]dpidle_block_cnt[%s]=%lu\n", i, reason_name[i], 
                dpidle_block_cnt[i]);
    }

    p += sprintf(p, "\n");


    for (i = 0; i < NR_GRPS; i++) {
        p += sprintf(p, "[%02d]dpidle_block_mask[%-8s]=0x%08x\n", i, 
                grp_get_name(i), dpidle_block_mask[i]);
    }

    p += sprintf(p, "dpidle help:   cat /sys/power/dpidle_state\n");
    p += sprintf(p, "switch on/off: echo [dpidle] 1/0 > /sys/power/dpidle_state\n");
    p += sprintf(p, "cpupdn on/off: echo cpupdn 1/0 > /sys/power/dpidle_state\n");
    p += sprintf(p, "en_dp_by_bit:  echo enable id > /sys/power/dpidle_state\n");
    p += sprintf(p, "dis_dp_by_bit: echo disable id > /sys/power/dpidle_state\n");

    len = p - buf;
    return len;
}

static ssize_t dpidle_state_store(struct kobject *kobj, 
                struct kobj_attribute *attr, const char *buf, size_t n)
{
    char cmd[32];
    int param;

    if (sscanf(buf, "%s %d", cmd, &param) == 2) {
        if (!strcmp(cmd, "dpidle")) {
            idle_switch[IDLE_TYPE_DP] = param;
        } else if (!strcmp(cmd, "enable")) {
            enable_dpidle_by_bit(param);
        } else if (!strcmp(cmd, "disable")) {
            disable_dpidle_by_bit(param);
        } else if (!strcmp(cmd, "cpupdn")) {
            dpidle_cpu_pdn = !!param;
        }
        return n;
    } else if (sscanf(buf, "%d", &param) == 1) {
        idle_switch[IDLE_TYPE_DP] = param;
        return n;
    }

    return -EINVAL;
}
idle_attr(dpidle_state);

static ssize_t slidle_state_show(struct kobject *kobj, 
                struct kobj_attribute *attr, char *buf)
{
    int len = 0;
    char *p = buf;

    int i;

    p += sprintf(p, "*********** slow idle state ************\n");
    for (i = 0; i < NR_REASONS; i++) {
        p += sprintf(p, "[%d]slidle_block_cnt[%s]=%lu\n", i, reason_name[i], slidle_block_cnt[i]);
    }

    p += sprintf(p, "\n");

    for (i = 0; i < NR_GRPS; i++) {
        p += sprintf(p, "[%2d]slidle_block_mask[%-8s]=0x%08x\n", i, 
                grp_get_name(i), slidle_block_mask[i]);
    }

    p += sprintf(p, "slidle help:   cat /sys/power/slidle_state\n");
    p += sprintf(p, "switch on/off: echo [slidle] 1/0 > /sys/power/slidle_state\n");

    len = p - buf;
    return len;
}

static ssize_t slidle_state_store(struct kobject *kobj, 
                struct kobj_attribute *attr, const char *buf, size_t n)
{
    char cmd[32];
    int param;

    if (sscanf(buf, "%s %d", cmd, &param) == 2) {
        if (!strcmp(cmd, "slidle")) {
            idle_switch[IDLE_TYPE_SL] = param;
        } else if (!strcmp(cmd, "enable")) {
            enable_slidle_by_bit(param);
        } else if (!strcmp(cmd, "disable")) {
            disable_slidle_by_bit(param);
        }
        return n;
    } else if (sscanf(buf, "%d", &param) == 1) {
        idle_switch[IDLE_TYPE_SL] = param;
        return n;
    }

    return -EINVAL;
}
idle_attr(slidle_state);

static ssize_t rgidle_state_show(struct kobject *kobj, 
                struct kobj_attribute *attr, char *buf)
{
    int len = 0;
    char *p = buf;

    p += sprintf(p, "*********** regular idle state ************\n");
    p += sprintf(p, "rgidle help:   cat /sys/power/rgidle_state\n");
    p += sprintf(p, "switch on/off: echo [rgidle] 1/0 > /sys/power/rgidle_state\n");

    len = p - buf;
    return len;
}

static ssize_t rgidle_state_store(struct kobject *kobj, 
                struct kobj_attribute *attr, const char *buf, size_t n)
{
    char cmd[32];
    int param;

    if (sscanf(buf, "%s %d", cmd, &param) == 2) {
        if (!strcmp(cmd, "rgidle")) {
            idle_switch[IDLE_TYPE_RG] = param;
        }
        return n;
    } else if (sscanf(buf, "%d", &param) == 1) {
        idle_switch[IDLE_TYPE_RG] = param;
        return n;
    }

    return -EINVAL;
}
idle_attr(rgidle_state);

static ssize_t idle_state_show(struct kobject *kobj, 
                struct kobj_attribute *attr, char *buf)
{
    int len = 0;
    char *p = buf;
    
    int i;

    p += sprintf(p, "********** idle state dump **********\n");
    for (i = 0; i < nr_cpu_ids; i++) {
        p += sprintf(p, "dpidle_cnt[%d]=%lu, slidle_cnt[%d]=%lu, rgidle_cnt[%d]=%lu\n", 
                i, dpidle_cnt[i], i, slidle_cnt[i], i, rgidle_cnt[i]);
    }
    
    p += sprintf(p, "\n********** variables dump **********\n");
    for (i = 0; i < NR_TYPES; i++) {
        p += sprintf(p, "%s_switch=%d, ", idle_name[i], idle_switch[i]);
    }
    p += sprintf(p, "\n");

    p += sprintf(p, "\n********** idle command help **********\n");
    p += sprintf(p, "status help:   cat /sys/power/idle_state\n");
    p += sprintf(p, "switch on/off: echo switch mask > /sys/power/idle_state\n");

    p += sprintf(p, "dpidle help:   cat /sys/power/dpidle_state\n");
    p += sprintf(p, "slidle help:   cat /sys/power/slidle_state\n");
    p += sprintf(p, "rgidle help:   cat /sys/power/rgidle_state\n");

    len = p - buf;
    return len;
}

static ssize_t idle_state_store(struct kobject *kobj, 
                struct kobj_attribute *attr, const char *buf, size_t n)
{
    char cmd[32];
    int idx;
    int param;

    if (sscanf(buf, "%s %x", cmd, &param) == 2) {
        if (!strcmp(cmd, "switch")) {
            for (idx = 0; idx < NR_TYPES; idx++) {
                idle_switch[idx] = (param & (1U << idx)) ? 1 : 0;
            }
        }
        return n;
    }

    return -EINVAL;
}
idle_attr(idle_state);


void mt_idle_init(void)
{
    int err = 0;
    
    idle_info("[%s]entry!!\n", __func__);
    arm_pm_idle = arch_idle;

    err = free_gpt(GPT1);
    if (err) {
        idle_info("[%s]fail to free GPT1\n", __func__);
    }
    
    err = request_gpt(GPT1, GPT_ONE_SHOT, GPT_CLK_SRC_SYS, GPT_CLK_DIV_1, 
                0, NULL, GPT_NOAUTOEN);
    if (err) {
        idle_info("[%s]fail to request GPT1\n", __func__);
    }

    err = request_gpt(GPT4, GPT_ONE_SHOT, GPT_CLK_SRC_SYS, GPT_CLK_DIV_1, 
                0, NULL, GPT_NOAUTOEN);
    if (err) {
        idle_info("[%s]fail to request GPT4\n", __func__);
    }

    err = request_gpt(GPT5, GPT_ONE_SHOT, GPT_CLK_SRC_SYS, GPT_CLK_DIV_1, 
                0, NULL, GPT_NOAUTOEN);
    if (err) {
        idle_info("[%s]fail to request GPT5\n", __func__);
    }

    err = sysfs_create_file(power_kobj, &idle_state_attr.attr);
    err |= sysfs_create_file(power_kobj, &dpidle_state_attr.attr);
    err |= sysfs_create_file(power_kobj, &slidle_state_attr.attr);
    err |= sysfs_create_file(power_kobj, &rgidle_state_attr.attr);

    if (err) {
        idle_err("[%s]: fail to create sysfs\n", __func__);
    }
}
