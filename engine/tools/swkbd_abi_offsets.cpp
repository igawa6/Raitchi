// Prints the real offsets of the software keyboard ABI fields (Eden/Yuzu header).
#include <cstddef>
#include <cstdio>
#include "applet_software_keyboard_types.h"

int main() {
    using namespace Service::AM::Frontend;
    printf("sizeof(SwkbdConfigCommon) = 0x%zx\n", sizeof(SwkbdConfigCommon));
    printf("offset type                 = 0x%zx\n", offsetof(SwkbdConfigCommon, type));
    printf("offset max_text_length      = 0x%zx\n", offsetof(SwkbdConfigCommon, max_text_length));
    printf("offset min_text_length      = 0x%zx\n", offsetof(SwkbdConfigCommon, min_text_length));
    printf("offset use_prediction       = 0x%zx\n", offsetof(SwkbdConfigCommon, use_prediction));
    printf("offset use_utf8             = 0x%zx\n", offsetof(SwkbdConfigCommon, use_utf8));
    printf("offset initial_string_offset= 0x%zx\n", offsetof(SwkbdConfigCommon, initial_string_offset));
    printf("offset initial_string_length= 0x%zx\n", offsetof(SwkbdConfigCommon, initial_string_length));
    return 0;
}
