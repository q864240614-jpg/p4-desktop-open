#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define BAMBU_TRAYS 16
typedef struct {
    bool present;
    int unit, slot, remaining;
    uint32_t color;
    char material[24];
} BambuTray;
typedef struct {
    bool online, has_data;
    char state[20], name[128];
    int progress, remaining, layer, layers, nozzle, nozzle_target, bed, bed_target, fan;
    int chamber, speed_level, door_open; /* -1 = not reported; door: 0=closed, 1=open. */
    int active_tray,stage; /* MQTT stg_cur; -1 = not reported. */
    bool dual_nozzle;
    int active_nozzle, nozzle_current[2], nozzle_targets[2]; /* H2D: 0=right, 1=left. */
    uint32_t error;
    BambuTray trays[BAMBU_TRAYS];
} BambuState;
void BambuState_Init(BambuState *s);
/* Incremental reports retain absent fields; malformed input never mutates state. */
bool BambuState_Parse(BambuState *s, const char *json, size_t length);
/* Bounded MQTT reassembly. Reset before accepting a new report topic. */
typedef struct { char *data; size_t size, used; } BambuFrame;
void BambuFrame_Reset(BambuFrame *frame);
bool BambuFrame_Append(BambuFrame *frame, size_t total, size_t offset, const char *data, size_t size);
