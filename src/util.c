#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_stop = 0;

int pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = ENOSPC;
            return -1;
        }
        p += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

int pread_full(int fd, void *buf, size_t len, off_t off)
{
    unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = pread(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO; /* unexpected end of device */
            return -1;
        }
        p += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

int random_bytes(void *buf, size_t len)
{
    unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Read a small text file (sysfs attribute etc.), trimming surrounding whitespace. */
int read_text_file(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
        n--;
    buf[n] = '\0';

    size_t lead = strspn(buf, " \t");
    if (lead > 0)
        memmove(buf, buf + lead, (size_t)n - lead + 1);
    return 0;
}

void human_size(uint64_t bytes, char *buf, size_t len)
{
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double v = (double)bytes;
    int u = 0;

    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
    else
        snprintf(buf, len, "%.2f %s", v, units[u]);
}

double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
