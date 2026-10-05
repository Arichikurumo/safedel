#include "ptable.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define GPT_ENTRIES     128
#define GPT_ENTRY_SIZE  128
#define GPT_HEADER_SIZE 92

const char *table_name(enum table_type t)
{
    switch (t) {
    case TABLE_GPT:  return "GPT";
    case TABLE_MBR:  return "MBR";
    case TABLE_NONE: return "none";
    }
    return "?";
}

static uint32_t crc32(const uint8_t *p, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;

    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static int write_sectors(int fd, const struct devinfo *d, const void *buf, size_t len, uint64_t lba)
{
    if (pwrite_full(fd, buf, len, (off_t)(lba * d->sector_size)) != 0) {
        fprintf(stderr, "safedel: writing partition table at LBA %llu failed: %s\n",
                (unsigned long long)lba, strerror(errno));
        return -1;
    }
    return 0;
}

static void mbr_signature(uint8_t *sec)
{
    sec[510] = 0x55;
    sec[511] = 0xAA;
}

static void gpt_header(uint8_t *h, uint64_t my_lba, uint64_t alt_lba, uint64_t entries_lba,
                       uint64_t first, uint64_t last, const uint8_t guid[16], uint32_t entries_crc)
{
    memset(h, 0, GPT_HEADER_SIZE);
    memcpy(h, "EFI PART", 8);
    put_le32(h + 8, 0x00010000);       /* revision 1.0 */
    put_le32(h + 12, GPT_HEADER_SIZE);
    put_le64(h + 24, my_lba);
    put_le64(h + 32, alt_lba);
    put_le64(h + 40, first);
    put_le64(h + 48, last);
    memcpy(h + 56, guid, 16);
    put_le64(h + 72, entries_lba);
    put_le32(h + 80, GPT_ENTRIES);
    put_le32(h + 84, GPT_ENTRY_SIZE);
    put_le32(h + 88, entries_crc);
    put_le32(h + 16, crc32(h, GPT_HEADER_SIZE)); /* computed with this field zero */
}

int ptable_write(int fd, const struct devinfo *d, enum table_type t)
{
    uint32_t ss = d->sector_size;
    uint64_t nlba = d->size / ss;
    uint32_t entries_bytes = GPT_ENTRIES * GPT_ENTRY_SIZE;
    uint64_t elba = (entries_bytes + ss - 1) / ss; /* sectors used by the entry array */
    size_t entries_len = (size_t)(elba * ss);
    uint8_t *sec = NULL, *entries = NULL;
    int rc = -1;

    if (t == TABLE_NONE)
        return 0;

    /* sector-aligned buffers so this also works on an O_DIRECT descriptor */
    if (posix_memalign((void **)&sec, 4096, ss) != 0 ||
        posix_memalign((void **)&entries, 4096, entries_len) != 0) {
        fprintf(stderr, "safedel: out of memory\n");
        goto out;
    }
    memset(sec, 0, ss);
    memset(entries, 0, entries_len);

    if (t == TABLE_MBR) {
        uint8_t disk_id[4];
        random_bytes(disk_id, sizeof disk_id);
        memcpy(sec + 440, disk_id, 4);
        mbr_signature(sec);
        rc = write_sectors(fd, d, sec, ss, 0);
        goto out;
    }

    if (nlba < 2 * elba + 6) {
        fprintf(stderr, "safedel: device too small for GPT\n");
        goto out;
    }

    uint64_t first = 2 + elba;
    uint64_t last = nlba - 2 - elba;
    uint8_t guid[16];
    uint32_t ecrc = crc32(entries, entries_bytes);

    random_bytes(guid, sizeof guid);
    guid[7] = (guid[7] & 0x0F) | 0x40; /* RFC 4122 version 4 (mixed-endian field) */
    guid[8] = (guid[8] & 0x3F) | 0x80;

    /* Protective MBR: one 0xEE partition covering the disk */
    uint8_t *pe = sec + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;  /* CHS start */
    pe[4] = 0xEE;
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;  /* CHS end */
    put_le32(pe + 8, 1);
    put_le32(pe + 12, nlba - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(nlba - 1));
    mbr_signature(sec);
    if (write_sectors(fd, d, sec, ss, 0) != 0)
        goto out;

    /* Primary header + entries */
    memset(sec, 0, ss);
    gpt_header(sec, 1, nlba - 1, 2, first, last, guid, ecrc);
    if (write_sectors(fd, d, sec, ss, 1) != 0 ||
        write_sectors(fd, d, entries, entries_len, 2) != 0)
        goto out;

    /* Backup entries + header at the end of the disk */
    memset(sec, 0, ss);
    gpt_header(sec, nlba - 1, 1, nlba - 1 - elba, first, last, guid, ecrc);
    if (write_sectors(fd, d, entries, entries_len, nlba - 1 - elba) != 0 ||
        write_sectors(fd, d, sec, ss, nlba - 1) != 0)
        goto out;

    rc = 0;
out:
    free(sec);
    free(entries);
    return rc;
}
