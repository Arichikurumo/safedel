#include "wipe.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "chacha20.h"
#include "util.h"

#define CHUNK (4u << 20) /* 4 MiB per I/O; multiple of every logical sector size */

int pattern_parse(const char *s, enum pattern *out)
{
    if (strcmp(s, "random") == 0)
        *out = PAT_RANDOM;
    else if (strcmp(s, "zero") == 0 || strcmp(s, "zeros") == 0)
        *out = PAT_ZERO;
    else if (strcmp(s, "ones") == 0 || strcmp(s, "one") == 0)
        *out = PAT_ONES;
    else
        return -1;
    return 0;
}

const char *pattern_name(enum pattern p)
{
    switch (p) {
    case PAT_RANDOM: return "random";
    case PAT_ZERO:   return "zeros (0x00)";
    case PAT_ONES:   return "ones (0xFF)";
    }
    return "?";
}

struct progress {
    const char *label;
    uint64_t    total;
    double      start;
    double      last;
    int         tty;
};

static void progress_init(struct progress *p, const char *label, uint64_t total)
{
    p->label = label;
    p->total = total;
    p->start = now_seconds();
    p->last = 0;
    p->tty = isatty(STDERR_FILENO);
}

static void progress_show(struct progress *p, uint64_t done, int final)
{
    double now = now_seconds();
    double interval = p->tty ? 0.5 : 30.0;

    if (!final && now - p->last < interval)
        return;
    p->last = now;

    double elapsed = now - p->start;
    double rate = elapsed > 0 ? (double)done / elapsed : 0;
    double eta = rate > 0 ? (double)(p->total - done) / rate : 0;
    double pct = p->total ? 100.0 * (double)done / (double)p->total : 100.0;
    char hd[32], ht[32];
    unsigned secs = (unsigned)(final ? elapsed : eta);

    human_size(done, hd, sizeof hd);
    human_size(p->total, ht, sizeof ht);
    fprintf(stderr, "%s  %-28s %5.1f%%  %10s / %-10s %8.1f MiB/s  %s %02u:%02u:%02u%s",
            p->tty ? "\r" : "", p->label, pct, hd, ht, rate / (1024 * 1024),
            final ? "took" : "ETA ", secs / 3600, secs / 60 % 60, secs % 60,
            (final || !p->tty) ? "\n" : "");
}

static void fill(uint8_t *buf, size_t len, enum pattern pat, const struct chacha20 *rng, uint64_t off)
{
    switch (pat) {
    case PAT_RANDOM: chacha20_keystream(rng, off, buf, len); break;
    case PAT_ZERO:   memset(buf, 0x00, len); break;
    case PAT_ONES:   memset(buf, 0xFF, len); break;
    }
}

static void drop_caches(int fd, const struct devinfo *d)
{
    if (d->is_blockdev)
        ioctl(fd, BLKFLSBUF, 0);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

static int write_pass(int fd, const struct devinfo *d, uint8_t *buf, enum pattern pat,
                      const struct chacha20 *rng, const char *label)
{
    struct progress pr;
    uint64_t off = 0;

    progress_init(&pr, label, d->size);
    while (off < d->size) {
        size_t len = d->size - off < CHUNK ? (size_t)(d->size - off) : CHUNK;

        if (g_stop) {
            progress_show(&pr, off, 1);
            return WIPE_INTERRUPTED;
        }
        fill(buf, len, pat, rng, off);
        if (pwrite_full(fd, buf, len, (off_t)off) != 0) {
            fprintf(stderr, "\nsafedel: write failed at offset %llu: %s\n",
                    (unsigned long long)off, strerror(errno));
            return WIPE_ERROR;
        }
        off += len;
        progress_show(&pr, off, 0);
    }

    /* Make sure everything actually reached the device, not just a cache. */
    if (fdatasync(fd) != 0) {
        fprintf(stderr, "\nsafedel: flushing to device failed: %s\n", strerror(errno));
        return WIPE_ERROR;
    }
    progress_show(&pr, off, 1);
    return WIPE_OK;
}

static int verify_pass(int fd, const struct devinfo *d, uint8_t *expect, uint8_t *got,
                       enum pattern pat, const struct chacha20 *rng)
{
    struct progress pr;
    uint64_t off = 0, bad_chunks = 0, first_bad = 0;

    drop_caches(fd, d);
    progress_init(&pr, "Verify", d->size);
    while (off < d->size) {
        size_t len = d->size - off < CHUNK ? (size_t)(d->size - off) : CHUNK;

        if (g_stop) {
            progress_show(&pr, off, 1);
            return WIPE_INTERRUPTED;
        }
        if (pread_full(fd, got, len, (off_t)off) != 0) {
            fprintf(stderr, "\nsafedel: read failed at offset %llu: %s\n",
                    (unsigned long long)off, strerror(errno));
            return WIPE_ERROR;
        }
        fill(expect, len, pat, rng, off);
        if (memcmp(expect, got, len) != 0) {
            if (bad_chunks++ == 0) {
                size_t i = 0;
                while (expect[i] == got[i])
                    i++;
                first_bad = off + i;
            }
        }
        off += len;
        progress_show(&pr, off, 0);
    }
    progress_show(&pr, off, 1);

    if (bad_chunks) {
        fprintf(stderr, "safedel: VERIFY FAILED: %llu chunk(s) differ, first mismatch at byte %llu\n",
                (unsigned long long)bad_chunks, (unsigned long long)first_bad);
        return WIPE_ERROR;
    }
    printf("  Verify OK: every byte matches the last pass.\n");
    return WIPE_OK;
}

int wipe_run(int fd, const struct devinfo *d, const enum pattern *passes, int npasses, int verify)
{
    uint8_t *buf = NULL, *vbuf = NULL;
    struct chacha20 rng;
    char label[64];
    int rc = WIPE_ERROR;

    if (posix_memalign((void **)&buf, 4096, CHUNK) != 0 ||
        (verify && posix_memalign((void **)&vbuf, 4096, CHUNK) != 0)) {
        fprintf(stderr, "safedel: out of memory\n");
        goto out;
    }

    for (int i = 0; i < npasses; i++) {
        if (passes[i] == PAT_RANDOM) {
            /* fresh key per pass: 256-bit key + 64-bit nonce from the kernel CSPRNG */
            uint8_t seed[40];
            if (random_bytes(seed, sizeof seed) != 0) {
                fprintf(stderr, "safedel: getrandom failed: %s\n", strerror(errno));
                goto out;
            }
            chacha20_init(&rng, seed, seed + 32);
            explicit_bzero(seed, sizeof seed);
        }
        snprintf(label, sizeof label, "Pass %d/%d (%s)", i + 1, npasses, pattern_name(passes[i]));
        rc = write_pass(fd, d, buf, passes[i], &rng, label);
        if (rc != WIPE_OK)
            goto out;
    }

    if (verify)
        rc = verify_pass(fd, d, buf, vbuf, passes[npasses - 1], &rng);

out:
    explicit_bzero(&rng, sizeof rng);
    free(buf);
    free(vbuf);
    return rc;
}
