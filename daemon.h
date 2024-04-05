/*
 * COLO background daemon
 *
 * Copyright (c) Lukas Straub <lukasstraub2@web.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef DAEMON_H
#define DAEMON_H

#include <syslog.h>
#include <stdio.h>

#include <glib-2.0/glib.h>

#include "base_types.h"
#include "util.h"
#include "qmp.h"

typedef struct ColodContext {
    /* Parameters */
    gchar *node_name, *instance_name, *base_dir;
    gchar *qmp_path, *qmp_yank_path;
    gboolean daemonize;
    guint qmp_timeout_low, qmp_timeout_high;
    guint checkpoint_interval;
    guint watchdog_interval;
    gboolean do_trace;
    gboolean primary_startup;

    /* Variables */
    GMainLoop *mainloop;

    int qmp1_fd, qmp2_fd, mngmt_listen_fd;

    ColodWatchdog *watchdog;
    Coroutine *raise_timeout_coroutine;
    JsonNode *migration_commands;
    JsonNode *failover_primary_commands, *failover_secondary_commands;
    ColodMainCoroutine *main_coroutine;

    ColodClientListener *listener;

    ColodQmpState *qmp;

    Cpg *cpg;
} ColodContext;

void colod_syslog(int pri, const char *fmt, ...)
     __attribute__ ((__format__ (__printf__, 2, 3)));
#define log_error(message) \
    colod_syslog(LOG_ERR, "%s: %s", __func__, message)
#define log_error_fmt(fmt, ...) \
    colod_syslog(LOG_ERR, "%s: " fmt, __func__, ##__VA_ARGS__)

void colod_trace(const char *fmt, ...);

#endif // DAEMON_H
