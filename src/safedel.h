#ifndef SAFEDEL_SAFEDEL_H
#define SAFEDEL_SAFEDEL_H

#include "ptable.h"
#include "wipe.h"

#define MAX_PASSES 35

enum method { METHOD_AUTO, METHOD_OVERWRITE, METHOD_FIRMWARE };

struct options {
    enum method     method;
    enum table_type table;
    enum pattern    passes[MAX_PASSES];
    int             npasses;
    int             verify;
    int             dry_run;
};

/* Check, plan, confirm and erase one target. Returns the process exit code. */
int safedel_run(const char *path, const struct options *o);

#endif
