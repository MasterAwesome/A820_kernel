
#include "mtk_sysfreq.h"
#include "mach/mt_gpufreq.h"
#include "mach/mt_clkmgr.h"


#define MTK_GPU_DVFS 0

#if MTK_GPU_DVFS
static struct mt_gpufreq_info freqs_special_vrf18_2[] = {
    {GPU_DVFS_F3, 40, 100, GPU_POWER_VRF18_1_15V, 100},
    {GPU_DVFS_F5, 0,  40, GPU_POWER_VRF18_1_05V,  80},
};
static struct mt_gpufreq_info freqs_special2_vrf18_2[] = {
    {GPU_DVFS_F2, 60, 100, GPU_POWER_VRF18_1_15V, 100},
    {GPU_DVFS_F3, 30,  60, GPU_POWER_VRF18_1_15V,  90},
    {GPU_DVFS_F5, 0,  30, GPU_POWER_VRF18_1_05V,  75},
};
#endif

void MtkInitSetFreqTbl(void)
{
#if MTK_GPU_DVFS
    int reged=0;

    if(get_gpu_power_src()==0x0) // vrf18_2
    {
        switch((readl(0xf0009040) >> 28))
        {
        case 5:
            printk("[GPU DVFS] register vrf18_2 special table ...\n");
            mt_gpufreq_register(freqs_special_vrf18_2, 2);
            reged = 1;
            break;
        case 6:
            printk("[GPU DVFS] register vrf18_2 special2 table ...\n");
            mt_gpufreq_register(freqs_special2_vrf18_2, 3);
            reged = 1;
            break;
        }
    }

    if (reged == 0)
#endif
    {
        mt_gpufreq_non_register();
    }
}


PVRSRV_ERROR MTKSetFreqInfo(unsigned int freq)
{
#if defined(MTK_FREQ_INIT)

#if defined(MTK_FORCE_M)
    freq = GPU_DVFS_F8;
#endif

//    printk("[test test] freq= %d", freq);
    mt_gpufreq_set_initial(freq, GPU_POWER_VRF18_1_05V);
#endif

    MtkInitSetFreqTbl();

    return PVRSRV_OK;
}
