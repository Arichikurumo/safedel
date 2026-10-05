#ifndef SAFEDEL_FWERASE_H
#define SAFEDEL_FWERASE_H

#include <stdint.h>

#include "device.h"

enum fw_kind { FW_NONE, FW_NVME_SANITIZE, FW_NVME_FORMAT, FW_ATA_SECURITY_ERASE };

struct fw_plan {
    enum fw_kind kind;
    uint32_t     cdw10;       /* NVMe: prepared command dword 10 */
    uint32_t     nsid;        /* NVMe Format: namespace id */
    int          enhanced;    /* ATA: use enhanced security erase */
    unsigned     est_minutes; /* drive's own time estimate, 0 = unknown */
    char         desc[128];
    char         why_not[256];
};

/* Query the drive (read-only). Returns 0 if a firmware erase is available and fills p. */
int fwerase_probe(int fd, const struct devinfo *d, struct fw_plan *p);

/* Run the erase described by p. Returns 0 on success. */
int fwerase_run(int fd, const struct devinfo *d, const struct fw_plan *p);

#endif
