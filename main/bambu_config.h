#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#define BAMBU_CONFIG_SIZE 8192
#define BAMBU_TOKEN_SIZE 2048
typedef enum { BAMBU_OFF, BAMBU_LAN, BAMBU_CLOUD } BambuMode;
typedef struct {
    BambuMode mode;
    char ip[16], serial[40], code[64];
} BambuPrinterConfig;
typedef struct {
    BambuPrinterConfig printers[2];
    char region[3], token[BAMBU_TOKEN_SIZE], username[80];
} BambuConfig;
bool BambuConfig_Parse(const char *json, const BambuConfig *saved, BambuConfig *out, char *error, size_t size);
char *BambuConfig_JSON(const BambuConfig *config, bool secrets);
const char *BambuConfig_Broker(const BambuConfig *config);
