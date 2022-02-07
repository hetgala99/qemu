/*
 * QEMU live migration via socket
 *
 * Copyright Red Hat, Inc. 2009-2016
 *
 * Authors:
 *  Chris Lalancette <clalance@redhat.com>
 *  Daniel P. Berrange <berrange@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"

#include "qemu/error-report.h"
#include "qapi/error.h"
#include "channel.h"
#include "socket.h"
#include "migration.h"
#include "qemu-file.h"
#include "io/channel-socket.h"
#include "io/net-listener.h"
#include "trace.h"
#include "multifd.h"

#define MAX_MULTIFD_SOCKETS  20

int length = 0;

struct SocketOutgoingArgs {
    SocketAddress *saddr;
} outgoing_args;

struct SocketArgs {
    SocketAddress *dst_addr;
    SocketAddress *src_addr;
    uint8_t multifd_channels;
    
};

struct SocketArgs *pt = NULL;


int total_multifd_channels(void)
{
/*  
    struct SocketArgs *cap = &ptr;
*/
    int channel_sum = 0;
    for (int i=0; i<length; i++) {
        channel_sum += (pt+i)->multifd_channels;
    }
    return channel_sum;
}

int *get_multifd_channel_array(int count) 
{
    int *r = malloc(count);
    int running_sum = 0;
    for(int i=0; i<count; i++) {
        running_sum += (pt+i)->multifd_channels;
        r[i] = running_sum;    
    }
    return r;
}

int multifd_index(int i) {
    int *p = get_multifd_channel_array(length);
    int j = 0;
    while (j < length) {
        if(i >= *(p+j)) {
            j++;
        }
        else {
            break;
        }
    }
    return j;
}

void socket_send_channel_create(QIOTaskFunc f, void *data, int idx)
{ 
/*
    QIOChannelSocket *sioc = qio_channel_socket_new();
    for(int i=0; i<MAX_MULTIFD_SOCKETS; i++) {
        uint8_t channel_count = pt[i].multifd_channels;
        while (channel_count) {
            qio_channel_socket_connect_async(sioc, pt[i].dst_addr,
                                      f, data, NULL, NULL, src_uri);
            channel_count -= 1;
        }    
    }
    free(pt);
*/
    QIOChannelSocket *sioc = qio_channel_socket_new();
    qio_channel_socket_connect_async(sioc, pt[idx].dst_addr,
                                 f, data, NULL, NULL, pt[idx].src_addr);
}

int socket_send_channel_destroy(QIOChannel *send)
{
    /* Remove channel */
    object_unref(OBJECT(send));
    if (outgoing_args.saddr) {
        qapi_free_SocketAddress(outgoing_args.saddr);
        outgoing_args.saddr = NULL;
    }
    return 0;
}

struct SocketConnectData {
    MigrationState *s;
    char *hostname;
};

static void socket_connect_data_free(void *opaque)
{
    struct SocketConnectData *data = opaque;
    if (!data) {
        return;
    }
    g_free(data->hostname);
    g_free(data);
}

static void socket_outgoing_migration(QIOTask *task,
                                      gpointer opaque)
{
    struct SocketConnectData *data = opaque;
    QIOChannel *sioc = QIO_CHANNEL(qio_task_get_source(task));
    Error *err = NULL;

    if (qio_task_propagate_error(task, &err)) {
        trace_migration_socket_outgoing_error(error_get_pretty(err));
    } else {
        trace_migration_socket_outgoing_connected(data->hostname);
    }
    migration_channel_connect(data->s, sioc, data->hostname, err);
    object_unref(OBJECT(sioc));
}

static void
socket_start_outgoing_migration_internal(MigrationState *s,
                                         SocketAddress *saddr,
                                         SocketAddress *src_addr,
                                         Error **errp) 
{
    QIOChannelSocket *sioc = qio_channel_socket_new();
    struct SocketConnectData *data = g_new0(struct SocketConnectData, 1);

    data->s = s;

    /* in case previous migration leaked it */
    qapi_free_SocketAddress(outgoing_args.saddr);
    outgoing_args.saddr = saddr;

    if (saddr->type == SOCKET_ADDRESS_TYPE_INET) {
        data->hostname = g_strdup(saddr->u.inet.host);
    }

    qio_channel_set_name(QIO_CHANNEL(sioc), "migration-socket-outgoing");
    qio_channel_socket_connect_async(sioc,
                                     saddr,
                                     socket_outgoing_migration,
                                     data,
                                     socket_connect_data_free,
                                     NULL, src_addr); 
}


void socket_start_outgoing_migration(MigrationState *s,
                                     const char *dst_str,
                                     const char *src_str,
                                     Error **errp) 
{
    Error *err = NULL;
    SocketAddress *dst_addr = socket_parse(dst_str, &err);
    SocketAddress *src_addr = socket_parse(src_str, &err); 
    if (!err) {
        socket_start_outgoing_migration_internal(s, dst_addr, src_addr, &err);
    }
    error_propagate(errp, err);
}

void store_multifd_migration_params(const char *dst_uri,
                                    const char *src_uri, 
                                    uint8_t multifd_channels,
                                    int idx, Error **errp) 
{ 
    Error *err = NULL;
    const char *p1 = NULL, *p2 = NULL;

    if(pt == NULL) {
        pt = (struct SocketArgs *)malloc(sizeof(struct SocketArgs) * MAX_MULTIFD_SOCKETS);   
    }
    length = idx + 1;
    SocketAddress *dst_addr;
    SocketAddress *src_addr;
    if(strstart(dst_uri, "tcp:", &p1) && strstart(src_uri, "tcp:", &p2)) {
         dst_addr = socket_parse(p1 ? p1 : dst_uri, &err);
         src_addr = socket_parse(p2 ? p2 : src_uri, &err);
    }
   
    if(!err) {
        (pt+idx)->dst_addr = dst_addr;
        (pt+idx)->src_addr = src_addr;
        (pt+idx)->multifd_channels = multifd_channels;
    }
    error_propagate(errp, err);
}

static void socket_accept_incoming_migration(QIONetListener *listener,
                                             QIOChannelSocket *cioc,
                                             gpointer opaque)
{
    trace_migration_socket_incoming_accepted();

    if (migration_has_all_channels()) {
        error_report("%s: Extra incoming migration connection; ignoring",
                     __func__);
        return;
    }

    qio_channel_set_name(QIO_CHANNEL(cioc), "migration-socket-incoming");
    migration_channel_process_incoming(QIO_CHANNEL(cioc));
}

static void
socket_incoming_migration_end(void *opaque)
{
    QIONetListener *listener = opaque;

    qio_net_listener_disconnect(listener);
    object_unref(OBJECT(listener));
}

static void
socket_start_incoming_migration_internal(SocketAddress *saddr,
                                         Error **errp)
{
    QIONetListener *listener = qio_net_listener_new();
    MigrationIncomingState *mis = migration_incoming_get_current();
    size_t i;
    int num = 1;

    qio_net_listener_set_name(listener, "migration-socket-listener");

    if (migrate_use_multifd()) {
        num = total_multifd_channels();
    }

    if (qio_net_listener_open_sync(listener, saddr, num, errp) < 0) {
        object_unref(OBJECT(listener));
        return;
    }

    mis->transport_data = listener;
    mis->transport_cleanup = socket_incoming_migration_end;

    qio_net_listener_set_client_func_full(listener,
                                          socket_accept_incoming_migration,
                                          NULL, NULL,
                                          g_main_context_get_thread_default());

    for (i = 0; i < listener->nsioc; i++)  {
        SocketAddress *address =
            qio_channel_socket_get_local_address(listener->sioc[i], errp);
        if (!address) {
            return;
        }
        migrate_add_address(address);
        qapi_free_SocketAddress(address);
    }
}

void socket_start_incoming_migration(const char *str, Error **errp)
{
    Error *err = NULL;
    SocketAddress *saddr = socket_parse(str, &err);
    if (!err) {
        socket_start_incoming_migration_internal(saddr, &err);
    }
    qapi_free_SocketAddress(saddr);
    error_propagate(errp, err);
}
