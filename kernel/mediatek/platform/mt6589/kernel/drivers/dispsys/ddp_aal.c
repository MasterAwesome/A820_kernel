#include <linux/sched.h>
#include <linux/wait.h>
#include "ddp_reg.h"
#include "ddp_drv.h"
#include "ddp_color.h"
#include "ddp_bls.h"

static DECLARE_WAIT_QUEUE_HEAD(g_disp_hist_alarm);

static unsigned int g_Alarm = 0;
static int g_AAL_NewFrameUpdate = 0;

/*
 *g_AAL_Param is protected by disp_set_needupdate
 */
static DISP_AAL_PARAM g_AAL_Param;
static int g_Configured = 0;
static int g_ColorIntFlag = 0;

static unsigned long g_PrevPWMDuty = 0;

unsigned long long g_PrevTime = 0;
unsigned long long g_CurrTime = 0;

int disp_wait_hist_update(unsigned long u4TimeOut_ms)
{
    int ret;

    ret = wait_event_interruptible(g_disp_hist_alarm , (1 == g_ColorIntFlag));
    g_ColorIntFlag = 0;

    return 0;
}


void disp_set_aal_alarm(unsigned int u4En)
{
    g_Alarm = u4En;
}

//Executed in ISR content
int disp_needWakeUp(void)
{
    return (g_AAL_NewFrameUpdate || g_Alarm) ? 1 : 0;
}

//Executed in ISR content
void on_disp_aal_alarm_set(void)
{
    if(disp_needWakeUp())
    {
        // enable interrupt
        //DISP_REG_SET((DISPSYS_COLOR_BASE + 0xf04), 0x00000007);
        DISP_REG_SET((DISPSYS_BLS_BASE + 0x10), 0x0000000F);
        g_AAL_NewFrameUpdate = 0;
    }
    else
    {
        // disable interrupt
        //DISP_REG_SET((DISPSYS_COLOR_BASE + 0xf04), 0x00000000);
        if (g_PrevPWMDuty == DISP_REG_GET(DISPSYS_BLS_BASE + 0x200))
            DISP_REG_SET((DISPSYS_BLS_BASE + 0x10), 0x00000000);
        else
            g_PrevPWMDuty = DISP_REG_GET(DISPSYS_BLS_BASE + 0x200);
    }
}

unsigned int is_disp_aal_alarm_on(void)
{
    return g_Alarm;
}

//Executed in ISR content
void disp_wakeup_aal(void)
{
    unsigned long long u8Delta = 0;
    g_CurrTime = sched_clock();
    u8Delta = (g_CurrTime > g_PrevTime ? (g_CurrTime - g_PrevTime) : g_CurrTime);
    
    if( 28000000 < u8Delta )
    {
        mb();
        g_ColorIntFlag = 1;
        wake_up_interruptible(&g_disp_hist_alarm);
        g_PrevTime = g_CurrTime;
    }

}


DISP_AAL_PARAM * get_aal_config()
{
    g_Configured = 1;
    return &g_AAL_Param;
}

int disp_is_aal_config()
{
    return g_Configured;
}

void disp_onConfig_aal(int i4FrameUpdate)
{
    if(0 == g_Configured)
    {
        return;
    }

    disp_onConfig_luma(g_AAL_Param.lumaCurve);

    disp_onConfig_bls(&g_AAL_Param);


    g_AAL_NewFrameUpdate = i4FrameUpdate;
}
