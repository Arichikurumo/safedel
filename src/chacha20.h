#ifndef SAFEDEL_CHACHA20_H
#define SAFEDEL_CHACHA20_H

#include <stddef.h>
#include <stdint.h>

/*
 * ChaCha20 (original variant: 64-bit block counter, 64-bit nonce) used as a
 * seekable CSPRNG. The keystream at any byte offset can be regenerated, which
 * lets the verify pass check random data without storing it.
 */
struct chacha20 {
    uint32_t state[16];
};

void chacha20_init(struct chacha20 *c, const uint8_t key[32], const uint8_t nonce[8]);
void chacha20_keystream(const struct chacha20 *c, uint64_t offset, uint8_t *out, size_t len);

#endif
