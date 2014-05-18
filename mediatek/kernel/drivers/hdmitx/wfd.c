/*****************************************************************************/
/* Copyright (c) 2009 NXP Semiconductors BV                                  */
/*                                                                           */
/* This program is free software; you can redistribute it and/or modify      */
/* it under the terms of the GNU General Public License as published by      */
/* the Free Software Foundation, using version 2 of the License.             */
/*                                                                           */
/* This program is distributed in the hope that it will be useful,           */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of            */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the              */
/* GNU General Public License for more details.                              */
/*                                                                           */
/* You should have received a copy of the GNU General Public License         */
/* along with this program; if not, write to the Free Software               */
/* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307       */
/* USA.                                                                      */
/*                                                                           */
/*****************************************************************************/
#if defined(MTK_WFD_SUPPORT)

#define _tx_c_
#include <linux/autoconf.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/earlysuspend.h>
#include <linux/kthread.h>
#include <linux/rtpm_prio.h>
#include <linux/vmalloc.h>
#include <linux/disp_assert_layer.h>

#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <asm/mach-types.h>
#include <asm/cacheflush.h>
#include <asm/io.h>

#include <mach/dma.h>
#include <mach/irqs.h>

#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/cdev.h>
#include <asm/tlbflush.h>
#include <asm/page.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/slab.h>

#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/earlysuspend.h>
#include <linux/kthread.h>
#include <linux/rtpm_prio.h>
#include <linux/list.h>

#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <asm/mach-types.h>
#include <asm/cacheflush.h>
#include <asm/io.h>

#include <mach/dma.h>
#include <mach/irqs.h>
#include <linux/vmalloc.h>

#include <asm/uaccess.h>

#include "hdmi_drv.h"

#include "disp_drv_platform.h"
#include "ddp_reg.h"
#include "ddp_dpfd.h"
#include "ddp_drv.h"

#include "dpi_drv.h"
#include "dpi1_drv.h"

//#include "lcd_reg.h"
#include "dsi_drv.h"
#include "dpi_reg.h"
#include "mach/eint.h"
#include "mach/irqs.h"
#include "hdmitx.h"

#include <linux/switch.h>


#define RETIF(cond, rslt)       if ((cond)){HDMI_LOG("return in %d\n",__LINE__);return (rslt);}
#define RET_VOID_IF(cond)       if ((cond)){HDMI_LOG("return in %d\n",__LINE__);return;}
#define RETIF_NOLOG(cond, rslt)       if ((cond)){return (rslt);}
#define RET_VOID_IF_NOLOG(cond)       if ((cond)){return;}
#define RETIFNOT(cond, rslt)    if (!(cond)){HDMI_LOG("return in %d\n",__LINE__);return (rslt);}

#define ALIGN_TO(x, n)  \
	(((x) + ((n) - 1)) & ~((n) - 1))

#include <mach/m4u.h>
#include <mach/mt_typedefs.h>
//#include <mach/mt_gpio.h>

extern int m4u_config_port(M4U_PORT_STRUCT* pm4uport);
extern void HDMI_DBG_Init(void);
extern bool mtkfb_is_suspend(void);
extern UINT32 DISP_GetScreenHeight(void);
extern UINT32 DISP_GetScreenWidth(void);
extern int disp_lock_mutex(void);
extern int disp_unlock_mutex(int id);
extern 	void hdmi_dsi_waitnotbusy(void);

static size_t hdmi_log_on = 1;
static struct switch_dev hdmi_switch_data;
#define HDMI_LOG(fmt, arg...) \
		do { \
				if (hdmi_log_on) {printk("[hdmi]%s,#%d ", __func__, __LINE__); printk(fmt, ##arg);} \
		}while (0)

#define HDMI_FUNC()	\
		do { \
				if(hdmi_log_on) printk("[hdmi] %s\n", __func__); \
		}while (0)

#define HDMI_LINE()	\
		do { \
				if (hdmi_log_on) {printk("[hdmi]%s,%d ", __func__, __LINE__); printk(fmt, ##arg);} \
		}while (0)

//extern int pll_fsel(enum mt65xx_pll_id id, unsigned int pll_value);
#define HDMI_DEVNAME "hdmitx"
HDMI_PARAMS _s_hdmi_params = {0};
HDMI_PARAMS *hdmi_params = &_s_hdmi_params;
HDMI_DRIVER *hdmi_drv = NULL;

static struct timeval  timestamp[16];

// xuecheng, this is for exernal display driver blocking "get buffer" method
static atomic_t wfd_buffer_update_flag = ATOMIC_INIT(0);
#define WFD_PUT_NEW_BUFFER() atomic_set(&wfd_buffer_update_flag, 1)
#define WFD_CLEAR_NEW_BUFFER() atomic_set(&wfd_buffer_update_flag, 0)
#define WFD_GET_NEW_BUFFER() atomic_read(&wfd_buffer_update_flag)

static wait_queue_head_t external_display_getbuffer_wq;

void hdmi_log_enable(int enable)
{
		printk("hdmi log %s\n", enable?"enabled":"disabled");
		hdmi_log_on = enable;
		hdmi_drv->log_enable(enable);
}

static DEFINE_SEMAPHORE(hdmi_update_mutex);
typedef struct{
		bool is_wfd;
		bool is_wfd_suspend;
		bool is_ipo_poweroff;   // IPO power off or not, like hdmi_suspend(), hdmi_resume()
		bool is_force_fullscreen;    // whether memory is forced to be rotated in Camera, no need to set orientation again
		bool is_force_portrait; // whether forced portrait mode, priority is higher than is_force_fullscreen
		bool is_reconfig_needed;    // whether need to reset HDMI memory
		bool is_enabled;    // whether HDMI is enabled or disabled by user
		//bool is_active;     // whether HDMI is actived with cable pluged in.
		bool is_force_disable; 		//used for camera scenario.
		bool is_clock_on;   // DPI is running or not
		bool is_factory_mode;
		atomic_t state; // HDMI_POWER_STATE state
		bool is_audio_avaliable;
		int 	lcm_width;  // LCD write buffer width
		int		lcm_height; // LCD write buffer height
		int		hdmi_width; // DPI read buffer width
		int		hdmi_height; // DPI read buffer height
		HDMI_VIDEO_RESOLUTION		output_video_resolution;
		HDMI_AUDIO_FORMAT           output_audio_format;
		int		orientation;    // MDP's orientation, 0 means 0 degree, 1 means 90 degree, 2 means 180 degree, 3 means 270 degree
		int     orientation_store;    // Store orientation setting when HDMI driver is_force_fullscreen
		int     orientation_store_portrait;	    // Store orientation setting when HDMI driver is_force_portrait
}_t_hdmi_context;

struct hdmi_video_buffer_list {
		struct hdmi_video_buffer_info buffer_info;
		pid_t  pid;
		void*  file_addr;
		unsigned int buffer_mva;
		struct list_head list;
};

static struct list_head hdmi_video_mode_buffer_list;
//static struct list_head *hdmi_video_buffer_list_head = &hdmi_video_mode_buffer_list;
DEFINE_SEMAPHORE(hdmi_video_mode_mutex);
static atomic_t hdmi_video_mode_flag = ATOMIC_INIT(0);
//static int hdmi_add_video_buffer(struct hdmi_video_buffer_info *buffer_info, struct file *file);
//static struct hdmi_video_buffer_list* hdmi_search_video_buffer(struct hdmi_video_buffer_info *buffer_info, struct file *file);
//static void hdmi_destory_video_buffer(void);
#define IS_HDMI_IN_VIDEO_MODE()        atomic_read(&hdmi_video_mode_flag)
#define SET_HDMI_TO_VIDEO_MODE()       atomic_set(&hdmi_video_mode_flag, 1)
#define SET_HDMI_LEAVE_VIDEO_MODE()    atomic_set(&hdmi_video_mode_flag, 0)
static wait_queue_head_t hdmi_video_mode_wq;
static atomic_t hdmi_video_mode_event = ATOMIC_INIT(0);
static atomic_t hdmi_video_mode_dpi_change_address = ATOMIC_INIT(0);
#define IS_HDMI_VIDEO_MODE_DPI_IN_CHANGING_ADDRESS()    atomic_read(&hdmi_video_mode_dpi_change_address)
#define SET_HDMI_VIDEO_MODE_DPI_CHANGE_ADDRESS()        atomic_set(&hdmi_video_mode_dpi_change_address, 1)
#define SET_HDMI_VIDEO_MODE_DPI_CHANGE_ADDRESS_DONE()   atomic_set(&hdmi_video_mode_dpi_change_address, 0)


static _t_hdmi_context hdmi_context;
static _t_hdmi_context *p = &hdmi_context;

#define IS_HDMI_ON()			(HDMI_POWER_STATE_ON == atomic_read(&p->state))
#define IS_HDMI_OFF()			(HDMI_POWER_STATE_OFF == atomic_read(&p->state))
#define IS_HDMI_STANDBY()	    (HDMI_POWER_STATE_STANDBY == atomic_read(&p->state))

#define IS_HDMI_NOT_ON()		(HDMI_POWER_STATE_ON != atomic_read(&p->state))
#define IS_HDMI_NOT_OFF()		(HDMI_POWER_STATE_OFF != atomic_read(&p->state))
#define IS_HDMI_NOT_STANDBY()	(HDMI_POWER_STATE_STANDBY != atomic_read(&p->state))

#define SET_HDMI_ON()	        atomic_set(&p->state, HDMI_POWER_STATE_ON)
#define SET_HDMI_OFF()	        atomic_set(&p->state, HDMI_POWER_STATE_OFF)
#define SET_HDMI_STANDBY()	    atomic_set(&p->state, HDMI_POWER_STATE_STANDBY)

extern int m4u_alloc_mva_stub(M4U_MODULE_ID_ENUM eModuleID, const unsigned int BufAddr, const unsigned int BufSize, unsigned int *pRetMVABuf);
extern int m4u_dealloc_mva_stub(M4U_MODULE_ID_ENUM eModuleID, const unsigned int BufAddr, const unsigned int BufSize, const unsigned int MVA);
extern int m4u_config_port_stub(M4U_PORT_STRUCT* pM4uPort);

extern int m4u_insert_tlb_range_stub(M4U_MODULE_ID_ENUM eModuleID, unsigned int MVAStart, const unsigned int MVAEnd, M4U_RANGE_PRIORITY_ENUM ePriority, unsigned int entryCount);
extern int m4u_invalid_tlb_range_stub(M4U_MODULE_ID_ENUM eModuleID, unsigned int MVAStart, unsigned int MVAEnd);
// TODO: refine to a single structure
static unsigned int overlay_dst_mva, overlay_dst_va, ddp_dst_va, ddp_dst_mva /*, fb_pa, fb_va, fb_size*/;

static dev_t hdmi_devno;
static struct cdev *hdmi_cdev;
static struct class *hdmi_class = NULL;

static UINT32 const DPI_PAD_CON = 0xf2080900;
static UINT32 const NLI_ARB_CS = 0xf100d014;

static int hdmi_bpp = 4;

static int hdmi_default_width = 1280;
static int hdmi_default_height = 720;
static int hdmi_temp_buffer_number = 0;

static int hdmi_buffer_write_id = 0;
static int hdmi_buffer_read_id = 0;
static int hdmi_buffer_lcdw_id = 0;
static struct timeval  timestamp[16];

static struct task_struct *hdmi_update_task = NULL;
static struct task_struct *hdmi_overlay_config_task = NULL;



static wait_queue_head_t hdmi_update_wq;
static wait_queue_head_t hdmi_overlay_config_wq;

static wait_queue_head_t external_display_getbuffer_wq;
static atomic_t hdmi_update_event = ATOMIC_INIT(0);
static atomic_t hdmi_overlay_config_event = ATOMIC_INIT(0);
static int wfd_pattern_output = 0;
static int wfd_pattern_output_index = 0;

#include <linux/mmprofile.h>
struct WFD_MMP_Events_t
{
	MMP_Event WFD;
    MMP_Event Rotate;
    MMP_Event UpdateRequest;
    MMP_Event DDPKBitblt;		
    MMP_Event Getbuffer;
    MMP_Event OverlayDone;
    MMP_Event WFDEnter;
    MMP_Event WFDLeave;
    MMP_Event WFDStart;
    MMP_Event WFDStop;
} WFD_MMP_Events;


// ---------------------------------------------------------------------------
//  Information Dump Routines
// ---------------------------------------------------------------------------

void init_wfd_mmp_events(void)
{
    if (WFD_MMP_Events.WFD == 0)
    {    
	WFD_MMP_Events.WFD = MMProfileRegisterEvent(MMP_RootEvent, "WFD");
        WFD_MMP_Events.Rotate = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "Rotate");
        WFD_MMP_Events.OverlayDone = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "OverlayDone");
        WFD_MMP_Events.UpdateRequest = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "UpdateRequest");
        WFD_MMP_Events.DDPKBitblt = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "DDPKBitblt");
        WFD_MMP_Events.Getbuffer = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "Getbuffer");
        WFD_MMP_Events.WFDEnter = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "WFDEnter");
        WFD_MMP_Events.WFDLeave = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "WFDLeave");
        WFD_MMP_Events.WFDStart = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "WFDStart");
        WFD_MMP_Events.WFDStop = MMProfileRegisterEvent(WFD_MMP_Events.WFD, "WFDStop");
    	MMProfileEnableEventRecursive(WFD_MMP_Events.WFD, 1);
    }
}



static void hdmi_udelay(unsigned int us)
{
		udelay(us);
}

static void hdmi_mdelay(unsigned int ms)
{
		msleep(ms);
}


/* Will be called in LCD Interrupt handler to check whether HDMI is actived */
bool is_hdmi_active(void)
{
		return IS_HDMI_ON();
}

#if 0
HDMI_STATUS hdmi_config_overlay_to_memory(unsigned int mva, int enable)
{
		int ret = 0;

		struct disp_path_config_mem_out_struct mem_out = {0};

		if(enable)
		{
			mem_out.outFormat = WDMA_OUTPUT_FORMAT_RGB888;

			mem_out.enable = 1;
			mem_out.dstAddr = mva;
			mem_out.srcROI.x = 0;
			mem_out.srcROI.y = 0;
			mem_out.srcROI.height= DISP_GetScreenHeight();
			mem_out.srcROI.width= DISP_GetScreenWidth();
			mutex_lock(&MemOutSettingMutex);

			disp_path_config_mem_out(&mem_out);
			MemOutConfig.dirty = 1;
			mutex_unlock(&MemOutSettingMutex);

		}
		else
		{
			mem_out.outFormat = WDMA_OUTPUT_FORMAT_RGB888;
	
			mem_out.enable = 0;
			mem_out.dstAddr = mva;
			mem_out.srcROI.x = 0;
			mem_out.srcROI.y = 0;
			mem_out.srcROI.height= DISP_GetScreenHeight();
			mem_out.srcROI.width= DISP_GetScreenWidth();
	
			hdmi_dsi_waitnotbusy();
			disp_path_get_mutex();
			disp_path_config_mem_out(&mem_out);
	
			// TODO: other mode?
			//	if(cmd_mode)
			DSI_EnableClk();
	
			disp_path_release_mutex();
		}
}
#endif

/* Used for HDMI Driver update */
static int hdmi_update_kthread(void *data)
{
		struct sched_param param = { .sched_priority = RTPM_PRIO_SCRN_UPDATE };
		sched_setscheduler(current, SCHED_RR, &param);

		for( ;; ) {
			
				wait_event_interruptible(hdmi_update_wq, atomic_read(&hdmi_update_event));
				//HDMI_LOG("wq wakeup\n");
				//hdmi_update_impl();

				atomic_set(&hdmi_update_event,0);

				hdmi_update_impl();
				

				
				if (kthread_should_stop())
						break;
		}

		return 0;
}

/* Used for HDMI Driver update */
static int hdmi_overlay_config_kthread(void *data)
{
		unsigned int addr = 0;
		struct sched_param param = { .sched_priority = RTPM_PRIO_SCRN_UPDATE };
		sched_setscheduler(current, SCHED_RR, &param);

		for( ;; ) {
			
				wait_event_interruptible(hdmi_overlay_config_wq, atomic_read(&hdmi_overlay_config_event));
				addr = overlay_dst_mva+hdmi_buffer_lcdw_id*p->lcm_width*p->lcm_height*3;
				DISP_Config_Overlay_to_Memory(addr, 1);

				atomic_set(&hdmi_overlay_config_event, 0);
				if (kthread_should_stop())
						break;
		}

		return 0;
}

/* Switch LCD write buffer, will be called in LCD Interrupt handler */
void hdmi_source_buffer_switch(void)
{
		//printk("lcd write buffer:%d\n", hdmi_buffer_lcdw_id);

		RET_VOID_IF_NOLOG(IS_HDMI_NOT_ON());
		RET_VOID_IF_NOLOG(IS_HDMI_IN_VIDEO_MODE());
				
		hdmi_buffer_lcdw_id = (hdmi_buffer_lcdw_id+1)%hdmi_temp_buffer_number;
		MMProfileLog(WFD_MMP_Events.OverlayDone, MMProfileFlagStart);

		//atomic_set(&hdmi_overlay_config_event, 1);
		//wake_up_interruptible(&hdmi_overlay_config_wq);
	
		DISP_Config_Overlay_to_Memory(overlay_dst_mva+ p->lcm_width*p->lcm_height*3*hdmi_buffer_lcdw_id, 1);
		//LCD_CHECK_RET(LCD_FBSetAddress(LCD_FB_0, temp_mva + p->lcm_width*p->lcm_height*3*hdmi_buffer_lcdw_id));
		MMProfileLog(WFD_MMP_Events.OverlayDone, MMProfileFlagEnd);
}

/* Switch DPI read buffer, will be called in DPI Interrupt handler */
void hdmi_update_buffer_switch(void)
{
		//HDMI_LOG("DPI read buffer:%d\n", hdmi_buffer_read_id);

		RET_VOID_IF_NOLOG(IS_HDMI_NOT_ON());

		if(IS_HDMI_IN_VIDEO_MODE())
		{
				if (IS_HDMI_VIDEO_MODE_DPI_IN_CHANGING_ADDRESS())
				{
						if (0 == *((volatile unsigned int*)(0xF208C020)))
						{
								SET_HDMI_VIDEO_MODE_DPI_CHANGE_ADDRESS_DONE();
								atomic_set(&hdmi_video_mode_event, 1);
								wake_up_interruptible(&hdmi_video_mode_wq);
						}
				}
		}
		else
		{
				//DPI_CHECK_RET(DPI_FBSetAddress(DPI_FB_0, hdmi_mva + p->hdmi_width*p->hdmi_height*3*hdmi_buffer_read_id));
				//hdmi_buffer_read_id = (hdmi_buffer_read_id + 1) % hdmi_temp_buffer_number;
		}
}

extern void DBG_OnTriggerHDMI(void);
extern void DBG_OnHDMIDone(void);

/* hdmi update api, will be called in LCD Interrupt handler */
void hdmi_update(void)
{
		//HDMI_FUNC();
#if 1
		RET_VOID_IF(IS_HDMI_NOT_ON());
		RET_VOID_IF(!p->is_wfd);
		
		MMProfileLog(WFD_MMP_Events.UpdateRequest, MMProfileFlagStart);
		atomic_set(&hdmi_update_event, 1);
		wake_up_interruptible(&hdmi_update_wq); //wake up hdmi_update_kthread() to do update
		MMProfileLog(WFD_MMP_Events.UpdateRequest, MMProfileFlagEnd);
		
#else
		hdmi_update_impl();
#endif
}

static long int get_current_time_us(void)
{
		struct timeval t;
		do_gettimeofday(&t);
		return (t.tv_sec & 0xFFF) * 1000000 + t.tv_usec;
}

static int ovl_dst_buffer_size = 0;
static int ddp_dst_buffer_size = 0;		

void hdmi_update_impl(void)
{
		//HDMI_LOG("hdmi_update_impl\n");

		int ret = 0;
		DdpkBitbltConfig pddp;
		int lcm_physical_rotation = 0;
		int bpp = 4;
		int pixelSize =  p->hdmi_width * p->hdmi_height;
		int dataSize = pixelSize * bpp;

		HDMI_FUNC();
		
		if(wfd_pattern_output) return;

		if(pixelSize == 0)
		{
				HDMI_LOG("ignored[resolution is null]\n");
				return;
		}

		//HDMI_FUNC();
		if(down_interruptible(&hdmi_update_mutex))
		{
				HDMI_LOG("[HDMI] can't get semaphore in\n");
				return;
		}

		if (IS_HDMI_NOT_ON())
		{
				goto done;
		}
		if(IS_HDMI_IN_VIDEO_MODE())
		{
				goto done;
		}

		DBG_OnTriggerHDMI();
		//LCD_WaitForNotBusy();
		//hdmi_buffer_write_id = (hdmi_buffer_write_id+1) % hdmi_temp_buffer_number;

		if(p->is_reconfig_needed || p->is_wfd_suspend)
		{
				int i = 0;
				for(i=0;i<hdmi_temp_buffer_number;i++)
				{
					memset((void*)(ddp_dst_va+i*p->hdmi_width*p->hdmi_height*3), 0x00,p->hdmi_width*p->hdmi_height);
					memset((void*)(ddp_dst_va+i*p->hdmi_width*p->hdmi_height*3+p->hdmi_width*p->hdmi_height), 0x80, p->hdmi_width*p->hdmi_height);
				}
				if(p->is_reconfig_needed)
				{
					p->is_reconfig_needed = false;
				}
		}

		if(p->is_wfd_suspend)
		{	
			MMProfileLogEx(WFD_MMP_Events.DDPKBitblt, MMProfileFlagStart, hdmi_buffer_write_id, 0);
			hdmi_buffer_write_id = ((hdmi_buffer_write_id+1)%hdmi_temp_buffer_number);

			WFD_PUT_NEW_BUFFER();
			wake_up_interruptible(&external_display_getbuffer_wq);
			MMProfileLogEx(WFD_MMP_Events.DDPKBitblt, MMProfileFlagEnd, hdmi_buffer_write_id, 0);
			up(&hdmi_update_mutex);
			return;
		}

		memset((void*)&pddp, 0, sizeof(DdpkBitbltConfig));
		pddp.srcX = pddp.srcY = 0;
		pddp.srcW = p->lcm_width;
		pddp.srcH	= p->lcm_height;
		pddp.srcWStride = p->lcm_width;
		pddp.srcHStride = p->lcm_height;

		pddp.srcFormat = eRGB888_K;

		pddp.srcAddr[0] = overlay_dst_mva+ p->lcm_width*p->lcm_height*3*((hdmi_buffer_lcdw_id+(hdmi_temp_buffer_number-1))%hdmi_temp_buffer_number);
		//pddp.srcAddr[0] = overlay_dst_va;
		pddp.srcAddr[1] = 0;
		pddp.srcAddr[2] = 0;


		pddp.srcBufferSize[0] = p->lcm_width*p->lcm_height*3;
		pddp.srcBufferSize[1] = 0;
		pddp.srcBufferSize[2] = 0;
		pddp.srcPlaneNum = 1;
		pddp.srcMemType =  DISP_MEMTYPE_MVA;
		pddp.orientation = p->orientation;

		#if 0
		// xuecheng, this is for IT only
		{
			MMP_MetaDataBitmap_t Bitmap;
			Bitmap.data1 = hdmi_buffer_lcdw_id;
			Bitmap.width = p->lcm_width;
			Bitmap.height = p->lcm_height;
			Bitmap.format = MMProfileBitmapBGR888;
			Bitmap.start_pos = 0;
			Bitmap.pitch = p->lcm_width*3;
			Bitmap.data_size = Bitmap.pitch * Bitmap.height;
			Bitmap.down_sample_x = 6;
			Bitmap.down_sample_y = 6;
			Bitmap.pData = (void*) overlay_dst_va+ p->lcm_width*p->lcm_height*3*((hdmi_buffer_lcdw_id+(hdmi_temp_buffer_number-1))%hdmi_temp_buffer_number);
			Bitmap.bpp = 24; 
			MMProfileLogMetaBitmap(WFD_MMP_Events.DDPKBitblt, MMProfileFlagStart, &Bitmap);
		}
		#else
		MMProfileLogEx(WFD_MMP_Events.DDPKBitblt, MMProfileFlagStart, hdmi_buffer_write_id, 0);
		#endif

	switch(pddp.orientation)
	{
		case 0:
		case 180:
		{
			// make sure in portrait mode, MDP resize must be as propotion
			// that is p.dstW/p.dstH == lcm_width/lcm_height
			pddp.dstX = pddp.dstY = 0;
			pddp.dstW = ALIGN_TO(p->lcm_width * p->hdmi_height / p->lcm_height, 4);
			pddp.dstH = ALIGN_TO(p->hdmi_height,4);

			if(p->is_wfd)
			{
				pddp.dstFormat = eYUV_420_3P_K;
			}
			else
			{
				pddp.dstFormat = eRGB888_K;
			}

			// setting memory write start address
			if(pddp.dstFormat == eRGB888_K)
			{
				pddp.dstAddr[0] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3 + 
							(p->hdmi_height - pddp.dstH) / 2 * p->hdmi_width * 3 +
							(p->hdmi_width - pddp.dstW) / 2 * 3;
				pddp.dstAddr[1] = 0;
				pddp.dstAddr[2] = 0;
				pddp.dstBufferSize[0] = p->hdmi_width*p->hdmi_height*3;
				pddp.dstBufferSize[1] = 0;
				pddp.dstBufferSize[2] = 0;
				pddp.dstPlaneNum = 1;							
			}
	else if(pddp.dstFormat == eYUV_420_3P_K)
	{
		pddp.dstAddr[0] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3 +
					(p->hdmi_height - pddp.dstH) / 2 * p->hdmi_width +
					(p->hdmi_width - pddp.dstW) / 2;
		pddp.dstAddr[1] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3 + 
					p->hdmi_width*p->hdmi_height +
					(p->hdmi_height - pddp.dstH) / 2 * p->hdmi_width / 4 +
					(p->hdmi_width - pddp.dstW) / 2 / 2;
		pddp.dstAddr[2] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3 + 
					p->hdmi_width*p->hdmi_height /4 * 5 +
					(p->hdmi_height - pddp.dstH) / 2 * p->hdmi_width / 4 +
					(p->hdmi_width - pddp.dstW) / 2 / 2;
		pddp.dstBufferSize[0] = p->hdmi_width*p->hdmi_height;
		pddp.dstBufferSize[1] = p->hdmi_width*p->hdmi_height /4;
		pddp.dstBufferSize[2] = p->hdmi_width*p->hdmi_height /4;
		pddp.dstPlaneNum = 3;				
	}

	pddp.pitch = p->hdmi_width;
	pddp.dstWStride = p->hdmi_width;
	pddp.dstHStride = p->hdmi_height;

	pddp.dstMemType =  DISP_MEMTYPE_MVA;
				break;
			}
			case 90:
			case 270:
			{
				pddp.dstX = pddp.dstY = 0;
				pddp.dstW = p->hdmi_width;
				pddp.dstH = p->hdmi_height;

				if(p->is_wfd)
				{
					pddp.dstFormat = eYUV_420_3P_K;
				}
				else
				{
					pddp.dstFormat = eRGB888_K;
				}

				if(pddp.dstFormat == eRGB888_K)
				{
					pddp.dstAddr[0] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3 + 
										(p->hdmi_height - pddp.dstH) / 2 * p->hdmi_width * 3 +
										(p->hdmi_width - pddp.dstW) / 2 * 3;
					pddp.dstAddr[1] = 0;
					pddp.dstAddr[2] = 0;						
					pddp.dstBufferSize[0] = p->hdmi_width*p->hdmi_height*3;
					pddp.dstBufferSize[1] = 0;
					pddp.dstBufferSize[2] = 0;
					pddp.dstPlaneNum = 1;				
				}
				else if(pddp.dstFormat == eYUV_420_3P_K)
				{
					pddp.dstAddr[0] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3;
					pddp.dstAddr[1] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3 + p->hdmi_width*p->hdmi_height;
					pddp.dstAddr[2] = ddp_dst_mva+ hdmi_buffer_write_id * p->hdmi_width * p->hdmi_height * 3 + p->hdmi_width*p->hdmi_height/4*5;
					pddp.dstBufferSize[0] = p->hdmi_width*p->hdmi_height;
					pddp.dstBufferSize[1] = p->hdmi_width*p->hdmi_height /4;
					pddp.dstBufferSize[2] = p->hdmi_width*p->hdmi_height /4;
					pddp.dstPlaneNum = 3;				
				}

				pddp.pitch = p->hdmi_width;
				pddp.dstWStride = p->hdmi_width;
				pddp.dstHStride = p->hdmi_height;			

				pddp.dstMemType =  DISP_MEMTYPE_MVA;
				break;
			}
		}

		//HDMI_LOG("dstw=%d, dsth=%d, ori=%d\n", p.dstW, p.dstH, p.orientation);
		do_gettimeofday(&timestamp[hdmi_buffer_write_id]);
		HDMI_LOG("ts:sec=%d, nsec=%d\n", timestamp[hdmi_buffer_write_id].tv_sec, timestamp[hdmi_buffer_write_id].tv_usec);

		long int profile_t = get_current_time_us();

		ret = DDPK_Bitblt_Config( DDPK_CH_HDMI_0, &pddp );
		if(ret)
		{
				HDMI_LOG("DDPK_Bitblt fail!, ret=%d\n", ret);
		}

		ret = DDPK_Bitblt( DDPK_CH_HDMI_0 );
		if(ret)
		{
				HDMI_LOG("DDPK_Bitblt fail!, ret=%d\n", ret);
		}
		HDMI_LOG("profile: %dms\n", (get_current_time_us()-profile_t)/1000);
		
		hdmi_buffer_write_id = ((hdmi_buffer_write_id+1)%hdmi_temp_buffer_number);
		HDMI_LOG("after bitblt, src addr = 0x%08x, dst addr = 0x%08x, write_id=%d\n", pddp.srcAddr[0], pddp.dstAddr[0], hdmi_buffer_write_id);

		WFD_PUT_NEW_BUFFER();
		wake_up_interruptible(&external_display_getbuffer_wq);
		MMProfileLogEx(WFD_MMP_Events.DDPKBitblt, MMProfileFlagEnd, hdmi_buffer_write_id, 0);

done:
		up(&hdmi_update_mutex);

		return;
}




/* Allocate memory, set M4U, LCD, MDP, DPI */
/* LCD overlay to memory -> MDP resize and rotate to memory -> DPI read to HDMI */
/* Will only be used in ioctl(MTK_HDMI_AUDIO_VIDEO_ENABLE) */
static HDMI_STATUS hdmi_drv_init(void)
{
		int ret = 0;
		int lcm_width = 0;
		int lcm_height = 0;
		M4U_PORT_STRUCT portStruct;

		HDMI_FUNC();

		lcm_width = DISP_GetScreenWidth();
		lcm_height = DISP_GetScreenHeight();

		p->lcm_width = lcm_width;
		p->lcm_height = lcm_height;

		HDMI_LOG("lcm_width=%d, lcm_height=%d\n", lcm_width, lcm_height);

		ret = m4u_alloc_mva(M4U_CLNTMOD_WDMA, 
						overlay_dst_va, 
						ovl_dst_buffer_size, 
						0,
						0,
						&overlay_dst_mva);
		if(ret!=0)
		{
				printk("m4u_alloc_mva() fail! \n");
				return HDMI_STATUS_OK;	
		}

		m4u_dma_cache_maint(M4U_CLNTMOD_WDMA, 
						overlay_dst_va, 
						ovl_dst_buffer_size,
						DMA_BIDIRECTIONAL);

		ret = m4u_alloc_mva(M4U_CLNTMOD_WDMA, 
						ddp_dst_va, 
						ddp_dst_buffer_size, 
						0,
						0,
						&ddp_dst_mva);
		if(ret!=0)
		{
				printk("m4u_alloc_mva() fail! \n");
				return HDMI_STATUS_OK;	
		}

		m4u_dma_cache_maint(M4U_CLNTMOD_WDMA, 
						ddp_dst_va, 
						ddp_dst_buffer_size,
						DMA_BIDIRECTIONAL);

		portStruct.ePortID = M4U_PORT_WDMA1;			 //hardware port ID, defined in M4U_PORT_ID_ENUM
		portStruct.Virtuality = 1;							 
		portStruct.Security = 0;
		portStruct.domain = 0;						//domain : 0 1 2 3
		portStruct.Distance = 1;
		portStruct.Direction = 0; 	
		m4u_config_port(&portStruct);


		DISP_Config_Overlay_to_Memory(overlay_dst_mva, 1);

		HDMI_LOG("overlay_dst_va=0x%08x, overlay_dst_mva=0x%08x, buffer size=0x%08x\n", overlay_dst_va, overlay_dst_mva, ovl_dst_buffer_size);


		return HDMI_STATUS_OK;
}

/* Release memory */
/* Will only be used in ioctl(MTK_HDMI_AUDIO_VIDEO_ENABLE) */
/*static*/ HDMI_STATUS hdmi_drv_deinit(void)
{
	int ret = 0;
	int temp_va_size;
	int hdmi_va_size;
	M4U_PORT_STRUCT portStruct;

	HDMI_FUNC();
	if(down_interruptible(&hdmi_update_mutex))
	{
		HDMI_LOG("[HDMI] can't get semaphore in\n");
		return;
	}

	DISP_Config_Overlay_to_Memory(overlay_dst_mva, 0);
	HDMI_LOG("disable overlay output to memory\n");
	
	portStruct.ePortID = M4U_PORT_WDMA1;			 //hardware port ID, defined in M4U_PORT_ID_ENUM
	portStruct.Virtuality = 0;							 
	portStruct.Security = 0;
	portStruct.domain = 0;						//domain : 0 1 2 3
	portStruct.Distance = 1;
	portStruct.Direction = 0; 	
	m4u_config_port(&portStruct);
	HDMI_LOG("disable WDMA1 m4u link\n");	
	
	ret = m4u_dealloc_mva(M4U_CLNTMOD_WDMA, 
			overlay_dst_va, 
			ovl_dst_buffer_size, 
			overlay_dst_mva);
	HDMI_LOG("overlay_dst_va mva dealloced\n");

	ret = m4u_dealloc_mva(M4U_CLNTMOD_WDMA, 
			ddp_dst_va, 
			ddp_dst_buffer_size, 
			ddp_dst_mva);
	HDMI_LOG("ddp_dst_va mva dealloced\n");
	up(&hdmi_update_mutex);
	
	return HDMI_STATUS_OK;
}

/* Set HDMI orientation, will be called in mtkfb_ioctl(SET_ORIENTATION) */
/*static*/ void hdmi_setorientation(int orientation)
{
		HDMI_FUNC();
		//RET_VOID_IF(!p->is_enabled);

		if(down_interruptible(&hdmi_update_mutex))
		{
				printk("[hdmi][HDMI] can't get semaphore in %s\n", __func__);
				return;
		}

		HDMI_LOG("old ori=%d, new ori=%d\n", p->orientation, orientation);
		p->orientation = orientation;
		p->is_reconfig_needed = true;
		HDMI_LOG("old ori=%d, new ori=%d\n", p->orientation, orientation);
		MMProfileLog(WFD_MMP_Events.Rotate, MMProfileFlagPulse);

		//done:
		up(&hdmi_update_mutex);
}

static int hdmi_release(struct inode *inode, struct file *file)
{
		return 0;
}

static int hdmi_open(struct inode *inode, struct file *file)
{
		return 0;
}

static BOOL hdmi_drv_init_context(void);


static char* _hdmi_ioctl_spy(unsigned int cmd)
{
		switch(cmd)
		{
				case MTK_EXT_DISPLAY_ENTER:
						return "MTK_EXT_DISPLAY_ENTER";
				case MTK_EXT_DISPLAY_LEAVE:
						return "MTK_EXT_DISPLAY_LEAVE";
				case MTK_EXT_DISPLAY_START:
						return "MTK_EXT_DISPLAY_START";
				case MTK_EXT_DISPLAY_STOP:
						return "MTK_EXT_DISPLAY_STOP";
				case MTK_EXT_DISPLAY_SET_MEMORY_INFO:
						return "MTK_EXT_DISPLAY_SET_MEMORY_INFO";
				case MTK_EXT_DISPLAY_GET_MEMORY_INFO:
						return "MTK_EXT_DISPLAY_GET_MEMORY_INFO";
				case MTK_EXT_DISPLAY_GET_BUFFER:
						return "MTK_EXT_DISPLAY_GET_BUFFER";
				case MTK_HDMI_AUDIO_VIDEO_ENABLE:
						return "MTK_HDMI_AUDIO_VIDEO_ENABLE";
				case MTK_HDMI_AUDIO_ENABLE:
						return "MTK_HDMI_AUDIO_ENABLE";
				case MTK_HDMI_VIDEO_ENABLE:
						return "MTK_HDMI_VIDEO_ENABLE";
				case MTK_HDMI_GET_CAPABILITY:
						return "MTK_HDMI_GET_CAPABILITY";
				case MTK_HDMI_GET_DEVICE_STATUS:
						return "MTK_HDMI_GET_DEVICE_STATUS";
				case MTK_HDMI_VIDEO_CONFIG:
						return "MTK_HDMI_VIDEO_CONFIG";
				case MTK_HDMI_AUDIO_CONFIG:
						return "MTK_HDMI_AUDIO_CONFIG";
				case MTK_HDMI_FORCE_FULLSCREEN_ON:
						return "MTK_HDMI_FORCE_FULLSCREEN_ON";
				case MTK_HDMI_FORCE_FULLSCREEN_OFF:
						return "MTK_HDMI_FORCE_FULLSCREEN_OFF";
				case MTK_HDMI_IPO_POWEROFF:
						return "MTK_HDMI_IPO_POWEROFF";
				case MTK_HDMI_IPO_POWERON:
						return "MTK_HDMI_IPO_POWERON";
				case MTK_HDMI_POWER_ENABLE:
						return "MTK_HDMI_POWER_ENABLE";
				case MTK_HDMI_PORTRAIT_ENABLE:
						return "MTK_HDMI_PORTRAIT_ENABLE";
				case MTK_HDMI_FORCE_OPEN:
						return "MTK_HDMI_FORCE_OPEN";
				case MTK_HDMI_FORCE_CLOSE:
						return "MTK_HDMI_FORCE_CLOSE";
				case MTK_HDMI_IS_FORCE_AWAKE:
						return "MTK_HDMI_IS_FORCE_AWAKE";
				case MTK_HDMI_ENTER_VIDEO_MODE:
						return "MTK_HDMI_ENTER_VIDEO_MODE";
				case MTK_HDMI_LEAVE_VIDEO_MODE:
						return "MTK_HDMI_LEAVE_VIDEO_MODE";
				case MTK_HDMI_REGISTER_VIDEO_BUFFER:
						return "MTK_HDMI_REGISTER_VIDEO_BUFFER";
				case MTK_HDMI_POST_VIDEO_BUFFER:
						return "MTK_HDMI_POST_VIDEO_BUFFER";
				case MTK_HDMI_FACTORY_MODE_ENABLE:
						return "MTK_HDMI_FACTORY_MODE_ENABLE";
#ifdef MTK_MT8193_HDMI_SUPPORT
				case MTK_HDMI_WRITE_DEV:
						return "MTK_HDMI_WRITE_DEV";
				case MTK_HDMI_READ_DEV:
						return "MTK_HDMI_READ_DEV";
				case MTK_HDMI_ENABLE_LOG:
						return "MTK_HDMI_ENABLE_LOG";
				case MTK_HDMI_CHECK_EDID:
						return "MTK_HDMI_CHECK_EDID";
				case MTK_HDMI_INFOFRAME_SETTING:
						return "MTK_HDMI_INFOFRAME_SETTING";
				case MTK_HDMI_ENABLE_HDCP:
						return "MTK_HDMI_ENABLE_HDCP";
				case MTK_HDMI_STATUS:
						return "MTK_HDMI_STATUS";
				case MTK_HDMI_HDCP_KEY:
						return "MTK_HDMI_HDCP_KEY";
				case MTK_HDMI_GET_EDID:
						return "MTK_HDMI_GET_EDID";
				case MTK_HDMI_SETLA:
						return "MTK_HDMI_SETLA";
				case MTK_HDMI_GET_CECCMD:
						return "MTK_HDMI_GET_CECCMD";
				case MTK_HDMI_SET_CECCMD:
						return "MTK_HDMI_SET_CECCMD";
				case MTK_HDMI_CEC_ENABLE:
						return "MTK_HDMI_CEC_ENABLE";
				case MTK_HDMI_GET_CECADDR:
						return "MTK_HDMI_GET_CECADDR";
				case MTK_HDMI_CECRX_MODE:
						return "MTK_HDMI_CECRX_MODE";
				case MTK_HDMI_SENDSLTDATA:
						return "MTK_HDMI_SENDSLTDATA";
				case MTK_HDMI_GET_SLTDATA:
						return "MTK_HDMI_GET_SLTDATA";			
#endif
				default:
						return "unknown ioctl command";
		}
}

extern int external_display_trigger_update(void);

void wfd_force_pattern_output(int enable)
{
	wfd_pattern_output = enable;
	if(wfd_pattern_output) 
	{
		wfd_pattern_output_index = 0;
	}
}

static long hdmi_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	int r = 0;

	HDMI_LOG("[HDMI] hdmi ioctl= %s(%d)\n", _hdmi_ioctl_spy(cmd),cmd&0xff);

	switch(cmd)
	{
		case MTK_EXT_DISPLAY_ENTER:
		{
			
			MMProfileLog(WFD_MMP_Events.WFDEnter, MMProfileFlagPulse);
			p->is_wfd = TRUE;
			if(p->is_enabled)
				return 0;

			p->is_enabled = true;
			WFD_CLEAR_NEW_BUFFER();

			break;
		}
		case MTK_EXT_DISPLAY_LEAVE:
		{
			MMProfileLog(WFD_MMP_Events.WFDLeave, MMProfileFlagPulse);
			p->is_wfd = FALSE;
			break;
		}
		case MTK_EXT_DISPLAY_START:
		{
			MMProfileLog(WFD_MMP_Events.WFDStart, MMProfileFlagPulse);
			SET_HDMI_ON();
			HDMI_CHECK_RET(hdmi_drv_init());
			//external_display_trigger_update();

			break;
		}
		case MTK_EXT_DISPLAY_STOP:
		{
			MMProfileLog(WFD_MMP_Events.WFDStop, MMProfileFlagPulse);			
			SET_HDMI_OFF();
			HDMI_CHECK_RET(hdmi_drv_deinit());
			HDMI_LOG("overlay_dst_va and ddp_dst_va will be freed\n");
			vfree(overlay_dst_va);
			vfree(ddp_dst_va);
			overlay_dst_va = 0;
			ddp_dst_va = 0;
			WFD_CLEAR_NEW_BUFFER();
			memset((void*)&timestamp, 0, sizeof(struct timeval)*16);
			break;
		}
		case MTK_EXT_DISPLAY_SET_MEMORY_INFO:
		{ 
			int i = 0;
			struct ext_memory_info info;
			if(copy_from_user(&info, (void __user *)argp, sizeof(struct ext_memory_info)))
			{
				HDMI_LOG("copy_from_user failed! line\n");
				r = -EFAULT;
				break;
			}

			HDMI_LOG("MTK_EXT_DISPLAY_SET_MEMORY_INFO, width=%d, height=%d, buffer number=%d, bpp=%d\n", info.width, info.height, info.buffer_num, info.bpp);
			p->hdmi_width = info.width;
			p->hdmi_height = info.height;
			hdmi_temp_buffer_number = info.buffer_num;
			HDMI_LOG("p->hdmi_width=%d, p->hdmi_height=%d\n", p->hdmi_width, p->hdmi_height);

			ovl_dst_buffer_size = DISP_GetScreenWidth() * DISP_GetScreenHeight() * 3 * hdmi_temp_buffer_number;
			ddp_dst_buffer_size = p->hdmi_width * p->hdmi_height *3 * hdmi_temp_buffer_number;

			overlay_dst_va = (unsigned int)vmalloc(ovl_dst_buffer_size);
			if (((void*) overlay_dst_va) == NULL)
			{
				HDMI_LOG("vmalloc %dbytes fail\n", ovl_dst_buffer_size);
				return -1;
			}
			
			ddp_dst_va = (unsigned int)vmalloc(ddp_dst_buffer_size);
			if (((void*) ddp_dst_va) == NULL)
			{
				HDMI_LOG("vmalloc %dbytes fail\n", ddp_dst_buffer_size);
				return -1;
			}

			HDMI_LOG("ddp_dst_va=0x%08x, overlay_dst_va=0x%08x\n", ddp_dst_va, overlay_dst_va);
			
			for(i=0;i<hdmi_temp_buffer_number;i++)
			{
				memset((void*)(ddp_dst_va+i*p->hdmi_width*p->hdmi_height*3), 0x00,p->hdmi_width*p->hdmi_height);
				memset((void*)(ddp_dst_va+i*p->hdmi_width*p->hdmi_height*3+p->hdmi_width*p->hdmi_height), 0x80, p->hdmi_width*p->hdmi_height);
			}

			break;
		}
		case MTK_EXT_DISPLAY_GET_BUFFER:
		{
			int ret = 0;
			struct ext_buffer buf = {0, 0, 0};

			MMProfileLogEx(WFD_MMP_Events.Getbuffer, MMProfileFlagStart, wfd_pattern_output_index, 0);

			if(	p->is_wfd_suspend)
			{	
				{
					memset((void*)(ddp_dst_va), 0x00,p->hdmi_width*p->hdmi_height);
					memset((void*)(ddp_dst_va+p->hdmi_width*p->hdmi_height), 0x80, p->hdmi_width*p->hdmi_height);
				}
				struct timeval t;
				do_gettimeofday(&t);
				buf.id = 0;
				buf.ts_sec = t.tv_sec;
				buf.ts_nsec = t.tv_usec;

				HDMI_LOG("get buffer, id=%d, ts: sec=%d, usec=%d\n", buf.id, timestamp[buf.id].tv_sec, timestamp[buf.id].tv_usec);

				ret = copy_to_user(argp, &buf,  sizeof(struct ext_buffer));
				HDMI_LOG("get buffer, copy to user finished\n");
				MMProfileLogEx(WFD_MMP_Events.Getbuffer, MMProfileFlagEnd, 1, 0);
				
#if 0
				// xuecheng, this is for IT only
				{
					MMP_MetaDataBitmap_t Bitmap;
					Bitmap.data1 = buf.id;
					Bitmap.width = p->hdmi_width;
					Bitmap.height = p->hdmi_height;
					Bitmap.format = MMProfileBitmapBGR888;
					Bitmap.start_pos = 0;
					Bitmap.pitch = p->hdmi_width*3;
					Bitmap.data_size = Bitmap.pitch * Bitmap.height;
					Bitmap.down_sample_x = 10;
					Bitmap.down_sample_y = 10;
					Bitmap.pData = (void*)ddp_dst_va+ buf.id* p->hdmi_width * p->hdmi_height * 3;
					Bitmap.bpp = 24; 
					MMProfileLogMetaBitmap(WFD_MMP_Events.Getbuffer, MMProfileFlagPulse, &Bitmap);
				}
#endif
				return ret;					
			}

			if(wfd_pattern_output)
			{
				struct timeval t;
				{
					int i = 0;
					memset((void*)(ddp_dst_va+wfd_pattern_output_index*p->hdmi_width*p->hdmi_height*3), 0x00,p->hdmi_width*p->hdmi_height);
					memset((void*)(ddp_dst_va+wfd_pattern_output_index*p->hdmi_width*p->hdmi_height*3+p->hdmi_width*p->hdmi_height), 0x80, p->hdmi_width*p->hdmi_height);

					memset((void*)(ddp_dst_va+wfd_pattern_output_index*p->hdmi_width*p->hdmi_height*3 + wfd_pattern_output_index*p->hdmi_width*p->hdmi_height/8), 0xFF, p->hdmi_width*p->hdmi_height/8);
				}

				do_gettimeofday(&t);
				buf.id = wfd_pattern_output_index;
				buf.ts_sec = t.tv_sec;
				buf.ts_nsec = t.tv_usec;

				wfd_pattern_output_index = ((wfd_pattern_output_index+1)%hdmi_temp_buffer_number);
				
				ret = copy_to_user(argp, &buf,	sizeof(struct ext_buffer));

				MMProfileLogEx(WFD_MMP_Events.Getbuffer, MMProfileFlagEnd, buf.ts_sec, buf.ts_nsec);

				return ret;
			}
			

			if(WFD_GET_NEW_BUFFER())
			{
				WFD_CLEAR_NEW_BUFFER();
			}
			else
			{
				interruptible_sleep_on(&external_display_getbuffer_wq);
				WFD_CLEAR_NEW_BUFFER();				
			}

			buf.id = ((hdmi_buffer_write_id+(hdmi_temp_buffer_number-1))%hdmi_temp_buffer_number);
			buf.ts_sec = timestamp[buf.id].tv_sec;
			buf.ts_nsec = timestamp[buf.id].tv_usec;
			HDMI_LOG("get buffer, id=%d, ts: sec=%d, usec=%d\n", buf.id, timestamp[buf.id].tv_sec, timestamp[buf.id].tv_usec);

			ret = copy_to_user(argp, &buf,  sizeof(struct ext_buffer));
			
			MMProfileLogEx(WFD_MMP_Events.Getbuffer, MMProfileFlagEnd, 2, hdmi_buffer_write_id);
			
#if 0
							// xuecheng, this is for IT only
							{
								MMP_MetaDataBitmap_t Bitmap;
								Bitmap.data1 = buf.id;
								Bitmap.width = p->hdmi_width;
								Bitmap.height = p->hdmi_height;
								Bitmap.format = MMProfileBitmapBGR888;
								Bitmap.start_pos = 0;
								Bitmap.pitch = p->hdmi_width*3;
								Bitmap.data_size = Bitmap.pitch * Bitmap.height;
								Bitmap.down_sample_x = 10;
								Bitmap.down_sample_y = 10;
								Bitmap.pData = (void*)ddp_dst_va+ buf.id* p->hdmi_width * p->hdmi_height * 3;
								Bitmap.bpp = 24; 
								MMProfileLogMetaBitmap(WFD_MMP_Events.Getbuffer, MMProfileFlagPulse, &Bitmap);
							}
#endif
			return ret;			
		}

		default:
		{
			printk("[hdmi][HDMI] arguments error\n");
			break;
		}
	}

	return r;
}


static int hdmi_remove(struct platform_device *pdev)
{
		return 0;
}

void wfd_suspend(void)
{
	if(down_interruptible(&hdmi_update_mutex))
	{
		HDMI_LOG("[HDMI] can't get semaphore in\n");
		return;
	}

	p->is_wfd_suspend = true;
	MMProfileLogEx(WFD_MMP_Events.DDPKBitblt, MMProfileFlagPulse, 100, 0);
	{
		int i = 0;
		for(i=0;i<hdmi_temp_buffer_number;i++)
		{
			memset((void*)(ddp_dst_va+i*p->hdmi_width*p->hdmi_height*3), 0x00,p->hdmi_width*p->hdmi_height);
			memset((void*)(ddp_dst_va+i*p->hdmi_width*p->hdmi_height*3+p->hdmi_width*p->hdmi_height), 0x80, p->hdmi_width*p->hdmi_height);
		}
	}
	WFD_PUT_NEW_BUFFER();
	wake_up_interruptible(&external_display_getbuffer_wq);

	up(&hdmi_update_mutex);
}

void wfd_resume(void)
{

	if(down_interruptible(&hdmi_update_mutex))
	{
		HDMI_LOG("[HDMI] can't get semaphore in\n");
		return;
	}

	p->is_wfd_suspend = false;
	MMProfileLogEx(WFD_MMP_Events.DDPKBitblt, MMProfileFlagPulse, 900, 0);

	up(&hdmi_update_mutex);
}


static void __exit hdmi_exit(void)
{
		device_destroy(hdmi_class, hdmi_devno);
		class_destroy(hdmi_class);
		cdev_del(hdmi_cdev);
		unregister_chrdev_region(hdmi_devno, 1);

}

static int hdmi_mmap(struct file *file, struct vm_area_struct * vma)
{
		int i;

		int buffer_size = (p->hdmi_width*p->hdmi_height*3*hdmi_temp_buffer_number);
		printk("[hdmi_mmap] vma->vm_pgoff=0x%08x\n", vma->vm_pgoff);
		printk("[hdmi_mmap] vma->vm_start=0x%08x\n", vma->vm_start);
		printk("[hdmi_mmap] vma->vm_end=0x%08x\n", vma->vm_end);
		printk("[hdmi_mmap] vma->vm_page_prot=0x%08x\n", vma->vm_page_prot);
		printk("[hdmi_mmap] vma->vm_flags=0x%08x\n", vma->vm_flags);

		vma->vm_flags |= VM_RESERVED;
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

		printk("[hdmi_mmap] vma->vm_flags=0x%08x\n", vma->vm_flags);
		for(i=0;i<buffer_size;i+=PAGE_SIZE)
		{
				if(remap_pfn_range(vma, 
										(vma->vm_start+i), 
										vmalloc_to_pfn((unsigned int)ddp_dst_va+i),
										PAGE_SIZE, 
										vma->vm_page_prot|PAGE_SHARED))
				{
						return -EAGAIN;
				}
		}

		return 0;
}

struct file_operations hdmi_fops = {
		.owner   = THIS_MODULE,
		.unlocked_ioctl   = hdmi_ioctl,
		.open    = hdmi_open,
		.release = hdmi_release,
		.mmap = hdmi_mmap
};

static int hdmi_probe(struct platform_device *pdev)
{
		int ret = 0;
		struct class_device *class_dev = NULL;

		printk("[hdmi]%s\n", __func__);

		/* Allocate device number for hdmi driver */
		ret = alloc_chrdev_region(&hdmi_devno, 0, 1, HDMI_DEVNAME);
		if(ret)
		{
				printk("[hdmi]alloc_chrdev_region fail\n");
				return -1;
		}

		/* For character driver register to system, device number binded to file operations */
		hdmi_cdev = cdev_alloc();
		hdmi_cdev->owner = THIS_MODULE;
		hdmi_cdev->ops = &hdmi_fops;
		ret = cdev_add(hdmi_cdev, hdmi_devno, 1);

		/* For device number binded to device name(hdmitx), one class is corresponeded to one node */
		hdmi_class = class_create(THIS_MODULE, HDMI_DEVNAME);
		/* mknod /dev/hdmitx */
		class_dev = (struct class_device *)device_create(hdmi_class, NULL, hdmi_devno,	NULL, HDMI_DEVNAME);

		printk("[hdmi][%s] current=0x%08x\n", __func__, (unsigned int)current);

		init_waitqueue_head(&hdmi_update_wq);
		init_waitqueue_head(&external_display_getbuffer_wq);
		init_waitqueue_head(&hdmi_overlay_config_wq);

		hdmi_update_task = kthread_create(hdmi_update_kthread, NULL, "hdmi_update_kthread");
		wake_up_process(hdmi_update_task);

		hdmi_overlay_config_task = kthread_create(hdmi_overlay_config_kthread, NULL, "hdmi_overlay_config_kthread");
		wake_up_process(hdmi_overlay_config_task);

		return 0;
}

static struct platform_driver hdmi_driver = {
		.probe  = hdmi_probe,
		.remove = hdmi_remove,
		.driver = { .name = HDMI_DEVNAME }
};
static struct platform_device hdmi_device = {
	.name = HDMI_DEVNAME,
	.id   = 0,
};

static int __init hdmi_init(void)
{
		int ret = 0;
		printk("[hdmi]%s\n", __func__);


		if (platform_device_register(&hdmi_device))
		{
			printk("[hdmi]failed to register hdmi device");
		}
		if (platform_driver_register(&hdmi_driver))
		{
				printk("[hdmi]failed to register hdmi driver\n");
				return -1;
		}

		SET_HDMI_OFF();
		init_wfd_mmp_events();

		return 0;
}


module_init(hdmi_init);
module_exit(hdmi_exit);
MODULE_AUTHOR("Xuecheng, Zhang <xuecheng.zhang@mediatek.com>");
MODULE_DESCRIPTION("WFD Driver");
MODULE_LICENSE("GPL");

#endif
