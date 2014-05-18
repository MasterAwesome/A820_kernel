#ifndef __MTK_DEBUG_H__
#define __MTK_DEBUG_H__

#include <linux/workqueue.h>

#include "img_types.h"

#ifdef MTK_DEBUG

#if defined (__cplusplus)
extern "C" {
#endif

IMG_IMPORT IMG_VOID IMG_CALLCONV MTKDebugInit(IMG_VOID);
IMG_IMPORT IMG_VOID IMG_CALLCONV MTKDebugDeinit(IMG_VOID);
IMG_IMPORT IMG_VOID IMG_CALLCONV MTKDebugSetInfo(const IMG_CHAR* acDebugMsg, IMG_INT32 i32Size);
IMG_IMPORT IMG_VOID IMG_CALLCONV MTKDebugGetInfo(IMG_CHAR* acDebugMsg, IMG_INT32 i32Size);
IMG_IMPORT IMG_VOID IMG_CALLCONV MTKDebugEnable3DMemInfo(IMG_BOOL bEnable);
IMG_IMPORT IMG_BOOL IMG_CALLCONV MTKDebugIsEnable3DMemInfo(IMG_VOID);

#ifdef MTK_DEBUG_WORKQUEUE_PRINT

#define MTK_DEBUG_WORKQUEUE_PRINT_WORK_MAX_SIZE		(3)
#define MTK_DEBUG_WORKQUEUE_PRINT_BUFFER_SIZE		(1 * 1024 * 1024)	// 1 MB
#define MTK_DEBUG_WORKQUEUE_PRINT_MAX_LINE			(4 * 1024)			// 

typedef struct MTK_WORKQUEUE_PRINT_TAG
{
	int cycle;
	struct workqueue_struct   	*psWorkQueue;
	
} MTK_WORKQUEUE_PRINT;

typedef struct MTK_WORKQUEUE_PRINT_WORK_TAG
{
	char buf[MTK_DEBUG_WORKQUEUE_PRINT_BUFFER_SIZE];
	char *line[MTK_DEBUG_WORKQUEUE_PRINT_MAX_LINE];
	char *p;
	int l;
	int workID;
	IMG_BOOL bInUse; 
	
	struct work_struct 			sWork;
	
} MTK_WORKQUEUE_PRINT_WORK;

IMG_IMPORT int IMG_CALLCONV MTKWPFindFree(IMG_VOID);
IMG_IMPORT IMG_VOID IMG_CALLCONV MTKWPPrint(int workID, const char *fmt, ...) IMG_FORMAT_PRINTF(2,3);
IMG_IMPORT IMG_VOID IMG_CALLCONV MTKWPRelease(int workID);

IMG_IMPORT IMG_VOID IMG_CALLCONV MTKWPRegister(IMG_VOID);
IMG_IMPORT int IMG_CALLCONV MTKWPGetGlobalID(IMG_VOID);
IMG_IMPORT IMG_BOOL IMG_CALLCONV MTKWPSomeoneRegister(IMG_VOID);
IMG_IMPORT IMG_VOID IMG_CALLCONV MTKWPUnregister(IMG_VOID);

#define _LOG_MDWP2(...)				MTKWPPrint(MTKWPGetGlobalID(), __VA_ARGS__)
#define _DPF_MDWP2(level, ...)		MTKWPPrint(MTKWPGetGlobalID(), __VA_ARGS__)

#define MDWP_REGISTER		MTKWPRegister()
#define PVR_LOG_MDWP(x) 	_LOG_MDWP2 x;
#define PVR_DPF_MDWP(x) 	_DPF_MDWP2 x
#define MDWP_UNREGISTER		MTKWPUnregister()

#endif

#if defined (__cplusplus)
}
#endif

#endif

#if !defined(MTK_DEBUG) || !defined(MTK_DEBUG_WORKQUEUE_PRINT)
#define MDWP_REGISTER
#define PVR_LOG_MDWP(x) PVR_LOG(x)
#define PVR_DPF_MDWP(x) PVR_DPF(x)
#define MDWP_UNREGISTER	
#endif

#endif	/* __MTK_DEBUG_H__ */

/******************************************************************************
 End of file (mtk_debug.h)
******************************************************************************/

