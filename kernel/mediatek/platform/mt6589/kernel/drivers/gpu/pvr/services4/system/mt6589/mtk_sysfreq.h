#ifndef MTK_SYSFREQ_H
#define MTK_SYSFREQ_H

#include "sgxdefs.h"
#include "services_headers.h"

#if defined(MTK_FREQ_INIT)

typedef struct PVRSRV_BRIDGE_INPUT_TAG
{
    unsigned int freq;
} PVRSRV_BRIDGE_INPUT;

PVRSRV_ERROR MTKSetFreqInfo(unsigned int freq);

#endif

PVRSRV_ERROR MtkInitSetFreq(void);
void MtkInitSetFreqTbl(void);


#endif // MTK_SYSFREQ_H