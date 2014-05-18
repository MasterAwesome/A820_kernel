#include <stdlib.h>
#include <stdio.h>
#include "camera_custom_if.h"

namespace NSCamCustom
{
/*******************************************************************************
* 
*******************************************************************************/


/*******************************************************************************
* custom exif
*******************************************************************************/
//#define EN_CUSTOM_EXIF_INFO
MINT32 custom_SetExif(void **ppCustomExifTag)
{
#ifdef EN_CUSTOM_EXIF_INFO
#define CUSTOM_EXIF_STRING_MAKE  "custom make"
#define CUSTOM_EXIF_STRING_MODEL "custom model"
#define CUSTOM_EXIF_STRING_SOFTWARE "custom software"
static customExifInfo_t exifTag = {CUSTOM_EXIF_STRING_MAKE,CUSTOM_EXIF_STRING_MODEL,CUSTOM_EXIF_STRING_SOFTWARE};
    if (0 != ppCustomExifTag) {
        *ppCustomExifTag = (void*)&exifTag;
    }
    return 0;
#else
    return -1;
#endif
}

/*******************************************************************************
* 
*******************************************************************************/
};  //NSCamCustom

