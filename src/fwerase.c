#include "fwerase.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "util.h"

/* ---------------------------------------------------------------- NVMe --- */

#define NVME_ADMIN_GET_LOG_PAGE 0x02
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_FORMAT_NVM   0x80
#define NVME_ADMIN_SANITIZE     0x84
#define NVME_LOG_SANITIZE       0x81
#define NVME_NSID_ALL           0xFFFFFFFFu

#define SANACT_BLOCK_ERASE  2
#define SANACT_OVERWRITE    3
#define SANACT_CRYPTO_ERASE 4

static int nvme_admin(int fd, uint8_t opcode, uint32_t nsid, uint32_t cdw10,
                      void *buf, uint32_t len, uint32_t timeout_ms)
{
    struct nvme_admin_cmd c;
    int r;

    memset(&c, 0, sizeof c);
    c.opcode = opcode;
    c.nsid = nsid;
    c.cdw10 = cdw10;
    c.addr = (uint64_t)(uintptr_t)buf;
    c.data_len = len;
    c.timeout_ms = timeout_ms;
    r = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c);
    if (r > 0) { /* positive return = NVMe status code */
        fprintf(stderr, "safedel: NVMe command 0x%02x failed, status 0x%x\n", opcode, r);
        errno = EIO;
        return -1;
    }
    return r;
}

/* Count namespaces on the same controller (nvme0n1, nvme0n2, ...). */
static int nvme_namespace_count(const char *name)
{
    const char *n = strchr(name + 4, 'n');
    size_t plen;
    DIR *dh;
    struct dirent *e;
    int count = 0;

    if (!n)
        return -1;
    plen = (size_t)(n - name) + 1; /* "nvme0n" */
    dh = opendir("/sys/class/block");
    if (!dh)
        return -1;
    while ((e = readdir(dh))) {
        if (strncmp(e->d_name, name, plen) != 0 || e->d_name[plen] == '\0')
            continue;
        if (strspn(e->d_name + plen, "0123456789") == strlen(e->d_name + plen))
            count++;
    }
    closedir(dh);
    return count;
}

static int nvme_probe(int fd, const struct devinfo *d, struct fw_plan *p)
{
    uint8_t *id = NULL, *ns = NULL;
    int rc = -1;
    int nns = nvme_namespace_count(d->name);

    if (nns != 1) {
        snprintf(p->why_not, sizeof p->why_not,
                 "controller has %d namespaces; a firmware erase would wipe all of them", nns);
        return -1;
    }
    if (posix_memalign((void **)&id, 4096, 4096) != 0 || posix_memalign((void **)&ns, 4096, 4096) != 0) {
        snprintf(p->why_not, sizeof p->why_not, "out of memory");
        goto out;
    }
    if (nvme_admin(fd, NVME_ADMIN_IDENTIFY, 0, 1 /* CNS: controller */, id, 4096, 0) != 0) {
        snprintf(p->why_not, sizeof p->why_not, "NVMe Identify failed: %s", strerror(errno));
        goto out;
    }

    uint16_t oacs = get_le16(id + 256);
    uint32_t sanicap = get_le32(id + 328);

    /* Prefer Sanitize: it also covers caches, spare blocks and the controller's buffers. */
    if (sanicap & 0x7) {
        uint32_t act = (sanicap & 0x2) ? SANACT_BLOCK_ERASE
                     : (sanicap & 0x1) ? SANACT_CRYPTO_ERASE
                     : SANACT_OVERWRITE;
        p->kind = FW_NVME_SANITIZE;
        p->cdw10 = act;
        snprintf(p->desc, sizeof p->desc, "NVMe Sanitize (%s)",
                 act == SANACT_BLOCK_ERASE ? "Block Erase"
                 : act == SANACT_CRYPTO_ERASE ? "Crypto Erase" : "Overwrite");
        rc = 0;
        goto out;
    }

    /* Fall back to Format NVM with Secure Erase Settings = user data erase. */
    if (oacs & 0x2) {
        int nsid = ioctl(fd, NVME_IOCTL_ID);
        if (nsid <= 0) {
            snprintf(p->why_not, sizeof p->why_not, "cannot determine namespace id");
            goto out;
        }
        if (nvme_admin(fd, NVME_ADMIN_IDENTIFY, (uint32_t)nsid, 0 /* CNS: namespace */, ns, 4096, 0) != 0) {
            snprintf(p->why_not, sizeof p->why_not, "NVMe Identify Namespace failed");
            goto out;
        }
        uint8_t flbas = ns[26], dps = ns[29];
        /* keep the current LBA format and protection settings; only request the erase */
        p->cdw10 = (flbas & 0x0Fu)                  /* LBAF   */
                 | ((flbas & 0x10u) ? 1u << 4 : 0)  /* MSET   */
                 | ((uint32_t)(dps & 0x7) << 5)     /* PI     */
                 | ((dps & 0x8) ? 1u << 8 : 0)      /* PIL    */
                 | (1u << 9)                        /* SES = 1: user data erase */
                 | ((uint32_t)((flbas >> 5) & 0x3) << 12); /* LBAF upper bits */
        p->nsid = (uint32_t)nsid;
        p->kind = FW_NVME_FORMAT;
        snprintf(p->desc, sizeof p->desc, "NVMe Format NVM with Secure Erase (user data erase)");
        rc = 0;
        goto out;
    }

    snprintf(p->why_not, sizeof p->why_not, "controller supports neither Sanitize nor Format NVM");
out:
    free(id);
    free(ns);
    return rc;
}

static int nvme_sanitize_status(int fd, uint16_t *progress, int *status)
{
    uint8_t log[512] __attribute__((aligned(8)));
    uint32_t numdl = sizeof log / 4 - 1;

    if (nvme_admin(fd, NVME_ADMIN_GET_LOG_PAGE, NVME_NSID_ALL,
                   NVME_LOG_SANITIZE | (numdl << 16), log, sizeof log, 0) != 0)
        return -1;
    *progress = get_le16(log);
    *status = get_le16(log + 2) & 0x7;
    return 0;
}

static int nvme_run(int fd, const struct fw_plan *p)
{
    if (p->kind == FW_NVME_FORMAT) {
        printf("  Running %s (this can take a few minutes; do not power off)...\n", p->desc);
        if (nvme_admin(fd, NVME_ADMIN_FORMAT_NVM, p->nsid, p->cdw10, NULL, 0, 4 * 3600 * 1000) != 0) {
            fprintf(stderr, "safedel: Format NVM failed: %s\n", strerror(errno));
            return -1;
        }
        printf("  Format NVM completed.\n");
        return 0;
    }

    uint16_t prog;
    int st;

    printf("  Starting %s...\n", p->desc);
    if (nvme_admin(fd, NVME_ADMIN_SANITIZE, 0, p->cdw10, NULL, 0, 0) != 0) {
        fprintf(stderr, "safedel: Sanitize command failed: %s\n", strerror(errno));
        return -1;
    }

    /* Sanitize runs inside the drive; poll the sanitize status log until done. */
    for (;;) {
        sleep(1);
        if (nvme_sanitize_status(fd, &prog, &st) != 0) {
            fprintf(stderr, "safedel: cannot read sanitize status: %s\n", strerror(errno));
            return -1;
        }
        if (st == 2) { /* in progress */
            fprintf(stderr, "\r  Sanitize in progress: %5.1f%%", 100.0 * prog / 65536.0);
        } else if (st == 1 || st == 4) {
            fprintf(stderr, "\r  Sanitize in progress: 100.0%%\n");
            printf("  Sanitize completed successfully.\n");
            return 0;
        } else if (st == 3) {
            fprintf(stderr, "\nsafedel: the drive reports that Sanitize FAILED\n");
            return -1;
        }
        if (g_stop) {
            fprintf(stderr, "\nsafedel: stopped watching. Sanitize keeps running inside the drive\n"
                            "         (it even resumes after a power cycle). Check with: nvme sanitize-log\n");
            return -1;
        }
    }
}

/* ----------------------------------------------------------------- ATA --- */

#define ATA_PASSWORD "safedel"

#define ATA_IDENTIFY            0xEC
#define ATA_SEC_SET_PASSWORD    0xF1
#define ATA_SEC_ERASE_PREPARE   0xF3
#define ATA_SEC_ERASE_UNIT      0xF4

#define ATA_PROTO_NONDATA 3
#define ATA_PROTO_PIO_IN  4
#define ATA_PROTO_PIO_OUT 5

/* Send an ATA command through SCSI ATA PASS-THROUGH(16). */
static int ata_cmd(int fd, int proto, uint8_t command, void *buf, unsigned len, unsigned timeout_ms)
{
    uint8_t cdb[16] = { 0 }, sense[32] = { 0 };
    sg_io_hdr_t io;

    cdb[0] = 0x85;
    cdb[1] = (uint8_t)(proto << 1);
    if (len) {
        /* T_DIR (in only), BYT_BLOK=1, T_LENGTH=2 (length is in the sector count field) */
        cdb[2] = (proto == ATA_PROTO_PIO_IN ? 0x08 : 0x00) | 0x04 | 0x02;
        cdb[6] = (uint8_t)(len / 512);
    }
    cdb[14] = command;

    memset(&io, 0, sizeof io);
    io.interface_id = 'S';
    io.cmd_len = sizeof cdb;
    io.cmdp = cdb;
    io.mx_sb_len = sizeof sense;
    io.sbp = sense;
    io.dxfer_direction = !len ? SG_DXFER_NONE
                       : proto == ATA_PROTO_PIO_IN ? SG_DXFER_FROM_DEV : SG_DXFER_TO_DEV;
    io.dxferp = buf;
    io.dxfer_len = len;
    io.timeout = timeout_ms;

    if (ioctl(fd, SG_IO, &io) < 0)
        return -1;
    if (io.host_status != 0 || (io.driver_status & ~0x08 /* DRIVER_SENSE */) != 0) {
        errno = EIO;
        return -1;
    }
    if (io.status == 0)
        return 0;
    /* Some translators report CHECK CONDITION with an ATA status descriptor even on success. */
    if (io.status == 2 && (sense[0] & 0x7F) == 0x72 && sense[8] == 0x09 && !(sense[21] & 0x21))
        return 0;
    errno = EIO;
    return -1;
}

static int ata_identify(int fd, uint16_t id[256])
{
    uint8_t raw[512];

    if (ata_cmd(fd, ATA_PROTO_PIO_IN, ATA_IDENTIFY, raw, sizeof raw, 15000) != 0)
        return -1;
    for (int i = 0; i < 256; i++)
        id[i] = get_le16(raw + 2 * i);
    return 0;
}

static unsigned ata_erase_minutes(uint16_t w)
{
    if (w & 0x8000)
        return (w & 0x7FFF) * 2u; /* extended format (ACS-3) */
    return (w & 0xFF) * 2u;       /* 255 means "more than 508 minutes" */
}

static int ata_probe(int fd, struct fw_plan *p)
{
    uint16_t id[256];

    if (ata_identify(fd, id) != 0) {
        snprintf(p->why_not, sizeof p->why_not, "ATA pass-through IDENTIFY failed");
        return -1;
    }

    uint16_t sec = id[128];
    if (!(id[82] & 0x0002) || !(sec & 0x0001)) {
        snprintf(p->why_not, sizeof p->why_not, "drive does not support the ATA Security feature set");
        return -1;
    }
    if (sec & 0x0002) {
        snprintf(p->why_not, sizeof p->why_not, "a security password is already set on the drive");
        return -1;
    }
    if (sec & 0x0004) {
        snprintf(p->why_not, sizeof p->why_not, "drive is security-locked");
        return -1;
    }
    if (sec & 0x0008) {
        snprintf(p->why_not, sizeof p->why_not,
                 "drive is security-FROZEN (usually by the BIOS at boot). Suspend and resume the\n"
                 "         machine (systemctl suspend) or hot-replug the drive, then try again");
        return -1;
    }
    if (sec & 0x0010) {
        snprintf(p->why_not, sizeof p->why_not, "security attempt counter expired; power-cycle the drive");
        return -1;
    }

    p->kind = FW_ATA_SECURITY_ERASE;
    p->enhanced = (sec & 0x0020) != 0;
    p->est_minutes = ata_erase_minutes(p->enhanced ? id[90] : id[89]);
    snprintf(p->desc, sizeof p->desc, "ATA Security Erase%s", p->enhanced ? " (enhanced)" : "");
    return 0;
}

static void ata_recovery_hint(const struct devinfo *d)
{
    fprintf(stderr,
            "\n!!! The drive may still have the temporary security password '%s' set.\n"
            "!!! To remove it run:  hdparm --user-master u --security-disable %s %s\n",
            ATA_PASSWORD, ATA_PASSWORD, d->path);
}

static int ata_run(int fd, const struct devinfo *d, const struct fw_plan *p)
{
    uint8_t blk[512];
    uint16_t id[256];
    unsigned timeout_ms;

    /* 1. set a temporary user password (required to enable security erase) */
    memset(blk, 0, sizeof blk);
    memcpy(blk + 2, ATA_PASSWORD, strlen(ATA_PASSWORD));
    if (ata_cmd(fd, ATA_PROTO_PIO_OUT, ATA_SEC_SET_PASSWORD, blk, sizeof blk, 15000) != 0) {
        fprintf(stderr, "safedel: SECURITY SET PASSWORD failed: %s\n", strerror(errno));
        return -1;
    }

    /* 2. prepare + erase; the drive returns only when the erase is finished */
    if (ata_cmd(fd, ATA_PROTO_NONDATA, ATA_SEC_ERASE_PREPARE, NULL, 0, 15000) != 0) {
        fprintf(stderr, "safedel: SECURITY ERASE PREPARE failed: %s\n", strerror(errno));
        ata_recovery_hint(d);
        return -1;
    }
    blk[0] = p->enhanced ? 0x02 : 0x00; /* bit0 = 0: user password, bit1: enhanced */
    if (p->est_minutes)
        printf("  Running %s; the drive estimates %u minutes.\n", p->desc, p->est_minutes);
    else
        printf("  Running %s; the drive gives no time estimate.\n", p->desc);
    printf("  Do NOT power off or unplug the drive. This cannot be interrupted.\n");

    /* generous timeout: twice the estimate plus an hour, or 12 hours if unknown */
    timeout_ms = p->est_minutes ? (p->est_minutes * 2 + 60) * 60000u : 12 * 3600 * 1000u;
    double t0 = now_seconds();
    if (ata_cmd(fd, ATA_PROTO_PIO_OUT, ATA_SEC_ERASE_UNIT, blk, sizeof blk, timeout_ms) != 0) {
        fprintf(stderr, "safedel: SECURITY ERASE UNIT failed: %s\n", strerror(errno));
        ata_recovery_hint(d);
        return -1;
    }
    printf("  Security erase finished in %.0f seconds.\n", now_seconds() - t0);

    /* 3. a successful erase clears the password; double-check */
    if (ata_identify(fd, id) == 0 && (id[128] & 0x0002)) {
        fprintf(stderr, "safedel: erase reported success but security is still enabled\n");
        ata_recovery_hint(d);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------- public --- */

int fwerase_probe(int fd, const struct devinfo *d, struct fw_plan *p)
{
    memset(p, 0, sizeof *p);
    switch (d->bus) {
    case BUS_NVME:
        return nvme_probe(fd, d, p);
    case BUS_ATA:
        return ata_probe(fd, p);
    case BUS_USB:
        snprintf(p->why_not, sizeof p->why_not,
                 "firmware erase through USB bridges is unreliable and can brick drives");
        return -1;
    default:
        snprintf(p->why_not, sizeof p->why_not, "no firmware erase method for %s devices", bus_name(d->bus));
        return -1;
    }
}

int fwerase_run(int fd, const struct devinfo *d, const struct fw_plan *p)
{
    switch (p->kind) {
    case FW_NVME_SANITIZE:
    case FW_NVME_FORMAT:
        return nvme_run(fd, p);
    case FW_ATA_SECURITY_ERASE:
        return ata_run(fd, d, p);
    default:
        return -1;
    }
}
