#include "npad_history.h"
#include <string.h>

static int id_index(uint32_t id) {
    return id < 8 ? (int)id : id == 0x10 ? 8 : id == 0x20 ? 9 : -1;
}
static int style_index(uint32_t style) {
    for (int i = 0; i < 5; ++i) if (style == (1u << i)) return i;
    return -1;
}
size_t npad_history_count(int32_t requested) {
    return requested <= 0 ? 0 : requested < (int32_t)NPAD_HISTORY_LIMIT ? (size_t)requested : NPAD_HISTORY_LIMIT;
}
bool npad_history_connected(uint32_t id, uint32_t style, uint32_t selected_id, uint32_t selected_styles) {
    return id_index(id) >= 0 && style_index(style) >= 0 && id == selected_id && (style & selected_styles);
}
void npad_history_reset(NpadHistoryBank *bank) { if (bank) memset(bank, 0, sizeof *bank); }
static void publish(NpadHistory *history, const NpadHistoryState *sample) {
    if (!sample || !sample->sampling) return;
    unsigned at = 0;
    while (at < history->count && history->state[at].sampling > sample->sampling) ++at;
    if (at == NPAD_HISTORY_LIMIT || (at < history->count && history->state[at].sampling == sample->sampling)) return;
    unsigned count = history->count < NPAD_HISTORY_LIMIT ? history->count + 1 : NPAD_HISTORY_LIMIT;
    memmove(history->state + at + 1, history->state + at, (count - at - 1) * sizeof *sample);
    history->state[at] = *sample;
    history->count = count;
}
size_t npad_history_read(NpadHistoryBank *bank, uint32_t id, uint32_t style,
    uint32_t selected_id, uint32_t selected_styles, const NpadHistoryState *sample,
    void *out, int32_t requested) {
    size_t capacity = npad_history_count(requested);
    if (!out || !capacity || !bank || (uintptr_t)out > UINTPTR_MAX - capacity * NPAD_STATE_SIZE) return 0;
    NpadHistoryState captured;
    if (sample) { memcpy(&captured, sample, sizeof captured); sample = &captured; }
    memset(out, 0, capacity * NPAD_STATE_SIZE);
    if (!bank->configured || bank->selected_id != selected_id || bank->selected_styles != selected_styles) {
        npad_history_reset(bank);
        bank->configured = true; bank->selected_id = selected_id; bank->selected_styles = selected_styles;
    }
    if (!npad_history_connected(id, style, selected_id, selected_styles)) return 0;
    NpadHistory *history = &bank->history[id_index(id)][style_index(style)];
    publish(history, sample);
    size_t count = history->count < capacity ? history->count : capacity;
    memcpy(out, history->state, count * NPAD_STATE_SIZE);
    return count;
}
