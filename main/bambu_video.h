#pragma once
void bambu_video_start(void); /* Under LVGL lock, after PortableUI_Create. */
void bambu_video_poll(void);  /* On the LVGL task. */
