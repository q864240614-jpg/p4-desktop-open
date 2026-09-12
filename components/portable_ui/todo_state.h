#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { int id; char title[321]; bool done; } TodoItem;
typedef struct {
    bool online,done;
    int page,pages,total,count,completed_id,failed_id;
    TodoItem items[8],next;
    uint32_t next_at;
} TodoState;
