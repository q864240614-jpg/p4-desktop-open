#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *p4_h264_decoder;
typedef struct {
    const uint8_t *plane[3];
    int width, height, stride_y, stride_uv;
    int ready;
} p4_h264_frame;
int p4_h264_create(p4_h264_decoder *decoder);
// Input contract: complete SPS/PPS/IDR access units; predicted frames are excluded.
int p4_h264_decode(p4_h264_decoder decoder, const uint8_t *access_unit,
                   size_t size, p4_h264_frame *frame);
void p4_h264_destroy(p4_h264_decoder decoder);
#ifdef __cplusplus
}
#endif
