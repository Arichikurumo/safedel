#ifndef SAFEDEL_UTIL_H
#define SAFEDEL_UTIL_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Set by the signal handler; long-running loops check it and stop cleanly. */
extern volatile sig_atomic_t g_stop;

int    pwrite_full(int fd, const void *buf, size_t len, off_t off);
int    pread_full(int fd, void *buf, size_t len, off_t off);
int    random_bytes(void *buf, size_t len);
int    read_text_file(const char *path, char *buf, size_t len);
void   human_size(uint64_t bytes, char *buf, size_t len);
double now_seconds(void);

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    put_le16(p, (uint16_t)v);
    put_le16(p + 2, (uint16_t)(v >> 16));
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)get_le16(p) | ((uint32_t)get_le16(p + 2) << 16);
}

#endif
