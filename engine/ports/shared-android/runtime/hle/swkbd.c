/* Derived from LAS Recomp Android at cca791a3e7218e1dac95e7d4a11b8f97dba3ea51.
 * Copyright (c) 2026 Peiikai. MIT: native/runtime/LICENSE-LASRecomp. */
// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL_stdinc.h>

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text byte offset / length (UTF-16 code units) in the work buffer at +0x4d0.
//
// The answer is written as UTF-16LE: that is what the game reads (a UTF-8 answer is read as
// UTF-16 and shows up as "汁???e" instead of "Alexandre"), and String::bufSize is a byte count.
// The applet's use_utf8 flag (+0x3bd of the applet config) is not part of this SDK argument, so
// it is not consulted here.

// Converts UTF-8 text to UTF-16LE. Returns the converted buffer (NUL terminated,
// caller frees it with SDL_free) and stores the number of code units in *units.
static char *keyboard_to_utf16(const char *text, size_t *units) {
    *units = 0;
    char *wide = SDL_iconv_string("UTF-16LE", "UTF-8", text, SDL_strlen(text) + 1);
    if (!wide) return NULL;
    while ((unsigned char)wide[*units * 2] || (unsigned char)wide[*units * 2 + 1]) (*units)++;
    return wide;
}

// Writes text (UTF-8) into out as UTF-16LE, at most max_len code units and at most cap bytes
// (NUL included). Returns the number of code units written.
static size_t keyboard_write_utf16(char *out, size_t cap, const char *text, int max_len) {
    if (!cap) return 0;
    memset(out, 0, cap);
    size_t units = 0;
    char *wide = keyboard_to_utf16(text, &units);
    if (!wide) return 0;
    if (max_len > 0 && units > (size_t)max_len) units = (size_t)max_len;
    size_t room = cap / 2 ? cap / 2 - 1 : 0;  // leave room for the UTF-16 NUL
    if (units > room) units = room;
    // Never split a surrogate pair at the length or capacity boundary.
    if (units && (unsigned char)wide[units * 2 - 1] >= 0xd8 && (unsigned char)wide[units * 2 - 1] <= 0xdb) units--;
    memcpy(out, wide, units * 2);
    SDL_free(wide);
    return units;
}

// The initial text lives in the work buffer as UTF-16LE; the overlay works in UTF-8.
static void keyboard_read_utf16(char *out, size_t cap, uint64_t work, uint32_t offset, uint32_t units) {
    if (!cap) return;
    memset(out, 0, cap);
    if (!work || !units) return;
    // One extra unit for the terminator: the guest buffer holds exactly `units` units.
    uint16_t *tmp = (uint16_t *)SDL_malloc(((size_t)units + 1) * sizeof(uint16_t));
    if (!tmp) return;
    for (uint32_t i = 0; i < units; i++) tmp[i] = *(const uint16_t *)(uintptr_t)(work + offset + 2 * i);
    tmp[units] = 0;
    char *utf8 = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)tmp, ((size_t)units + 1) * sizeof(uint16_t));
    SDL_free(tmp);
    if (!utf8) return;
    size_t n = SDL_strlen(utf8);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)utf8[n] & 0xc0) == 0x80) n--;
    }
    memcpy(out, utf8, n);
    SDL_free(utf8);
}

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";
    keyboard_read_utf16(initial, sizeof initial, wb, off, n);
    char text[96];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] name entry (max %d, initial \"%s\"): %s \"%s\"\n", max_len, initial, ok ? "entered" : "cancelled", text);
    if (ptr && size) keyboard_write_utf16((char *)(uintptr_t)ptr, size, text, max_len);
    c->x[0] = 0;
}
