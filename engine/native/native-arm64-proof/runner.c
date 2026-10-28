#include "proof.h"
#include <stdio.h>
int main(void) {
    char output[2048];
    int result = native_arm64_proof_run(output, sizeof output);
    puts(output);
    return result != 0;
}
