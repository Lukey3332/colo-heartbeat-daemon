/*
 * COLO background daemon
 *
 * Copyright (c) Lukas Straub <lukasstraub2@web.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <assert.h>

#include <glib-2.0/glib.h>
#include <glib-2.0/glib-unix.h>

#include "base_types.h"
#include "daemon.h"
#include "main_coroutine.h"
#include "coroutine.h"
#include "coroutine_stack.h"
#include "json_util.h"
#include "util.h"
#include "client.h"
#include "qmp.h"
#include "cpg.h"
#include "watchdog.h"

static FILE *trace = NULL;
static gboolean do_syslog = FALSE;

void colod_trace(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    if (trace) {
        vfprintf(trace, fmt, args);
        fflush(trace);
    }

    va_end(args);
}

void colod_syslog(int pri, const char *fmt, ...) {
    va_list args;

    if (trace) {
        va_start(args, fmt);
        vfprintf(trace, fmt, args);
        fwrite("\n", 1, 1, trace);
        fflush(trace);
        va_end(args);
    }

    va_start(args, fmt);
    if (do_syslog) {
        vsyslog(pri, fmt, args);
    } else {
        vfprintf(stderr, fmt, args);
        fwrite("\n", 1, 1, stderr);
    }
    va_end(args);
}



static gboolean colod_hup_cb(G_GNUC_UNUSED GIOChannel *channel,
                             G_GNUC_UNUSED GIOCondition revents,
                             gpointer data) {
    ColodContext *ctx = data;

    log_error("qemu quit");
    ctx->qemu_quit = TRUE;
    colod_event_queue(ctx, EVENT_QEMU_QUIT, "qmp hup");
    return G_SOURCE_REMOVE;
}

static void colod_qmp_event_cb(gpointer data, ColodQmpResult *result) {
    ColodContext *ctx = data;
    const gchar *event;

    event = get_member_str(result->json_root, "event");

    if (!strcmp(event, "QUORUM_REPORT_BAD")) {
        const gchar *node, *type;
        node = get_member_member_str(result->json_root, "data", "node-name");
        type = get_member_member_str(result->json_root, "data", "type");

        if (!strcmp(node, "nbd0")) {
            if (!!strcmp(type, "read")) {
                colod_event_queue(ctx, EVENT_FAILOVER_SYNC,
                                  "nbd write/flush error");
            }
        } else {
            if (!!strcmp(type, "read")) {
                colod_event_queue(ctx, EVENT_YELLOW,
                                  "local disk write/flush error");
            }
        }
    } else if (!strcmp(event, "COLO_EXIT")) {
        const gchar *reason;
        reason = get_member_member_str(result->json_root, "data", "reason");

        if (!strcmp(reason, "error")) {
            colod_event_queue(ctx, EVENT_FAILOVER_SYNC, "COLO_EXIT");
        }
    } else if (!strcmp(event, "RESET")) {
        colod_raise_timeout_coroutine(ctx);
    }
}

static void colod_mainloop(ColodContext *ctx) {
    GError *local_errp = NULL;

    // g_main_context_default creates the global context on demand
    GMainContext *main_context = g_main_context_default();
    ctx->mainloop = g_main_loop_new(main_context, FALSE);

    ctx->qmp = qmp_new(ctx->qmp1_fd, ctx->qmp2_fd, ctx->qmp_timeout_low,
                       &local_errp);
    if (!ctx->qmp) {
        colod_syslog(LOG_ERR, "Failed to initialize qmp: %s",
                     local_errp->message);
        g_error_free(local_errp);
        exit(EXIT_FAILURE);
    }

    colod_main_coroutine(ctx);

    ctx->listener = client_listener_new(ctx->mngmt_listen_fd, ctx);
    ctx->watchdog = colod_watchdog_new(ctx);
    ctx->cpg = cpg_new(ctx->cpg, &local_errp);
    if (!ctx->cpg) {
        colod_syslog(LOG_ERR, "Failed to initialize cpg: %s",
                     local_errp->message);
        g_error_free(local_errp);
        exit(EXIT_FAILURE);
    }

    qmp_add_notify_event(ctx->qmp, colod_qmp_event_cb, ctx);
    qmp_hup_source(ctx->qmp, colod_hup_cb, ctx);

    g_main_loop_run(ctx->mainloop);
    g_main_loop_unref(ctx->mainloop);
    ctx->mainloop = NULL;

    qmp_del_notify_event(ctx->qmp, colod_qmp_event_cb, ctx);
    cpg_free(ctx->cpg);
    colod_raise_timeout_coroutine_free(ctx);
    colod_main_free(ctx);
    colo_watchdog_free(ctx->watchdog);
    client_listener_free(ctx->listener);
    qmp_free(ctx->qmp);

    g_main_context_unref(main_context);
}

static int colod_open_mngmt(ColodContext *ctx, GError **errp) {
    int sockfd, ret;
    struct sockaddr_un address = { 0 };
    g_autofree char *path = NULL;

    path = g_strconcat(ctx->base_dir, "/colod.sock", NULL);
    if (strlen(path) >= sizeof(address.sun_path)) {
        colod_error_set(errp, "Management unix path too long");
        return -1;
    }
    strcpy(address.sun_path, path);
    address.sun_family = AF_UNIX;

    ret = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ret < 0) {
        colod_error_set(errp, "Failed to create management socket: %s",
                        g_strerror(errno));
        return -1;
    }
    sockfd = ret;

    unlink(path);
    ret = bind(sockfd, (const struct sockaddr *) &address, sizeof(address));
    if (ret < 0) {
        colod_error_set(errp, "Failed to bind management socket: %s",
                        g_strerror(errno));
        goto err;
    }

    ret = listen(sockfd, 2);
    if (ret < 0) {
        colod_error_set(errp, "Failed to listen management socket: %s",
                        g_strerror(errno));
        goto err;
    }

    ret = colod_fd_set_blocking(sockfd, FALSE, errp);
    if (ret < 0) {
        goto err;
    }

    ctx->mngmt_listen_fd = sockfd;
    return 0;

err:
    close(sockfd);
    return -1;
}

static int colod_open_qmp(ColodContext *ctx, GError **errp) {
    int ret;

    ret = colod_unix_connect(ctx->qmp_path, errp);
    if (ret < 0) {
        return -1;
    }
    ctx->qmp1_fd = ret;

    ret = colod_unix_connect(ctx->qmp_yank_path, errp);
    if (ret < 0) {
        close(ctx->qmp1_fd);
        ctx->qmp1_fd = 0;
        return -1;
    }
    ctx->qmp2_fd = ret;

    return 0;
}

static int colod_daemonize(ColodContext *ctx) {
    GError *local_errp = NULL;
    gchar *path;
    int logfd, pipefd, ret;

    pipefd = os_daemonize();

    path = g_strconcat(ctx->base_dir, "/colod.log", NULL);
    logfd = open(path, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
    g_free(path);

    if (logfd < 0) {
        openlog("colod", LOG_PID, LOG_DAEMON);
        syslog(LOG_ERR, "Fatal: Unable to open log file");
        exit(EXIT_FAILURE);
    }

    assert(logfd == 0);
    assert(dup(0) == 1);
    assert(dup(0) == 2);

    openlog("colod", LOG_PID, LOG_DAEMON);

    if (ctx->do_trace) {
        path = g_strconcat(ctx->base_dir, "/trace.log", NULL);
        trace = fopen(path, "a");
        g_free(path);
    }

    path = g_strconcat(ctx->base_dir, "/colod.pid", NULL);
    ret = colod_write_pidfile(path, &local_errp);
    g_free(path);
    if (!ret) {
        syslog(LOG_ERR, "Fatal: %s", local_errp->message);
        g_error_free(local_errp);
        exit(EXIT_FAILURE);
    }

    return pipefd;
}

static int colod_parse_options(ColodContext *ctx, int *argc, char ***argv,
                               GError **errp) {
    gboolean ret;
    GOptionContext *context;
    GOptionEntry entries[] =
    {
        {"daemonize", 'd', 0, G_OPTION_ARG_NONE, &ctx->daemonize, "Daemonize", NULL},
        {"syslog", 's', 0, G_OPTION_ARG_NONE, &do_syslog, "Log to syslog", NULL},
        {"instance_name", 'i', 0, G_OPTION_ARG_STRING, &ctx->instance_name, "The CPG group name for corosync communication", NULL},
        {"node_name", 'n', 0, G_OPTION_ARG_STRING, &ctx->node_name, "The node hostname", NULL},
        {"base_directory", 'b', 0, G_OPTION_ARG_FILENAME, &ctx->base_dir, "The base directory to store logs and sockets", NULL},
        {"qmp_path", 'q', 0, G_OPTION_ARG_FILENAME, &ctx->qmp_path, "The path to the qmp socket", NULL},
        {"qmp_yank_path", 'y', 0, G_OPTION_ARG_FILENAME, &ctx->qmp_yank_path, "The path to the qmp socket used for yank", NULL},
        {"timeout_low", 'l', 0, G_OPTION_ARG_INT, &ctx->qmp_timeout_low, "Low qmp timeout", NULL},
        {"timeout_high", 't', 0, G_OPTION_ARG_INT, &ctx->qmp_timeout_high, "High qmp timeout", NULL},
        {"watchdog_interval", 'a', 0, G_OPTION_ARG_INT, &ctx->watchdog_interval, "Watchdog interval (0 to disable)", NULL},
        {"primary", 'p', 0, G_OPTION_ARG_NONE, &ctx->primary, "Startup in primary mode", NULL},
        {"trace", 0, 0, G_OPTION_ARG_NONE, &ctx->do_trace, "Enable tracing", NULL},
        {0}
    };

    ctx->qmp_timeout_low = 600;
    ctx->qmp_timeout_high = 10000;

    context = g_option_context_new("- qemu colo heartbeat daemon");
    g_option_context_set_help_enabled(context, TRUE);
    g_option_context_add_main_entries(context, entries, 0);

    ret = g_option_context_parse(context, argc, argv, errp);
    g_option_context_free(context);
    if (!ret) {
        return -1;
    }

    if (!ctx->node_name || !ctx->instance_name || !ctx->base_dir ||
            !ctx->qmp_path) {
        g_set_error(errp, COLOD_ERROR, COLOD_ERROR_FATAL,
                    "--instance_name, --node_name, --base_directory and --qmp_path need to be given.");
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    GError *errp = NULL;
    ColodContext ctx_struct = { 0 };
    ColodContext *ctx = &ctx_struct;
    int ret;
    int pipefd = 0;

    ret = colod_parse_options(ctx, &argc, &argv, &errp);
    if (ret < 0) {
        fprintf(stderr, "%s\n", errp->message);
        g_error_free(errp);
        exit(EXIT_FAILURE);
    }

    if (ctx->daemonize) {
        pipefd = colod_daemonize(ctx);
    }
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    prctl(PR_SET_DUMPABLE, 1);

    signal(SIGPIPE, SIG_IGN); // TODO: Handle this properly

    ret = colod_open_qmp(ctx, &errp);
    if (ret < 0) {
        goto err;
    }

    ret = colod_open_mngmt(ctx, &errp);
    if (ret < 0) {
        goto err;
    }

    ctx->cpg = colod_open_cpg(ctx, &errp);
    if (!ctx->cpg) {
        goto err;
    }

    if (ctx->daemonize) {
        ret = os_daemonize_post_init(pipefd, &errp);
        if (ret < 0) {
            goto err;
        }
    }

    colod_mainloop(ctx);

    // cleanup pidfile, cpg, qmp and mgmt connection

    return EXIT_SUCCESS;

err:
    if (errp) {
        colod_syslog(LOG_ERR, "Fatal: %s", errp->message);
        g_error_free(errp);
    }
    exit(EXIT_FAILURE);
}
