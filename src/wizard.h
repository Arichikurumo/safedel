#ifndef SAFEDEL_WIZARD_H
#define SAFEDEL_WIZARD_H

#include <stddef.h>

#include "safedel.h"

/* Print all disks and whether they can be erased. Returns the process exit code. */
int wizard_list(void);

/*
 * Interactive mode: pick a disk and answer a few questions.
 * Fills path and o; returns 0 if the user chose a disk, -1 if they quit.
 */
int wizard_run(struct options *o, char *path, size_t len);

#endif
