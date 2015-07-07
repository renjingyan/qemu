/*
 * Copyright (c) 2015 FUJITSU LIMITED
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */

#include "qemu-common.h"
#include "qapi-visit.h"
#include "qapi/qmp/qerror.h"
#include "qemu/error-report.h"
#include "qapi-visit.h"
#include "qapi/opts-visitor.h"
#include "qapi/dealloc-visitor.h"
#include "qemu/config-file.h"
#include "qmp-commands.h"
#include "qemu/iov.h"

#include "net/filter.h"
#include "net/net.h"
#include "net/vhost_net.h"
#include "filters.h"
#include "net/queue.h"
#include "filters.h"

static QTAILQ_HEAD(, NetFilterState) net_filters;

const char *qemu_netfilter_get_chain_str(int chain)
{
    const char *str;

    switch (chain) {
    case NET_FILTER_IN:
        str = "in";
        break;
    case NET_FILTER_OUT:
        str = "out";
        break;
    case NET_FILTER_ALL:
        str = "all";
        break;
    default:
        str = "unknown";
        break;
    }

    return str;
}

NetFilterState *qemu_new_net_filter(NetFilterInfo *info,
                                    NetClientState *netdev,
                                    const char *name,
                                    const char *model,
                                    int chain)
{
    NetFilterState *nf;

    assert(info->size >= sizeof(NetFilterState));
    assert(info->receive_iov);

    nf = g_malloc0(info->size);
    nf->info = info;
    nf->name = g_strdup(name);
    nf->model = g_strdup(model);
    nf->netdev = netdev;
    nf->chain = chain;
    QTAILQ_INSERT_TAIL(&net_filters, nf, global_list);
    QTAILQ_INSERT_TAIL(&netdev->filters, nf, next);

    return nf;
}

static void qemu_cleanup_net_filter(NetFilterState *nf)
{
    QTAILQ_REMOVE(&nf->netdev->filters, nf, next);
    QTAILQ_REMOVE(&net_filters, nf, global_list);

    if (nf->info->cleanup) {
        nf->info->cleanup(nf);
    }

    g_free(nf->name);
    g_free(nf);
}

static int qemu_find_netfilters_by_name(const char *id, NetFilterState **nfs,
                                        int max)
{
    NetFilterState *nf;
    int ret = 0;

    QTAILQ_FOREACH(nf, &net_filters, global_list) {
        if (!strcmp(nf->name, id)) {
            if (ret < max) {
                nfs[ret] = nf;
            }
            ret++;
        }
    }

    return ret;
}

int qemu_find_netfilters_by_model(const char *model, NetFilterState **nfs,
                                  int max)
{
    NetFilterState *nf;
    int ret = 0;

    QTAILQ_FOREACH(nf, &net_filters, global_list) {
        if (!strcmp(nf->model, model)) {
            if (ret < max) {
                nfs[ret] = nf;
            }
            ret++;
        }
    }

    return ret;
}

void qemu_del_net_filter(NetFilterState *nf)
{
    NetFilterState *nfs[MAX_QUEUE_NUM];
    int queues, i;
    QemuOpts *opts;

    opts = qemu_opts_find(qemu_find_opts_err("netfilter", NULL), nf->name);

    queues = qemu_find_netfilters_by_name(nf->name, nfs, MAX_QUEUE_NUM);
    assert(queues != 0);

    for (i = 0; i < queues; i++) {
        qemu_cleanup_net_filter(nfs[i]);
    }

    qemu_opts_del(opts);
}

static NetFilterState *qemu_find_netfilter(const char *id)
{
    NetFilterState *nf;

    QTAILQ_FOREACH(nf, &net_filters, global_list) {
        if (!strcmp(nf->name, id)) {
            return nf;
        }
    }

    return NULL;
}

static int net_init_filter(void *dummy, QemuOpts *opts, Error **errp);
void netfilter_add(QemuOpts *opts, Error **errp)
{
    net_init_filter(NULL, opts, errp);
}

static int net_filter_init1(const NetFilter *netfilter, Error **errp);
void qmp_netfilter_add(NetFilter *data, Error **errp)
{
    net_filter_init1(data, errp);
}

void qmp_netfilter_del(const char *id, Error **errp)
{
    NetFilterState *nf;

    nf = qemu_find_netfilter(id);
    if (!nf) {
        error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                  "Filter '%s' not found", id);
        return;
    }

    qemu_del_net_filter(nf);
}

ssize_t qemu_netfilter_pass_to_next(NetFilterState *nf, NetPacket *packet)
{
    int ret = 0;
    int chain;
    NetFilterState *next = QTAILQ_NEXT(nf, next);
    struct iovec iov = {
        .iov_base = (void *)packet->data,
        .iov_len = packet->size
    };

    if (!packet->sender || !packet->sender->peer) {
        /* no receiver, or sender been deleted, no need to pass it further */
        goto out;
    }

    if (nf->chain == NET_FILTER_ALL) {
        if (packet->sender == nf->netdev) {
            /* This packet is sent by netdev itself */
            chain = NET_FILTER_OUT;
        } else {
            chain = NET_FILTER_IN;
        }
    } else {
        chain = nf->chain;
    }

    while (next) {
        if (next->chain == chain || next->chain == NET_FILTER_ALL) {
            ret = next->info->receive_iov(next, packet->sender, packet->flags,
                                          &iov, 1, packet->sent_cb);
            if (ret) {
                return ret;
            }
        }
        next = QTAILQ_NEXT(next, next);
    }

    /*
     * We have gone through all filters, pass it to receiver.
     * Do the valid check again incase sender or receiver been
     * deleted while we go through filters.
     */
    if (packet->sender && packet->sender->peer) {
        return qemu_net_queue_send_iov(packet->sender->peer->incoming_queue,
                                       packet->sender, packet->flags,
                                       &iov, 1, packet->sent_cb);
    }

out:
    /* no receiver, or sender been deleted */
    return packet->size;
}

typedef int (NetFilterInit)(const NetFilter *netfilter,
                            const char *name, int chain,
                            NetClientState *netdev, Error **errp);

static
NetFilterInit * const net_filter_init_fun[NET_FILTER_TYPE_MAX] = {
    [NET_FILTER_TYPE_BUFFER] = net_init_filter_buffer,
};

static int net_filter_init1(const NetFilter *netfilter, Error **errp)
{
    NetClientState *ncs[MAX_QUEUE_NUM];
    const char *name = netfilter->id;
    const char *netdev_id = netfilter->netdev;
    const char *chain_str = NULL;
    int chain, queues, i;
    NetFilterState *nf;

    if (!net_filter_init_fun[netfilter->kind]) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "type",
                   "a net filter type");
        return -1;
    }

    nf = qemu_find_netfilter(netfilter->id);
    if (nf) {
        error_setg(errp, "Filter '%s' already exists", netfilter->id);
        return -1;
    }

    if (netfilter->has_chain) {
        chain_str = netfilter->chain;
        if (!strcmp(chain_str, "in")) {
            chain = NET_FILTER_IN;
        } else if (!strcmp(chain_str, "out")) {
            chain = NET_FILTER_OUT;
        } else if (!strcmp(chain_str, "all")) {
            chain = NET_FILTER_ALL;
        } else {
            error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "chain",
                       "netfilter chain (in/out/all)");
            return -1;
        }
    } else {
        /* default */
        chain = NET_FILTER_ALL;
    }

    queues = qemu_find_net_clients_except(netdev_id, ncs,
                                          NET_CLIENT_OPTIONS_KIND_NIC,
                                          MAX_QUEUE_NUM);
    if (queues < 1) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "netdev",
                   "a network backend id");
        return -1;
    }

    if (get_vhost_net(ncs[0])) {
        error_setg(errp, "vhost is not supported");
        return -1;
    }

    for (i = 0; i < queues; i++) {
        if (net_filter_init_fun[netfilter->kind](netfilter, name,
                                                 chain, ncs[i], errp) < 0) {
            if (errp && !*errp) {
                error_setg(errp, QERR_DEVICE_INIT_FAILED,
                           NetFilterType_lookup[netfilter->kind]);
            }
            return -1;
        }
    }

    return 0;
}

static int net_init_filter(void *dummy, QemuOpts *opts, Error **errp)
{
    NetFilter *object = NULL;
    Error *err = NULL;
    int ret = -1;
    OptsVisitor *ov = opts_visitor_new(opts);

    visit_type_NetFilter(opts_get_visitor(ov), &object, NULL, &err);
    opts_visitor_cleanup(ov);

    if (!err) {
        ret = net_filter_init1(object, &err);
    }

    if (object) {
        QapiDeallocVisitor *dv = qapi_dealloc_visitor_new();

        visit_type_NetFilter(qapi_dealloc_get_visitor(dv), &object, NULL, NULL);
        qapi_dealloc_visitor_cleanup(dv);
    }

    if (errp) {
        error_propagate(errp, err);
    } else if (err) {
        error_report_err(err);
    }

    return ret;
}

int net_init_filters(void)
{
    QTAILQ_INIT(&net_filters);

    if (qemu_opts_foreach(qemu_find_opts("netfilter"),
                          net_init_filter, NULL, NULL)) {
        return -1;
    }

    return 0;
}

QemuOptsList qemu_netfilter_opts = {
    .name = "netfilter",
    .implied_opt_name = "type",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_netfilter_opts.head),
    .desc = {
        /*
         * no elements => accept any params
         * validation will happen later
         */
        { /* end of list */ }
    },
};