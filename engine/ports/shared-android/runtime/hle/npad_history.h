/* Generic bounded Npad public-state history. No SDL or guest CPU dependency. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NPAD_HISTORY_LIMIT 16u
#define NPAD_STATE_SIZE 40u
#define NPAD_HISTORY_IDS 10u
#define NPAD_HISTORY_STYLES 5u
typedef struct {
    uint64_t sampling, buttons;
    int32_t lx, ly, rx, ry;
    uint32_t attributes, reserved;
} NpadHistoryState;
_Static_assert(sizeof(NpadHistoryState) == NPAD_STATE_SIZE, "public Npad state ABI");
typedef struct {
    uint32_t count;
    NpadHistoryState state[NPAD_HISTORY_LIMIT];
} NpadHistory;
typedef struct {
    uint32_t selected_id, selected_styles;
    bool configured;
    NpadHistory history[NPAD_HISTORY_IDS][NPAD_HISTORY_STYLES];
} NpadHistoryBank;
size_t npad_history_count(int32_t requested);
bool npad_history_connected(uint32_t id, uint32_t style, uint32_t selected_id, uint32_t selected_styles);
void npad_history_reset(NpadHistoryBank *bank);
/* Caller serializes bank access. sample is one genuinely captured state or NULL;
 * sort publication by its sampling number so reentry/concurrent capture cannot
 * reverse the history. Return real available count; zero every unused requested
 * slot up to16, never fabricate fresh copies/edges for warming history. */
size_t npad_history_read(NpadHistoryBank *bank, uint32_t id, uint32_t style,
    uint32_t selected_id, uint32_t selected_styles, const NpadHistoryState *sample,
    void *out, int32_t requested);
