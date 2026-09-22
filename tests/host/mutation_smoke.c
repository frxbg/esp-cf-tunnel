/* Deterministic mutation driver for the same libFuzzer entry point on GCC.
 * This is a bounded smoke run, not coverage-guided fuzzing. */
#include "reference_vectors.h"
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *, size_t);
static uint32_t rng = 0x1234abcd;
static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

int main(void)
{
    static const uint8_t config[] = "{\"version\":1,\"config\":{\"ingress\":[{\"hostname\":\"device.example.com\",\"service\":\"http://localhost:80\"},{\"service\":\"http_status:404\"}]}}";
    const uint8_t *seeds[] = {gold_headers, gold_token, gold_single_segment, gold_multi_segment, config};
    const size_t sizes[] = {sizeof(gold_headers), sizeof(gold_token), sizeof(gold_single_segment), sizeof(gold_multi_segment), sizeof(config)-1};
    uint8_t data[16384];
    size_t n, i, j;
    for (i = 0; i < 20000; ++i) {
        size_t seed = i % 5;
        n = sizes[seed]; memcpy(data, seeds[seed], n);
        if (i % 7 == 0) n = random32() % sizeof(data);
        if (i % 7 == 0) for (j = 0; j < n; ++j) data[j] = (uint8_t)random32();
        else {
            for (j = 0; j < 1 + i % 8; ++j) data[random32() % n] ^= (uint8_t)random32();
            if (i % 3 == 0) n = random32() % (n + 1);
        }
        (void)LLVMFuzzerTestOneInput(data, n);
    }
    puts("PASS: 20000 deterministic parser mutations (not coverage-guided fuzzing)");
    return 0;
}
