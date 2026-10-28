#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc.h"
static char logged[320];
static void test_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(logged, sizeof logged, format, args); va_end(args);
}
#define LOG(...) test_log(__VA_ARGS__)
#include "production_audio_request_summary.inc"

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)

int main(void) {
    uint8_t bytes[96];
    for (unsigned i = 0; i < sizeof bytes; ++i) bytes[i] = (uint8_t)(i * 29u + 7u);
    char summary[AUDREN_REQUEST_SUMMARY_CAP];
    IpcReq req = {.cmd = 1, .data = bytes, .data_size = 4};
    audren_request_summary(&req, summary, sizeof summary);
    CHECK(!strcmp(summary, "cmd=1 data_size=4 captured=4 truncated=0 raw=0724415e"));
    audren_log_size_request(&req);
    CHECK(!strcmp(logged, "[audren] request cmd=1 data_size=4 captured=4 truncated=0 raw=0724415e\n"));

    req.cmd = 0; req.data_size = 81;
    audren_request_summary(&req, summary, sizeof summary);
    CHECK(strstr(summary, "cmd=0 data_size=81 captured=80 truncated=1 raw=") == summary);
    const char *raw = strstr(summary, "raw=") + 4;
    CHECK(strlen(raw) == 160);
    for (unsigned i = 0; i < 80; ++i) {
        char byte[3]; snprintf(byte, sizeof byte, "%02x", bytes[i]);
        CHECK(raw[i * 2] == byte[0] && raw[i * 2 + 1] == byte[1]);
    }

    req.data = NULL; req.data_size = 0;
    audren_request_summary(&req, summary, sizeof summary);
    CHECK(!strcmp(summary, "cmd=0 data_size=0 captured=0 truncated=0 raw="));
    req.data_size = UINT32_MAX;
    audren_request_summary(&req, summary, sizeof summary);
    CHECK(!strcmp(summary, "cmd=0 data_size=4294967295 captured=0 truncated=0 raw=<null-data>"));

    audren_request_summary(NULL, summary, sizeof summary);
    CHECK(!strcmp(summary, "cmd=<null> data_size=0 captured=0 truncated=0 raw=<null-request>"));
    char tiny[1] = {'x'};
    audren_request_summary(&req, tiny, sizeof tiny);
    CHECK(tiny[0] == '\0');
    audren_request_summary(&req, NULL, 0);
    printf("%u bounded audio request-summary assertions passed\n", checks);
    return 0;
}
