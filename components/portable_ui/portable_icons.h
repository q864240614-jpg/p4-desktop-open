#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    PORTABLE_ICON_BULB,
    PORTABLE_ICON_FAN,
    PORTABLE_ICON_PREVIOUS,
    PORTABLE_ICON_PLAY,
    PORTABLE_ICON_PAUSE,
    PORTABLE_ICON_NEXT,
    PORTABLE_ICON_SPEAKER,
    PORTABLE_ICON_STATUS_CLUSTER,
    PORTABLE_ICON_COUNT
} PortableIcon;
const lv_img_dsc_t *PortableIcons_Get(PortableIcon icon);
const lv_img_dsc_t *PortableIcons_GetAccent(PortableIcon icon);
#ifdef __cplusplus
}
#endif
