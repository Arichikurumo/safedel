/*
 * safedel - securely erase a disk, then write a fresh, empty partition table.
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "device.h"
#include "fwerase.h"
#include "safedel.h"
#include "util.h"
#include "wizard.h"

static void usage(FILE *f, const char *prog)
{
    fprintf(f,
        "Usage: %s [options] <device>\n"
        "       %s              (no device: interactive mode)\n"
        "\n"
        "Securely erase a whole disk, then write a fresh, empty partition table.\n"
        "\n"
        "Options:\n"
        "  -l, --list           list disks and whether they can be erased\n"
        "  -m, --method METHOD  auto (default), overwrite or firmware\n"
        "                         auto: use the drive's firmware erase (NVMe Sanitize /\n"
        "                         Format, ATA Security Erase) on SSDs when supported,\n"
        "                         otherwise overwrite\n"
        "  -n, --passes N       number of random overwrite passes (default 1)\n"
        "  -p, --pattern LIST   explicit pass list, e.g. zero,ones,random\n"
        "                         patterns: random, zero, ones\n"
        "  -V, --verify         read back and verify the last overwrite pass\n"
        "  -t, --table TYPE     partition table to write: gpt (default), mbr, none\n"
        "  -d, --dry-run        run all checks and show the plan, write nothing\n"
        "  -h, --help           show this help\n"
        "\n"
        "Examples:\n"
        "  %s --list\n"
        "  %s --dry-run /dev/sdb\n"
        "  %s --verify /dev/sdb\n"
        "  %s --method overwrite --pattern zero,ones,random --table mbr /dev/sdc\n",
        prog, prog, prog, prog, prog, prog);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int parse_patterns(const char *s, enum pattern *out, int max)
{
    char *copy = strdup(s), *save = NULL;
    int n = 0;

    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (n == max) {
            fprintf(stderr, "safedel: at most %d passes\n", max);
            n = -1;
            break;
        }
        if (pattern_parse(tok, &out[n]) != 0) {
            fprintf(stderr, "safedel: unknown pattern '%s' (use random, zero, ones)\n", tok);
            n = -1;
            break;
        }
        n++;
    }
    free(copy);
    return n;
}

static int confirm(const struct devinfo *d)
{
    char hs[32], line[256];

    human_size(d->size, hs, sizeof hs);
    printf("\n*** ALL DATA ON %s (%s, %s) WILL BE PERMANENTLY DESTROYED ***\n",
           d->path, d->model[0] ? d->model : d->is_blockdev ? "unknown model" : "image file", hs);
    printf("Type the device name '%s' to continue: ", d->name);
    fflush(stdout);

    if (!fgets(line, sizeof line, stdin))
        return 0;
    line[strcspn(line, "\n")] = '\0';
    return strcmp(line, d->name) == 0;
}

static void reread_partitions(const struct devinfo *d)
{
    int fd = open(d->path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return;
    if (ioctl(fd, BLKRRPART) != 0)
        fprintf(stderr, "note: kernel did not re-read the partition table (%s); "
                        "replug the device or run 'partprobe'\n", strerror(errno));
    close(fd);
}

int safedel_run(const char *path, const struct options *o)
{
    /* ---- 1. identify the target ------------------------------------- */
    struct devinfo dev;
    if (device_probe(path, &dev) != 0)
        return 1;
    device_print(&dev);
    printf("\n");

    /* ---- 2. safety checks ------------------------------------------- */
    if (dev.is_blockdev) {
        printf("Safety checks:\n");
        if (device_check_unused(&dev, NULL, 0) != 0) {
            fprintf(stderr, "\nRefusing to erase a device that is in use. Unmount / deactivate it first.\n");
            return 1;
        }
        printf("  ✓ not mounted, not used as swap, not held by LVM / LUKS / RAID\n\n");
        if (geteuid() != 0 && !o->dry_run) {
            fprintf(stderr, "safedel: erasing a block device requires root\n");
            return 1;
        }
    }

    /* ---- 3. choose the erase method --------------------------------- */
    struct fw_plan fw;
    int fw_ok = 0, use_fw = 0;

    memset(&fw, 0, sizeof fw);
    if (o->method != METHOD_OVERWRITE) {
        if (!dev.is_blockdev) {
            snprintf(fw.why_not, sizeof fw.why_not, "target is a regular file");
        } else if (geteuid() != 0) {
            snprintf(fw.why_not, sizeof fw.why_not, "querying the drive requires root");
        } else {
            int pfd = open(dev.path, O_RDONLY | O_CLOEXEC);
            if (pfd < 0)
                snprintf(fw.why_not, sizeof fw.why_not, "%s", strerror(errno));
            else {
                fw_ok = fwerase_probe(pfd, &dev, &fw) == 0;
                close(pfd);
            }
        }
    }
    if (o->method == METHOD_FIRMWARE) {
        if (!fw_ok) {
            fprintf(stderr, "safedel: firmware erase not available: %s\n", fw.why_not);
            return 1;
        }
        use_fw = 1;
    } else if (o->method == METHOD_AUTO) {
        use_fw = fw_ok && dev.rotational == 0;
    }

    if (!use_fw && dev.is_blockdev && dev.rotational == 0) {
        printf("WARNING: this is flash storage. Overwriting cannot reach spare or remapped\n"
               "         blocks inside the drive, so fragments of old data may survive.\n");
        if (o->method == METHOD_AUTO)
            printf("         Firmware erase unavailable: %s\n", fw.why_not);
        else
            printf("         Consider --method auto to use the drive's built-in erase.\n");
        printf("\n");
    }

    /* ---- 4. show the plan ------------------------------------------- */
    int step = 1;
    printf("Plan:\n");
    if (use_fw) {
        printf("  %d. %s", step++, fw.desc);
        if (fw.est_minutes)
            printf(" (drive estimate: %u min)", fw.est_minutes);
        printf("\n");
        if (fw.kind == FW_ATA_SECURITY_ERASE)
            printf("     (sets the temporary password '%s', which the erase clears again)\n", "safedel");
        if (o->verify)
            printf("     note: --verify applies to overwrite passes only and is ignored here\n");
    } else {
        for (int i = 0; i < o->npasses; i++)
            printf("  %d. Overwrite pass %d/%d: %s\n", step++, i + 1, o->npasses,
                   pattern_name(o->passes[i]));
        if (o->verify)
            printf("  %d. Read back and verify the last pass\n", step++);
    }
    if (o->table != TABLE_NONE)
        printf("  %d. Write an empty %s partition table\n", step++, table_name(o->table));

    if (o->dry_run) {
        printf("\nDry run: nothing was written.\n");
        return 0;
    }

    /* ---- 5. confirm --------------------------------------------------- */
    if (!confirm(&dev)) {
        printf("Aborted. Nothing was written.\n");
        return 1;
    }

    /* ---- 6. erase ----------------------------------------------------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* O_EXCL on a block device fails with EBUSY if anything (fs, dm, md) holds it. */
    int flags = O_RDWR | O_CLOEXEC | (dev.is_blockdev ? O_EXCL | O_DIRECT : 0);
    int fd = open(dev.path, flags);
    if (fd < 0 && errno == EINVAL && (flags & O_DIRECT))
        fd = open(dev.path, flags & ~O_DIRECT);
    if (fd < 0) {
        fprintf(stderr, "safedel: cannot open %s: %s%s\n", dev.path, strerror(errno),
                errno == EBUSY ? " (mounted or held by another driver)" : "");
        return 1;
    }
    if (dev.is_blockdev) {
        uint64_t sz;
        int ss;
        if (ioctl(fd, BLKGETSIZE64, &sz) == 0)
            dev.size = sz;
        if (ioctl(fd, BLKSSZGET, &ss) == 0 && ss > 0)
            dev.sector_size = (uint32_t)ss;
    }

    printf("\n");
    double t0 = now_seconds();
    int rc = use_fw ? fwerase_run(fd, &dev, &fw)
                    : wipe_run(fd, &dev, o->passes, o->npasses, o->verify);
    if (rc != 0) {
        close(fd);
        if (rc == WIPE_INTERRUPTED) {
            fprintf(stderr, "\nInterrupted: %s is only PARTIALLY erased.\n", dev.path);
            return 130;
        }
        fprintf(stderr, "\nErase FAILED: %s is not fully erased.\n", dev.path);
        return 1;
    }

    /* ---- 7. partition table ----------------------------------------- */
    if (o->table != TABLE_NONE) {
        if (ptable_write(fd, &dev, o->table) != 0 || fsync(fd) != 0) {
            close(fd);
            fprintf(stderr, "safedel: data was erased, but writing the partition table failed\n");
            return 1;
        }
        printf("  Wrote empty %s partition table.\n", table_name(o->table));
    }
    close(fd);
    if (dev.is_blockdev)
        reread_partitions(&dev);

    unsigned secs = (unsigned)(now_seconds() - t0);
    printf("\nDone. %s was securely erased in %02u:%02u:%02u.\n",
           dev.path, secs / 3600, secs / 60 % 60, secs % 60);
    if (o->table != TABLE_NONE)
        printf("Create partitions and a filesystem with e.g. fdisk/parted and mkfs.\n");
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "list",    no_argument,       NULL, 'l' },
        { "method",  required_argument, NULL, 'm' },
        { "passes",  required_argument, NULL, 'n' },
        { "pattern", required_argument, NULL, 'p' },
        { "verify",  no_argument,       NULL, 'V' },
        { "table",   required_argument, NULL, 't' },
        { "dry-run", no_argument,       NULL, 'd' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    struct options o = { .method = METHOD_AUTO, .table = TABLE_GPT,
                         .passes = { PAT_RANDOM }, .npasses = 1 };
    int list = 0, have_n = 0, have_p = 0;
    int c;

    setvbuf(stdout, NULL, _IOLBF, 0);

    while ((c = getopt_long(argc, argv, "lm:n:p:Vt:dh", longopts, NULL)) != -1) {
        switch (c) {
        case 'l':
            list = 1;
            break;
        case 'm':
            if (strcmp(optarg, "auto") == 0)
                o.method = METHOD_AUTO;
            else if (strcmp(optarg, "overwrite") == 0)
                o.method = METHOD_OVERWRITE;
            else if (strcmp(optarg, "firmware") == 0)
                o.method = METHOD_FIRMWARE;
            else {
                fprintf(stderr, "safedel: unknown method '%s'\n", optarg);
                return 2;
            }
            break;
        case 'n': {
            char *end;
            long v = strtol(optarg, &end, 10);
            if (*end || v < 1 || v > MAX_PASSES) {
                fprintf(stderr, "safedel: --passes must be between 1 and %d\n", MAX_PASSES);
                return 2;
            }
            o.npasses = (int)v;
            for (int i = 0; i < o.npasses; i++)
                o.passes[i] = PAT_RANDOM;
            have_n = 1;
            break;
        }
        case 'p':
            o.npasses = parse_patterns(optarg, o.passes, MAX_PASSES);
            if (o.npasses < 1)
                return 2;
            have_p = 1;
            break;
        case 'V':
            o.verify = 1;
            break;
        case 't':
            if (strcmp(optarg, "gpt") == 0)
                o.table = TABLE_GPT;
            else if (strcmp(optarg, "mbr") == 0)
                o.table = TABLE_MBR;
            else if (strcmp(optarg, "none") == 0)
                o.table = TABLE_NONE;
            else {
                fprintf(stderr, "safedel: unknown table type '%s'\n", optarg);
                return 2;
            }
            break;
        case 'd':
            o.dry_run = 1;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (have_n && have_p) {
        fprintf(stderr, "safedel: use either --passes or --pattern, not both\n");
        return 2;
    }
    if (list)
        return wizard_list();

    if (optind == argc) {
        /* no device given: interactive mode (keeps --dry-run if it was passed) */
        char path[PATH_MAX];
        if (!isatty(STDIN_FILENO)) {
            usage(stderr, argv[0]);
            return 2;
        }
        if (wizard_run(&o, path, sizeof path) != 0)
            return 1;
        return safedel_run(path, &o);
    }
    if (optind != argc - 1) {
        usage(stderr, argv[0]);
        return 2;
    }
    return safedel_run(argv[optind], &o);
}
