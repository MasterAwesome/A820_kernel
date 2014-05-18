obj-  := dummy.o

# Only included in non-EVB projects
ifeq ($(CONFIG_POWER_EXT), y)
else
    ifeq ($(CONFIG_MTK_SMART_BATTERY), y)
        # Always align the Makefile in mediatek/platform/mt6589/kernel/drivers/power/
        ifeq ($(MTK_NCP1851_SUPPORT),yes)
            # BCCT not supported
        else
            ifeq ($(MTK_BQ24196_SUPPORT),yes) 
                # BCCT not supported
            else
                # FAN5405 or PMIC6320 pulse charging
                obj-$(CONFIG_THERMAL) += mtk_cooler_bcct.o
            endif
        endif
    endif
endif

ifeq ($(MTK_TMP103_SUPPORT),yes)
obj-$(CONFIG_THERMAL) += mtk_ts_pcb.o
endif                                                  
