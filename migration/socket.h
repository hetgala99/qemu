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

#ifndef QEMU_MIGRATION_SOCKET_H
#define QEMU_MIGRATION_SOCKET_H

#include "io/channel.h"
#include "io/task.h"
#include "io/net-listener.h"

int socket_args_len(void);
int total_multifd_channels(void);
int *get_multifd_channel_array(int count);
int multifd_index(int i);
void socket_send_channel_create(QIOTaskFunc f, void *data, int idx);
int socket_send_channel_destroy(QIOChannel *send);

void socket_start_incoming_migration(const char *str, uint8_t number,
                                     Error **errp);

void socket_start_outgoing_migration(MigrationState *s, const char *dst_str,
                                     const char *src_str, Error **errp);

void store_multifd_migration_params(const char *dst_uri, const char *src_uri,
                                    uint8_t multifd_channels,
                                    int idx, Error **erp);
#endif
