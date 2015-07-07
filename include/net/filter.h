/*
 * Copyright (c) 2015 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#ifndef QEMU_NET_FILTER_H
#define QEMU_NET_FILTER_H

#include "qemu-common.h"
#include "qemu/typedefs.h"
#include "net/queue.h"

/* the netfilter chain */
enum {
    NET_FILTER_IN,
    NET_FILTER_OUT,
    NET_FILTER_ALL,
};

typedef void (FilterCleanup) (NetFilterState *);
/*
 * Return:
 *   0: finished handling the packet, we should continue
 *   size: filter stolen this packet, we stop pass this packet further
 */
typedef ssize_t (FilterReceiveIOV)(NetFilterState *nc,
                                   NetClientState *sender,
                                   unsigned flags,
                                   const struct iovec *iov,
                                   int iovcnt,
                                   NetPacketSent *sent_cb);

typedef struct NetFilterInfo {
    NetFilterType type;
    size_t size;
    FilterCleanup *cleanup;
    FilterReceiveIOV *receive_iov;
} NetFilterInfo;

struct NetFilterState {
    NetFilterInfo *info;
    char *name;
    char *model;
    NetClientState *netdev;
    int chain;
    QTAILQ_ENTRY(NetFilterState) global_list;
    QTAILQ_ENTRY(NetFilterState) next;
};

int net_init_filters(void);
NetFilterState *qemu_new_net_filter(NetFilterInfo *info,
                                    NetClientState *netdev,
                                    const char *name,
                                    const char *model,
                                    int chain);
void qemu_del_net_filter(NetFilterState *nf);
void netfilter_add(QemuOpts *opts, Error **errp);
const char *qemu_netfilter_get_chain_str(int chain);

int qemu_find_netfilters_by_model(const char *model, NetFilterState **nfs,
                                  int max);

/* pass the packet to the next filter */
ssize_t qemu_netfilter_pass_to_next(NetFilterState *nf, NetPacket *packet);

/* buffer filter */
void filter_buffer_release_all(void);

#endif /* QEMU_NET_FILTER_H */