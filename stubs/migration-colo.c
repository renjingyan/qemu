/*
 * COarse-grain LOck-stepping Virtual Machines for Non-stop Service (COLO)
 * (a.k.a. Fault Tolerance or Continuous Replication)
 *
 * Copyright (c) 2015 HUAWEI TECHNOLOGIES CO.,LTD.
 * Copyright (c) 2015 FUJITSU LIMITED
 * Copyright (c) 2015 Intel Corporation
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "migration/migration-colo.h"
#include "qmp-commands.h"

void colo_init_checkpointer(MigrationState *s)
{
}

void *colo_process_incoming_checkpoints(void *opaque)
{
    return NULL;
}

bool migrate_in_colo_state(void)
{
    return false;
}

void qmp_colo_lost_heartbeat(Error **errp)
{
    error_setg(errp, "COLO is not supported, please rerun configure"
                     " with --enable-colo option in order to support"
                     " COLO feature");
}
