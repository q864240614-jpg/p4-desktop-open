#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "bambu_state.h"
#include "todo_state.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_data, online, limited;
    char plan[16];
    float remaining[2]; /* 0=weekly, 1=short window; NAN means unknown. */
    uint32_t reset_at[2], short_seconds; /* Unix seconds, and duration in seconds. */
} PortableUI_Codex;
typedef struct {
    bool nas_online;
    PortableUI_Codex codex;
} PortableUI_Status;

/* Call all UI functions on the LVGL task or under the same LVGL lock.
 * One instance per boot; parent must be >=800x480 and outlive the UI. */
lv_obj_t *PortableUI_Create(lv_obj_t *parent);
void PortableUI_SetTodo(const TodoState *state);
void PortableUI_SetTodoHandler(bool (*handler)(int page,int done,int id));
/* Call until true: starts when the event queue is idle, acknowledges after the
 * title is revealed. Caller retains the same reminder until acknowledged. */
bool PortableUI_ShowReminder(const char *title);
void PortableUI_SetPrinterSlot(int slot, const BambuState *state);
void PortableUI_SetPrinter(const BambuState *state);
void PortableUI_SetNetwork(const char *message);
/* Handler receives 0/1 to play, -1 to stop; all calls on the LVGL task. */
void PortableUI_SetVideoHandler(void (*handler)(int slot));
void PortableUI_SetVideoStatus(const char *message);
#define PORTABLE_UI_VIDEO_WIDTH 800
#define PORTABLE_UI_VIDEO_HEIGHT 480
/* Ownership transfers even when inactive; release runs on the LVGL task after replacement/exit. */
void PortableUI_SetVideoFrame(const uint16_t *rgb565, void (*release)(const uint16_t *));
void PortableUI_Process(void); /* Once per UI loop, alongside lv_timer_handler(). */
void PortableUI_SetStandby(bool show);
void PortableUI_Activity(void); /* Physical input: wake and reset idle countdown. */
bool PortableUI_ParseCodexJSON(const char *json, PortableUI_Codex *state);
void PortableUI_SetCodex(const PortableUI_Codex *state);
void PortableUI_SetStatus(const PortableUI_Status *status);
/* Optional parser, usable on a worker task. Zero-initialize status before first use.
 * On malformed JSON it marks data offline while retaining prior values. */
bool PortableUI_ParseStatusJSON(const char *json, PortableUI_Status *status);
#ifdef __cplusplus
}
#endif
