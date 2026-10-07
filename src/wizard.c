#include "wizard.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "util.h"

struct disk {
    struct devinfo info;
    int            available;
    char           status[256];
};

/* Real disks only: skip partitions, virtual devices (dm, md, zram, loop) and empty drives. */
static int is_listable(const char *name)
{
    char p[PATH_MAX], buf[64];

    if (name[0] == '.')
        return 0;
    snprintf(p, sizeof p, "/sys/class/block/%s/partition", name);
    if (access(p, F_OK) == 0)
        return 0;
    snprintf(p, sizeof p, "/sys/class/block/%s/device", name);
    if (access(p, F_OK) != 0)
        return 0;
    snprintf(p, sizeof p, "/sys/class/block/%s/size", name);
    if (read_text_file(p, buf, sizeof buf) != 0 || strtoull(buf, NULL, 10) == 0)
        return 0;
    return 1;
}

static int cmp_disk(const void *a, const void *b)
{
    return strcmp(((const struct disk *)a)->info.name, ((const struct disk *)b)->info.name);
}

static int scan_disks(struct disk **out)
{
    DIR *dh = opendir("/sys/class/block");
    struct dirent *e;
    struct disk *v = NULL;
    int n = 0, cap = 0;

    if (!dh) {
        perror("safedel: /sys/class/block");
        return -1;
    }
    while ((e = readdir(dh))) {
        char path[PATH_MAX];

        if (!is_listable(e->d_name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            struct disk *nv = realloc(v, (size_t)cap * sizeof *v);
            if (!nv) {
                free(v);
                closedir(dh);
                fprintf(stderr, "safedel: out of memory\n");
                return -1;
            }
            v = nv;
        }
        memset(&v[n], 0, sizeof v[n]);
        snprintf(path, sizeof path, "/dev/%s", e->d_name);
        if (device_probe(path, &v[n].info) != 0)
            continue;
        v[n].available = device_check_unused(&v[n].info, v[n].status, sizeof v[n].status) == 0;
        if (v[n].available)
            snprintf(v[n].status, sizeof v[n].status, "available");
        n++;
    }
    closedir(dh);
    if (n > 1)
        qsort(v, (size_t)n, sizeof *v, cmp_disk);
    *out = v;
    return n;
}

static void print_disks(const struct disk *v, int n, int numbered)
{
    int k = 0;

    printf("  %s%-14s %11s  %-4s %-8s  %-24s  %s\n",
           numbered ? "#   " : "", "DEVICE", "SIZE", "TYPE", "BUS", "MODEL", "STATUS");
    for (int i = 0; i < n; i++) {
        const struct devinfo *d = &v[i].info;
        char hs[32], num[8];

        human_size(d->size, hs, sizeof hs);
        if (v[i].available)
            snprintf(num, sizeof num, "%d", ++k);
        else
            snprintf(num, sizeof num, "-");
        printf("  ");
        if (numbered)
            printf("%-3s ", num);
        printf("%-14s %11s  %-4s %-8s  %-24.24s  %s%s\n",
               d->path, hs,
               d->rotational == 1 ? "HDD" : d->rotational == 0 ? "SSD" : "?",
               bus_name(d->bus), d->model[0] ? d->model : "-",
               v[i].available ? "" : "in use: ", v[i].status);
    }
}

int wizard_list(void)
{
    struct disk *v = NULL;
    int n = scan_disks(&v);

    if (n < 0)
        return 1;
    if (n == 0)
        printf("No disks found.\n");
    else
        print_disks(v, n, 0);
    free(v);
    return 0;
}

/* ---- prompts --------------------------------------------------------- */

/* Read one trimmed line. Returns -1 on EOF or when the user types q/quit. */
static int read_answer(char *buf, size_t len)
{
    fflush(stdout);
    if (!fgets(buf, (int)len, stdin))
        return -1;
    buf[strcspn(buf, "\n")] = '\0';
    size_t lead = strspn(buf, " \t");
    memmove(buf, buf + lead, strlen(buf + lead) + 1);
    for (size_t l = strlen(buf); l > 0 && (buf[l - 1] == ' ' || buf[l - 1] == '\t'); l--)
        buf[l - 1] = '\0';
    if (strcmp(buf, "q") == 0 || strcmp(buf, "quit") == 0)
        return -1;
    return 0;
}

/* Number in [min, max]; empty input returns def (def < min means "no default"). */
static int ask_number(const char *prompt, int def, int min, int max)
{
    char buf[64];

    for (;;) {
        if (def >= min)
            printf("%s [%d]: ", prompt, def);
        else
            printf("%s [%d-%d, q to quit]: ", prompt, min, max);
        if (read_answer(buf, sizeof buf) != 0)
            return -1;
        if (buf[0] == '\0' && def >= min)
            return def;

        char *end;
        long v = strtol(buf, &end, 10);
        if (buf[0] != '\0' && *end == '\0' && v >= min && v <= max)
            return (int)v;
        printf("  Please enter a number from %d to %d.\n", min, max);
    }
}

/* Returns the 0-based index of the chosen option, or -1 to quit. */
static int ask_choice(const char *question, const char *const *opts, int n, int def)
{
    printf("\n%s\n", question);
    for (int i = 0; i < n; i++)
        printf("  %d) %s\n", i + 1, opts[i]);
    int v = ask_number("Choice", def + 1, 1, n);
    return v < 0 ? -1 : v - 1;
}

static int ask_yesno(const char *question, int def)
{
    char buf[64];

    for (;;) {
        printf("\n%s [%s]: ", question, def ? "Y/n" : "y/N");
        if (read_answer(buf, sizeof buf) != 0)
            return -1;
        if (buf[0] == '\0')
            return def;
        if (strcmp(buf, "y") == 0 || strcmp(buf, "yes") == 0)
            return 1;
        if (strcmp(buf, "n") == 0 || strcmp(buf, "no") == 0)
            return 0;
        printf("  Please answer y or n.");
    }
}

/* ---- wizard ---------------------------------------------------------- */

int wizard_run(struct options *o, char *path, size_t len)
{
    static const char *const methods[] = {
        "auto       use the drive's built-in secure erase on SSDs, otherwise overwrite (recommended)",
        "overwrite  write over the whole disk",
        "firmware   only use the drive's built-in secure erase",
    };
    static const char *const tables[] = {
        "GPT   (recommended)",
        "MBR   (for very old computers)",
        "none  leave the disk completely blank",
    };
    struct disk *v = NULL;
    int n, navail = 0, pick, idx = -1, ans;

    printf("safedel - interactive mode (type q or press Ctrl-D to quit)\n\n");
    n = scan_disks(&v);
    if (n < 0)
        return -1;
    if (n == 0) {
        printf("No disks found.\n");
        free(v);
        return -1;
    }
    print_disks(v, n, 1);
    for (int i = 0; i < n; i++)
        navail += v[i].available;
    if (navail == 0) {
        printf("\nNo disk can be erased: all of them are in use (mounted, swap,\n"
               "or held by LVM / LUKS / RAID). Unmount or deactivate one first.\n");
        free(v);
        return -1;
    }
    if (geteuid() != 0)
        printf("\nNote: you are not root. Erasing a disk needs root, so start safedel with sudo.\n");

    printf("\n");
    pick = ask_number("Disk to erase", 0, 1, navail);
    if (pick < 0)
        goto quit;
    for (int i = 0, k = 0; i < n; i++)
        if (v[i].available && ++k == pick)
            idx = i;
    snprintf(path, len, "%s", v[idx].info.path);
    printf("  -> %s (%s)\n", v[idx].info.path, v[idx].info.model[0] ? v[idx].info.model : "unknown model");

    if ((ans = ask_choice("How should the disk be erased?", methods, 3, 0)) < 0)
        goto quit;
    o->method = (enum method)ans;

    if (o->method != METHOD_FIRMWARE) {
        printf("\nOne pass of random data is enough for modern drives.\n");
        if ((ans = ask_number("Number of overwrite passes", 1, 1, MAX_PASSES)) < 0)
            goto quit;
        o->npasses = ans;
        for (int i = 0; i < ans; i++)
            o->passes[i] = PAT_RANDOM;

        if ((ans = ask_yesno("Read everything back afterwards to verify? (takes about twice as long)", 0)) < 0)
            goto quit;
        o->verify = ans;
    }

    if ((ans = ask_choice("Which partition table should be written afterwards?", tables, 3, 0)) < 0)
        goto quit;
    o->table = ans == 0 ? TABLE_GPT : ans == 1 ? TABLE_MBR : TABLE_NONE;

    printf("\n----------------------------------------------------------------------\n\n");
    free(v);
    return 0;

quit:
    printf("\nAborted. Nothing was written.\n");
    free(v);
    return -1;
}
