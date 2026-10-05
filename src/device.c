#include "device.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "util.h"

#define SYSBUF (PATH_MAX + 128)

const char *bus_name(enum bus b)
{
    switch (b) {
    case BUS_FILE: return "file";
    case BUS_ATA:  return "SATA/ATA";
    case BUS_NVME: return "NVMe";
    case BUS_USB:  return "USB";
    case BUS_MMC:  return "MMC/SD";
    case BUS_LOOP: return "loop";
    default:       return "other";
    }
}

static int sys_read(const char *dir, const char *attr, char *buf, size_t len)
{
    char p[SYSBUF];

    snprintf(p, sizeof p, "%s/%s", dir, attr);
    if (read_text_file(p, buf, len) != 0) {
        buf[0] = '\0';
        return -1;
    }
    return 0;
}

/* Look up a property (e.g. ID_SERIAL_SHORT) in udev's database for a block device. */
static void udev_prop(dev_t rdev, const char *key, char *buf, size_t len)
{
    char p[64], line[512];
    size_t klen = strlen(key);
    FILE *f;

    buf[0] = '\0';
    snprintf(p, sizeof p, "/run/udev/data/b%u:%u", major(rdev), minor(rdev));
    f = fopen(p, "re");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "E:", 2) == 0 && strncmp(line + 2, key, klen) == 0 && line[2 + klen] == '=') {
            line[strcspn(line, "\n")] = '\0';
            snprintf(buf, len, "%s", line + 3 + klen);
            break;
        }
    }
    fclose(f);
}

static int parse_devnum(const char *s, dev_t *out)
{
    unsigned maj, min;

    if (sscanf(s, "%u:%u", &maj, &min) != 2)
        return -1;
    *out = makedev(maj, min);
    return 0;
}

static int cmp_part(const void *a, const void *b)
{
    return strcmp(((const struct part *)a)->name, ((const struct part *)b)->name);
}

static void find_partitions(struct devinfo *d)
{
    DIR *dh = opendir(d->syspath);
    struct dirent *e;
    char p[SYSBUF], buf[64];

    if (!dh)
        return;
    while ((e = readdir(dh)) && d->nparts < MAX_PARTS) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/%s/partition", d->syspath, e->d_name);
        if (access(p, F_OK) != 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", d->syspath, e->d_name);
        if (sys_read(p, "dev", buf, sizeof buf) != 0)
            continue;
        struct part *pt = &d->parts[d->nparts];
        if (parse_devnum(buf, &pt->dev) != 0)
            continue;
        snprintf(pt->name, sizeof pt->name, "%s", e->d_name);
        d->nparts++;
    }
    closedir(dh);
    qsort(d->parts, (size_t)d->nparts, sizeof d->parts[0], cmp_part);
}

static enum bus detect_bus(const struct devinfo *d)
{
    if (strstr(d->syspath, "/usb"))
        return BUS_USB;
    if (strncmp(d->name, "nvme", 4) == 0)
        return BUS_NVME;
    if (strncmp(d->name, "mmcblk", 6) == 0)
        return BUS_MMC;
    if (strncmp(d->name, "loop", 4) == 0)
        return BUS_LOOP;
    if (strstr(d->syspath, "/ata"))
        return BUS_ATA;
    return BUS_OTHER;
}

int device_probe(const char *arg, struct devinfo *d)
{
    struct stat st;
    char link[64], buf[256];

    memset(d, 0, sizeof *d);
    d->rotational = -1;

    if (!realpath(arg, d->path) || stat(d->path, &st) != 0) {
        fprintf(stderr, "safedel: %s: %s\n", arg, strerror(errno));
        return -1;
    }

    if (S_ISREG(st.st_mode)) {
        const char *base = strrchr(d->path, '/');
        snprintf(d->name, sizeof d->name, "%s", base ? base + 1 : d->path);
        d->size = (uint64_t)st.st_size;
        d->sector_size = 512;
        d->bus = BUS_FILE;
        return 0;
    }
    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "safedel: %s is neither a block device nor a regular file\n", d->path);
        return -1;
    }

    d->is_blockdev = 1;
    d->rdev = st.st_rdev;
    snprintf(link, sizeof link, "/sys/dev/block/%u:%u", major(d->rdev), minor(d->rdev));
    if (!realpath(link, d->syspath)) {
        fprintf(stderr, "safedel: cannot find %s in sysfs: %s\n", d->path, strerror(errno));
        return -1;
    }
    snprintf(d->name, sizeof d->name, "%s", strrchr(d->syspath, '/') + 1);

    if (sys_read(d->syspath, "partition", buf, sizeof buf) == 0) {
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", d->syspath);
        *strrchr(parent, '/') = '\0';
        fprintf(stderr, "safedel: %s is a partition; give the whole disk (/dev/%s)\n",
                d->path, strrchr(parent, '/') + 1);
        return -1;
    }

    /* sysfs reports size in 512-byte units regardless of the logical sector size */
    if (sys_read(d->syspath, "size", buf, sizeof buf) == 0)
        d->size = strtoull(buf, NULL, 10) * 512ULL;
    if (sys_read(d->syspath, "queue/logical_block_size", buf, sizeof buf) == 0)
        d->sector_size = (uint32_t)strtoul(buf, NULL, 10);
    if (d->sector_size == 0)
        d->sector_size = 512;
    if (sys_read(d->syspath, "queue/rotational", buf, sizeof buf) == 0)
        d->rotational = atoi(buf);
    if (sys_read(d->syspath, "removable", buf, sizeof buf) == 0)
        d->removable = atoi(buf);
    if (sys_read(d->syspath, "ro", buf, sizeof buf) == 0)
        d->read_only = atoi(buf);

    d->bus = detect_bus(d);

    if (sys_read(d->syspath, "device/model", d->model, sizeof d->model) != 0)
        udev_prop(d->rdev, "ID_MODEL", d->model, sizeof d->model);
    if (sys_read(d->syspath, "device/serial", d->serial, sizeof d->serial) != 0)
        udev_prop(d->rdev, "ID_SERIAL_SHORT", d->serial, sizeof d->serial);

    find_partitions(d);

    if (d->size == 0) {
        fprintf(stderr, "safedel: %s has size 0 (no medium?)\n", d->path);
        return -1;
    }
    return 0;
}

void device_print(const struct devinfo *d)
{
    char hs[32];

    human_size(d->size, hs, sizeof hs);
    printf("Target:      %s\n", d->path);
    if (d->is_blockdev) {
        printf("Model:       %s\n", d->model[0] ? d->model : "(unknown)");
        printf("Serial:      %s\n", d->serial[0] ? d->serial : "(unknown)");
    }
    printf("Size:        %s (%llu bytes)\n", hs, (unsigned long long)d->size);
    if (!d->is_blockdev) {
        printf("Type:        regular file (disk image)\n");
        return;
    }
    printf("Sector size: %u bytes\n", d->sector_size);
    printf("Type:        %s\n", d->rotational == 1 ? "HDD (rotational)"
                              : d->rotational == 0 ? "SSD / flash (non-rotational)"
                              : "unknown");
    printf("Bus:         %s%s\n", bus_name(d->bus), d->removable ? " (removable)" : "");
    printf("Partitions:  ");
    if (d->nparts == 0)
        printf("none");
    for (int i = 0; i < d->nparts; i++)
        printf("%s%s", i ? ", " : "", d->parts[i].name);
    printf("\n");
}

/* Returns the name of the disk/partition matching dev, or NULL. */
static const char *owned_name(const struct devinfo *d, dev_t dev)
{
    if (dev == d->rdev)
        return d->name;
    for (int i = 0; i < d->nparts; i++)
        if (d->parts[i].dev == dev)
            return d->parts[i].name;
    return NULL;
}

static const char *owned_path(const struct devinfo *d, const char *path)
{
    struct stat st;

    if (path[0] != '/' || stat(path, &st) != 0 || !S_ISBLK(st.st_mode))
        return NULL;
    return owned_name(d, st.st_rdev);
}

static int is_system_mount(const char *mnt)
{
    return strcmp(mnt, "/") == 0 || strncmp(mnt, "/boot", 5) == 0 ||
           strcmp(mnt, "/efi") == 0 || strcmp(mnt, "/usr") == 0 ||
           strcmp(mnt, "/var") == 0 || strcmp(mnt, "/home") == 0;
}

static int check_holders(const char *dir, const char *name)
{
    char p[SYSBUF];
    DIR *dh;
    struct dirent *e;
    int n = 0;

    snprintf(p, sizeof p, "%s/holders", dir);
    dh = opendir(p);
    if (!dh)
        return 0;
    while ((e = readdir(dh))) {
        if (e->d_name[0] == '.')
            continue;
        fprintf(stderr, "  ✗ %s is in use by %s (device-mapper: LVM / LUKS / RAID)\n", name, e->d_name);
        n++;
    }
    closedir(dh);
    return n;
}

static int check_mounts(const struct devinfo *d)
{
    FILE *f = fopen("/proc/self/mountinfo", "re");
    char *line = NULL;
    size_t cap = 0;
    int n = 0;

    if (!f) {
        fprintf(stderr, "  ✗ cannot read /proc/self/mountinfo: %s\n", strerror(errno));
        return 1;
    }
    while (getline(&line, &cap, f) > 0) {
        unsigned maj, min;
        char mnt[PATH_MAX] = "", src[PATH_MAX] = "";
        const char *who;

        if (sscanf(line, "%*d %*d %u:%u %*s %4095s", &maj, &min, mnt) != 3)
            continue;
        char *sep = strstr(line, " - ");
        if (sep)
            sscanf(sep + 3, "%*s %4095s", src);

        /* btrfs and others report an anonymous dev number, so also check the source path */
        who = owned_name(d, makedev(maj, min));
        if (!who)
            who = owned_path(d, src);
        if (who) {
            fprintf(stderr, "  ✗ %s is mounted on %s%s\n", who, mnt,
                    is_system_mount(mnt) ? "  <-- THIS IS A SYSTEM DISK" : "");
            n++;
        }
    }
    free(line);
    fclose(f);
    return n;
}

static int check_swaps(const struct devinfo *d)
{
    FILE *f = fopen("/proc/swaps", "re");
    char line[PATH_MAX + 128], path[PATH_MAX];
    int n = 0;

    if (!f)
        return 0;
    if (!fgets(line, sizeof line, f)) { /* header */
        fclose(f);
        return 0;
    }
    while (fgets(line, sizeof line, f)) {
        const char *who;
        if (sscanf(line, "%4095s", path) != 1)
            continue;
        if ((who = owned_path(d, path))) {
            fprintf(stderr, "  ✗ %s is in use as swap\n", who);
            n++;
        }
    }
    fclose(f);
    return n;
}

int device_check_unused(const struct devinfo *d)
{
    char p[SYSBUF];
    int problems = 0;

    if (d->read_only) {
        fprintf(stderr, "  ✗ %s is read-only (write-protected)\n", d->name);
        problems++;
    }
    problems += check_holders(d->syspath, d->name);
    for (int i = 0; i < d->nparts; i++) {
        snprintf(p, sizeof p, "%s/%s", d->syspath, d->parts[i].name);
        problems += check_holders(p, d->parts[i].name);
    }
    problems += check_mounts(d);
    problems += check_swaps(d);
    return problems ? -1 : 0;
}
