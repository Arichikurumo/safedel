#ifndef SAFEDEL_DEVICE_H
#define SAFEDEL_DEVICE_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_PARTS 128

enum bus { BUS_FILE, BUS_ATA, BUS_NVME, BUS_USB, BUS_MMC, BUS_LOOP, BUS_OTHER };

struct part {
    char  name[64];
    dev_t dev;
};

struct devinfo {
    char        path[PATH_MAX];    /* canonical path, e.g. /dev/sdb */
    char        syspath[PATH_MAX]; /* /sys/devices/... (block devices only) */
    char        name[NAME_MAX + 1];/* kernel name (sdb, nvme0n1) or file name */
    int         is_blockdev;
    dev_t       rdev;
    uint64_t    size;              /* bytes */
    uint32_t    sector_size;       /* logical sector size in bytes */
    int         rotational;        /* 1 = spinning disk, 0 = flash, -1 = unknown */
    int         removable;
    int         read_only;
    enum bus    bus;
    char        model[128];
    char        serial[128];
    struct part parts[MAX_PARTS];
    int         nparts;
};

int         device_probe(const char *path, struct devinfo *d);
void        device_print(const struct devinfo *d);
int         device_check_unused(const struct devinfo *d);
const char *bus_name(enum bus b);

#endif
