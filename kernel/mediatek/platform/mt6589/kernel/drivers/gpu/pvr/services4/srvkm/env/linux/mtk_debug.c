#include <linux/sched.h>
#include "img_types.h"
#include <linux/string.h>	
#include "mtk_debug.h"
#include "servicesext.h"
#include "mutex.h"
#include "services.h"
#include "osfunc.h"

#include <linux/vmalloc.h>
#include "pvr_debug.h"
#include "srvkm.h"

#ifdef MTK_DEBUG

static IMG_UINT32 g_PID;
static PVRSRV_LINUX_MUTEX g_sDebugMutex;
static IMG_CHAR g_acMsgBuffer[MTK_DEBUG_MSG_LENGTH];
static IMG_BOOL g_bEnable3DMemInfo;

#ifdef MTK_DEBUG_WORKQUEUE_PRINT

static PVRSRV_LINUX_MUTEX g_sMWPMutex;
static MTK_WORKQUEUE_PRINT *g_psMWP;
static MTK_WORKQUEUE_PRINT_WORK *g_psMWPWork;
static int g_ui32MWPworkID;
static int g_ui32MWPwrokPID;

static IMG_VOID MTKWPHandler(struct work_struct *_psWork)
{
	MTK_WORKQUEUE_PRINT_WORK *psWork = container_of(_psWork, MTK_WORKQUEUE_PRINT_WORK, sWork);
	int i;

	
	printk(KERN_ERR "workID= %d, line= %d, size= %d\n", 
		psWork->workID, psWork->l, (int)(psWork->p - psWork->buf));
	
	for (i = 0;i < psWork->l; ++i)
	{
		printk(KERN_ERR "%s\n", psWork->line[i]);
	}
	
	LinuxLockMutex(&g_sMWPMutex);
	psWork->bInUse = IMG_FALSE;
	LinuxUnLockMutex(&g_sMWPMutex);
}

static IMG_BOOL MTKWPDeinit(IMG_VOID)
{
	if (g_psMWP)
	{
		destroy_workqueue(g_psMWP->psWorkQueue);
	}

	if (g_psMWPWork)
	{
		vfree(g_psMWP);
	}
	
	LinuxInitMutex(&g_sMWPMutex);

	g_psMWP = NULL; 
	g_psMWPWork = NULL;
	
	return IMG_TRUE;
}

static IMG_BOOL MTKWPInit(IMG_VOID)	
{	
	int i;

	g_ui32MWPworkID = -1;
	
	LinuxInitMutex(&g_sMWPMutex);

	g_psMWP = vmalloc(sizeof(MTK_WORKQUEUE_PRINT));
	if (!g_psMWP)
	{
		goto psMWPfail;
	}
	g_psMWP->cycle = -1;

	g_psMWPWork = vmalloc(sizeof(MTK_WORKQUEUE_PRINT_WORK) * MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE);
	if (!g_psMWPWork)
	{
		goto psWorkfail;
	}
	
	g_psMWP->psWorkQueue = alloc_ordered_workqueue("mwp", WQ_FREEZABLE | WQ_MEM_RECLAIM);
	for (i = 0; i < MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE; ++i)
	{
		g_psMWPWork[i].bInUse = IMG_FALSE;
		g_psMWPWork[i].workID = i;
		INIT_WORK(&g_psMWPWork[i].sWork, MTKWPHandler);
	}
	
	return IMG_TRUE;

psWorkfail:
	MTKWPDeinit();
psMWPfail:
	return IMG_FALSE;
}

int MTKWPFindFree(IMG_VOID)
{
	int i, c;

	if (!g_psMWP)
	{
		return -1;
	}

	c = g_psMWP->cycle;

	LinuxLockMutex(&g_sMWPMutex);
	for (i = 0; i < MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE; ++i)
	{
		c = (c + 1) % MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE;
		
		if (g_psMWPWork[c].bInUse == IMG_FALSE)
		{
			g_psMWP->cycle = c;
			
			// inital buffer
			g_psMWPWork[c].bInUse = IMG_TRUE;
			g_psMWPWork[c].p = g_psMWPWork[c].buf;
			g_psMWPWork[c].p[0] = 0;
			g_psMWPWork[c].l = 0;

			// for LOG the start time
			PVR_LOG(("MDWP_workID = %d", c));
			
			LinuxUnLockMutex(&g_sMWPMutex);
			return c;
		}
	}

	PVR_DPF((PVR_DBG_ERROR, 
		"MTKWPFindFreeWork: cannot get free work [size=%d]", 
		MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE));

	LinuxUnLockMutex(&g_sMWPMutex);
	return -1;
}

IMG_VOID MTKWPPrint(int workID, const char *fmt, ...)
{
	MTK_WORKQUEUE_PRINT_WORK *psWork;
	va_list args;
	int len;

	if (MTKWPSomeoneRegister())
	{
		return;
	}

	if (workID < 0 || workID >= MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE || !g_psMWPWork)
	{
		char buf[256];
		va_start(args, fmt);
		len = vsprintf(buf, fmt, args);
		va_end(args);
		
		PVR_LOG((buf));
		return;
	}
	
	psWork = &g_psMWPWork[workID];
	if ((psWork->p - psWork->buf) >= MTK_DEBUG_WORKQUEUE_PRINT_BUFFER_SIZE - 256)
	{
		// FIXME: Detect out of buffer 
		return;
	}

	if (psWork->l >= MTK_DEBUG_WORKQUEUE_PRINT_MAX_LINE)
	{
		// FIXME: archive max line
		return;
	}
	
	va_start(args, fmt);
	len = vsprintf(psWork->p, fmt, args);
	va_end(args);

	if (len > 0)
	{
		psWork->p[len] = 0;
		psWork->line[psWork->l++] = psWork->p;
		psWork->p += (len + 1);		
	}
	else 
	{ 
		// vsprintf fail 
	}
}

IMG_VOID MTKWPRelease(int workID)
{
	PVR_LOG(("MDWP_workID = %d [release]", workID));
	
	if (workID < 0 || workID >= MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE || !g_psMWPWork)
	{
		return;
	}
	queue_work(g_psMWP->psWorkQueue, &g_psMWPWork[workID].sWork);
}

IMG_VOID MTKWPRegister()	
{	
	int workID = MTKWPFindFree();
		
	if (workID < 0 || workID >= MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE 
		|| !g_psMWPWork
		|| 	g_ui32MWPworkID != -1)
	{
		return;
	}

	LinuxLockMutex(&g_sMWPMutex);
	g_ui32MWPworkID = workID;
	g_ui32MWPwrokPID = current->pid;
	LinuxUnLockMutex(&g_sMWPMutex);
}

IMG_IMPORT int IMG_CALLCONV MTKWPGetGlobalID(IMG_VOID)
{
	return g_ui32MWPworkID;
}

IMG_BOOL MTKWPSomeoneRegister(IMG_VOID)
{
	return g_ui32MWPworkID != -1 && g_ui32MWPwrokPID != current->pid;
}

IMG_VOID MTKWPUnregister(IMG_VOID)
{
	int workID = g_ui32MWPworkID;
	
	if (workID < 0 || workID >= MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE 
		|| !g_psMWPWork
		||  MTKWPSomeoneRegister())
	{
		return;
	}

	MTKWPRelease(g_ui32MWPworkID);

	LinuxLockMutex(&g_sMWPMutex);
	g_ui32MWPworkID = -1;
	g_ui32MWPwrokPID = -1;
	LinuxUnLockMutex(&g_sMWPMutex);
}


#endif


IMG_VOID MTKDebugInit(IMG_VOID)
{
	LinuxInitMutex(&g_sDebugMutex);
    g_bEnable3DMemInfo = IMG_FALSE;
    g_PID = 0;
	g_acMsgBuffer[0] = '\0';
	
#ifdef MTK_DEBUG_WORKQUEUE_PRINT
	if (!MTKWPInit())
	{
		PVR_DPF((PVR_DBG_ERROR, "mtk_workqueue_print init fail"));
	}
#endif
}

IMG_VOID MTKDebugDeinit(IMG_VOID)
{
#ifdef MTK_DEBUG_WORKQUEUE_PRINT
	MTKWPDeinit();
#endif
}

IMG_VOID MTKDebugSetInfo(
	const IMG_CHAR* pszInfo,
    IMG_INT32       i32Size)
{
    if (i32Size > MTK_DEBUG_MSG_LENGTH)
    {
        i32Size = MTK_DEBUG_MSG_LENGTH;
    }
	LinuxLockMutex(&g_sDebugMutex);
    if (pszInfo && i32Size > 0)
    {
    	g_PID = OSGetCurrentProcessIDKM();
        memcpy(g_acMsgBuffer, pszInfo, i32Size);
    	g_acMsgBuffer[i32Size - 1] = '\0';
    }
    else
    {
        g_PID = 0;
        g_acMsgBuffer[0] = '\0';
    }
	LinuxUnLockMutex(&g_sDebugMutex);
}

IMG_VOID MTKDebugGetInfo(
    IMG_CHAR* acDebugMsg,
    IMG_INT32 i32Size)
{
    if (i32Size > MTK_DEBUG_MSG_LENGTH)
    {
        i32Size = MTK_DEBUG_MSG_LENGTH;
    }
	LinuxLockMutex(&g_sDebugMutex);
	if ((g_PID == OSGetCurrentProcessIDKM()) || (g_acMsgBuffer[0] == '\0'))
	{
		memcpy(acDebugMsg, g_acMsgBuffer, i32Size);
        acDebugMsg[i32Size - 1] = '\0';
	}
	else
	{
		sprintf(acDebugMsg, "{None}");
	}
	LinuxUnLockMutex(&g_sDebugMutex);
}

IMG_IMPORT IMG_VOID IMG_CALLCONV MTKDebugEnable3DMemInfo(IMG_BOOL bEnable)
{
	LinuxLockMutex(&g_sDebugMutex);
    g_bEnable3DMemInfo = bEnable;
	LinuxUnLockMutex(&g_sDebugMutex);
}

IMG_IMPORT IMG_BOOL IMG_CALLCONV MTKDebugIsEnable3DMemInfo(IMG_VOID)
{
    return g_bEnable3DMemInfo;
}

#endif
