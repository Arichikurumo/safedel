#ifndef SAFEDEL_PTABLE_H
#define SAFEDEL_PTABLE_H

#include "device.h"

enum table_type { TABLE_GPT, TABLE_MBR, TABLE_NONE };

const char *table_name(enum table_type t);

/* Write a fresh, empty partition table (no partitions). */
int ptable_write(int fd, const struct devinfo *d, enum table_type t);

#endif
