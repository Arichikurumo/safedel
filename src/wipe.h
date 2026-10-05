#ifndef SAFEDEL_WIPE_H
#define SAFEDEL_WIPE_H

#include "device.h"

enum pattern { PAT_RANDOM, PAT_ZERO, PAT_ONES };

enum { WIPE_OK = 0, WIPE_ERROR = -1, WIPE_INTERRUPTED = -2 };

int         pattern_parse(const char *s, enum pattern *out);
const char *pattern_name(enum pattern p);

/* Overwrite the whole target once per entry in passes, optionally verifying the last pass. */
int wipe_run(int fd, const struct devinfo *d, const enum pattern *passes, int npasses, int verify);

#endif
