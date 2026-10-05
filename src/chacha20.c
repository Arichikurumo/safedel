#include "chacha20.h"

#include <string.h>

#include "util.h"

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d)                          \
    do {                                        \
        a += b; d ^= a; d = ROTL(d, 16);        \
        c += d; b ^= c; b = ROTL(b, 12);        \
        a += b; d ^= a; d = ROTL(d, 8);         \
        c += d; b ^= c; b = ROTL(b, 7);         \
    } while (0)

static void chacha20_block(const uint32_t in[16], uint8_t out[64])
{
    uint32_t x[16];

    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++)
        put_le32(out + 4 * i, x[i] + in[i]);
}

void chacha20_init(struct chacha20 *c, const uint8_t key[32], const uint8_t nonce[8])
{
    c->state[0] = 0x61707865; /* "expand 32-byte k" */
    c->state[1] = 0x3320646e;
    c->state[2] = 0x79622d32;
    c->state[3] = 0x6b206574;
    for (int i = 0; i < 8; i++)
        c->state[4 + i] = get_le32(key + 4 * i);
    c->state[12] = 0;
    c->state[13] = 0;
    c->state[14] = get_le32(nonce);
    c->state[15] = get_le32(nonce + 4);
}

void chacha20_keystream(const struct chacha20 *c, uint64_t offset, uint8_t *out, size_t len)
{
    uint32_t st[16];
    uint8_t blk[64];
    uint64_t ctr = offset / 64;
    size_t skip = offset % 64;

    memcpy(st, c->state, sizeof st);
    while (len > 0) {
        st[12] = (uint32_t)ctr;
        st[13] = (uint32_t)(ctr >> 32);
        if (skip == 0 && len >= 64) {
            chacha20_block(st, out);
            out += 64;
            len -= 64;
        } else {
            size_t n = 64 - skip;
            if (n > len)
                n = len;
            chacha20_block(st, blk);
            memcpy(out, blk + skip, n);
            out += n;
            len -= n;
            skip = 0;
        }
        ctr++;
    }
}
