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

#include <corosync/cpg.h>
#include <corosync/corotypes.h>

#include "base_types.h"
#include "daemon.h"
#include "coroutine.h"
#include "coutil.h"
#include "json_util.h"
#include "util.h"
#include "client.h"
#include "qmp.h"
#include "coroutine_stack.h"

static gboolean do_syslog = FALSE;

void colod_syslog(int pri, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    if (do_syslog) {
        vsyslog(pri, fmt, args);
    } else {
        vfprintf(stderr, fmt, args);
        fwrite("\n", 1, 1, stderr);
    }

    va_end(args);
}

static gboolean colod_cpg_readable(G_GNUC_UNUSED gint fd,
                                   G_GNUC_UNUSED GIOCondition events,
                                   gpointer data) {
    ColodContext *ctx = data;
    cpg_dispatch(ctx->cpg_handle, CS_DISPATCH_ALL);
    return G_SOURCE_CONTINUE;
}

void colod_set_migration_commands(ColodContext *ctx, JsonNode *commands) {
    if (ctx->migration_commands) {
        json_node_unref(ctx->migration_commands);
    }
    ctx->migration_commands = json_node_ref(commands);
}

void colod_set_primary_commands(ColodContext *ctx, JsonNode *commands) {
    if (ctx->failover_primary_commands) {
        json_node_unref(ctx->failover_primary_commands);
    }
    ctx->failover_primary_commands = json_node_ref(commands);
}

void colod_set_secondary_commands(ColodContext *ctx, JsonNode *commands) {
    if (ctx->failover_secondary_commands) {
        json_node_unref(ctx->failover_secondary_commands);
    }
    ctx->failover_secondary_commands = json_node_ref(commands);
}

static gboolean qemu_runnng(const gchar *status) {
    return !strcmp(status, "running")
            || !strcmp(status, "finish-migrate")
            || !strcmp(status, "colo");
}

#define qemu_query_status_co(result, qmp, role, replication, errp) \
    co_call_co((result), _qemu_query_status_co, (qmp), (role), \
               (replication), (errp))

static int _qemu_query_status_co(Coroutine *coroutine, ColodQmpState *qmp,
                                 ColodRole *role, gboolean *replication,
                                 GError **errp) {
    ColodCo *co = co_stack(colodco);

    co_begin(int, -1);

    qmp_execute_co(CO qemu_status, qmp, errp,
                   "{'execute': 'query-status'}\n");
    if (!CO qemu_status) {
        return -1;
    }

    qmp_execute_co(CO colo_status, qmp, errp,
                   "{'execute': 'query-colo-status'}\n");
    if (!CO colo_status) {
        qmp_result_free(CO qemu_status);
        return -1;
    }

    co_end;

    const gchar *status, *colo_mode, *colo_reason;
    status = get_member_member_str(CO qemu_status->json_root,
                                   "return", "status");
    colo_mode = get_member_member_str(CO colo_status->json_root,
                                      "return", "mode");
    colo_reason = get_member_member_str(CO colo_status->json_root,
                                        "return", "reason");
    if (!status || !colo_mode || !colo_reason) {
        colod_error_set(errp, "Failed to parse query-status "
                        "and query-colo-status output");
        qmp_result_free(CO qemu_status);
        qmp_result_free(CO colo_status);
        return -1;
    }

    if (!strcmp(status, "inmigrate") || !strcmp(status, "shutdown")) {
        *role = ROLE_SECONDARY;
        *replication = FALSE;
    } else if (qemu_runnng(status) && !strcmp(colo_mode, "none")
               && (!strcmp(colo_reason, "none")
                   || !strcmp(colo_reason, "request"))) {
        *role = ROLE_PRIMARY;
        *replication = FALSE;
    } else if (qemu_runnng(status) &&!strcmp(colo_mode, "primary")) {
        *role = ROLE_PRIMARY;
        *replication = TRUE;
    } else if (qemu_runnng(status) && !strcmp(colo_mode, "secondary")) {
        *role = ROLE_SECONDARY;
        *replication = TRUE;
    } else {
        colod_error_set(errp, "Unknown qemu status: %s, %s",
                        CO qemu_status->line, CO colo_status->line);
        qmp_result_free(CO qemu_status);
        qmp_result_free(CO colo_status);
        return -1;
    }

    qmp_result_free(CO qemu_status);
    qmp_result_free(CO colo_status);
    return 0;
}

int _colod_check_health_co(Coroutine *coroutine, ColodContext *ctx,
                           GError **errp) {
    ColodRole role;
    gboolean replication;
    int ret;

    ret = _qemu_query_status_co(coroutine, ctx->qmp, &role, &replication,
                                errp);
    if (coroutine->yield) {
        return -1;
    }
    if (ret < 0) {
        return -1;
    }

    ret = qmp_get_error(ctx->qmp, errp);
    if (ret < 0) {
        return -1;
    }

    if (ctx->role != role || ctx->replication != replication) {
        colod_error_set(errp, "qemu status mismatch");
        return -1;
    }

    return 0;
}

void colod_quit(ColodContext *ctx) {
    g_main_loop_quit(ctx->mainloop);
}

typedef struct ColodWatchdog {
    Coroutine coroutine;
    ColodContext *ctx;
    guint interval;
    guint timer_id;
    gboolean quit;
} ColodWatchdog;

static void colod_watchdog_refresh(ColodWatchdog *state) {
    if (state->timer_id) {
        g_source_remove(state->timer_id);
        state->timer_id = g_timeout_add_full(G_PRIORITY_LOW,
                                             state->interval,
                                             state->coroutine.cb.plain,
                                             &state->coroutine, NULL);
    }
}

static void colod_watchdog_event_cb(gpointer data,
                                    G_GNUC_UNUSED ColodQmpResult *result) {
    ColodWatchdog *state = data;
    colod_watchdog_refresh(state);
}

static gboolean _colod_watchdog_co(Coroutine *coroutine);
static gboolean colod_watchdog_co(gpointer data) {
    ColodWatchdog *state = data;
    Coroutine *coroutine = &state->coroutine;
    gboolean ret;

    co_enter(ret, coroutine, _colod_watchdog_co);
    if (coroutine->yield) {
        return GPOINTER_TO_INT(coroutine->yield_value);
    }

    g_source_remove_by_user_data(coroutine);
    assert(!g_source_remove_by_user_data(coroutine));
    return ret;
}

static gboolean colod_watchdog_co_wrap(
        G_GNUC_UNUSED GIOChannel *channel,
        G_GNUC_UNUSED GIOCondition revents,
        gpointer data) {
    return colod_watchdog_co(data);
}

static gboolean _colod_watchdog_co(Coroutine *coroutine) {
    ColodWatchdog *state = (ColodWatchdog *) coroutine;
    int ret;
    GError *local_errp = NULL;

    co_begin(gboolean, G_SOURCE_CONTINUE);

    while (!state->quit) {
        state->timer_id = g_timeout_add_full(G_PRIORITY_LOW,
                                             state->interval,
                                             coroutine->cb.plain,
                                             coroutine, NULL);
        co_yield_int(G_SOURCE_REMOVE);
        if (state->quit) {
            break;
        }
        state->timer_id = 0;

        colod_check_health_co(ret, state->ctx, &local_errp);
        if (ret < 0) {
            g_error_free(local_errp);
            local_errp = NULL;
            // qemu died...
        }
    }

    co_end;

    return G_SOURCE_REMOVE;
}

static void colo_watchdog_free(ColodWatchdog *state) {

    if (!state->interval) {
        g_free(state);
        return;
    }

    state->quit = TRUE;

    qmp_del_notify_event(state->ctx->qmp, colod_watchdog_event_cb, state);

    if (state->timer_id) {
        g_source_remove(state->timer_id);
        state->timer_id = 0;
        g_idle_add(colod_watchdog_co, &state->coroutine);
    }

    while (!state->coroutine.quit) {
        g_main_context_iteration(g_main_context_default(), TRUE);
    }

    g_free(state);
}

static ColodWatchdog *colod_watchdog_new(ColodContext *ctx) {
    ColodWatchdog *state;
    Coroutine *coroutine;

    state = g_new0(ColodWatchdog, 1);
    coroutine = &state->coroutine;
    coroutine->cb.plain = colod_watchdog_co;
    coroutine->cb.iofunc = colod_watchdog_co_wrap;
    state->ctx = ctx;
    state->interval = ctx->watchdog_interval;

    if (state->interval) {
        g_idle_add(colod_watchdog_co, coroutine);
        qmp_add_notify_event(ctx->qmp, colod_watchdog_event_cb, state);
    }
    return state;
}

ColodQmpResult *_colod_execute_nocheck_co(Coroutine *coroutine,
                                          ColodContext *ctx,
                                          GError **errp,
                                          const gchar *command) {
    ColodQmpResult *result;

    result = _qmp_execute_co(coroutine, ctx->qmp, errp, command);
    if (coroutine->yield) {
        return NULL;
    }
    if (!result) {
        return NULL;
    }

    return result;
}

ColodQmpResult *_colod_execute_co(Coroutine *coroutine,
                                  ColodContext *ctx,
                                  GError **errp,
                                  const gchar *command) {
    ColodQmpResult *result;

    result = _colod_execute_nocheck_co(coroutine, ctx, errp, command);
    if (coroutine->yield) {
        return NULL;
    }
    if (!result) {
        return NULL;
    }
    if (has_member(result->json_root, "error")) {
        g_set_error(errp, COLOD_ERROR, COLOD_ERROR_QMP,
                    "qmp command returned error: %s %s",
                    command, result->line);
        qmp_result_free(result);
        return NULL;
    }

    return result;
}

#define colod_execute_array(ret, ctx, array, ignore_errors, errp) \
    co_call_co((ret), _colod_execute_array_co, (ctx), (array), \
               (ignore_errors), (errp))

static int _colod_execute_array_co(Coroutine *coroutine, ColodContext *ctx,
                                   JsonNode *array, gboolean ignore_errors,
                                   GError **errp) {
    ColodArrayCo *co = co_stack(colodarrayco);
    int ret = 0;
    GError *local_errp = NULL;

    co_begin(int, -1);

    assert(!errp || !*errp);
    assert(JSON_NODE_HOLDS_ARRAY(array));

    CO reader = json_reader_new(array);

    CO count = json_reader_count_elements(CO reader);
    for (CO i = 0; CO i < CO count; CO i++) {
        json_reader_read_element(CO reader, CO i);
        JsonNode *node = json_reader_get_value(CO reader);
        assert(node);

        gchar *tmp = json_to_string(node, FALSE);
        CO line = g_strdup_printf("%s\n", tmp);
        g_free(tmp);

        ColodQmpResult *result;
        colod_execute_co(result, ctx, &local_errp, CO line);
        if (ignore_errors &&
                g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
            colod_syslog(LOG_WARNING, "Ignoring qmp error: %s",
                         local_errp->message);
            g_error_free(local_errp);
            local_errp = NULL;
        } else if (!result) {
            g_propagate_error(errp, local_errp);
            json_reader_end_element(CO reader);
            g_free(CO line);
            ret = -1;
            break;
        }
        qmp_result_free(result);

        json_reader_end_element(CO reader);
    }
    json_reader_end_member(CO reader);
    g_object_unref(CO reader);

    co_end;

    return ret;
}

enum ColodEvent {
    EVENT_NONE = 0,
    EVENT_FAILED,
    EVENT_PEER_FAILOVER,
    EVENT_DO_FAILOVER,
    EVENT_FAILOVER_WIN,
    EVENT_QUIT,
    EVENT_YELLOW,
    EVENT_START_MIGRATION
};

typedef enum ColodResult {
    RESULT_SUCCESS = 0,
    RESULT_FAILED,
    RESULT_PEER_FAILOVER,
    RESULT_DID_FAILOVER,
    RESULT_QUIT
} ColodResult;

static gboolean colod_event_critical(ColodEvent event) {
    switch (event) {
        case EVENT_NONE:
        case EVENT_YELLOW:
        case EVENT_START_MIGRATION:
            return FALSE;
        break;

        default:
            return TRUE;
        break;
    }
}

static gboolean colod_event_failed(ColodEvent event) {
    assert(event != EVENT_FAILOVER_WIN);

    switch (event) {
        case EVENT_FAILED:
        case EVENT_PEER_FAILOVER:
            return TRUE;
        break;

        default:
            return FALSE;
        break;
    }
}

static ColodResult colod_failed_result(ColodEvent event) {
    switch (event) {
        case EVENT_FAILED:
            return RESULT_FAILED;
        break;

        case EVENT_PEER_FAILOVER:
            return RESULT_PEER_FAILOVER;
        break;

        default:
            abort();
        break;
    }
}

static gboolean colod_event_failover(ColodEvent event) {
    assert(event != EVENT_FAILOVER_WIN);

    return event == EVENT_DO_FAILOVER;
}

static gboolean colod_event_quit(ColodEvent event) {
    assert(event != EVENT_FAILOVER_WIN);

    return event == EVENT_QUIT;
}

static ColodResult colod_critical_result(ColodEvent event) {
    if (colod_event_failed(event)) {
        return colod_failed_result(event);
    } else if (colod_event_quit(event)) {
        return RESULT_QUIT;
    } else {
        abort();
    }
}

static gboolean colod_event_pending(ColodContext *ctx) {
    return !queue_empty(&ctx->events) || !queue_empty(&ctx->critical_events);
}

static gboolean colod_main_co(gpointer data);
static void colod_event_queue(ColodContext *ctx, ColodEvent event) {
    assert(event != EVENT_NONE);
    ColodQueue *queue;

    if (colod_event_critical(event)) {
        queue = &ctx->critical_events;
    } else {
        queue = &ctx->events;
    }

    if (queue_empty(queue) && ctx->main_coroutine) {
        g_idle_add(colod_main_co, ctx);
    }

    queue_add(queue, event);
}

#define colod_event_wait(result, ctx) \
    co_call_co(result, _colod_event_wait, ctx)
static ColodEvent _colod_event_wait(Coroutine *coroutine, ColodContext *ctx) {
    ColodQueue *queue = &ctx->events;

    if (!colod_event_pending(ctx)) {
        coroutine->yield = TRUE;
        coroutine->yield_value = GINT_TO_POINTER(G_SOURCE_REMOVE);
        return EVENT_NONE;
    }

    if (!queue_empty(&ctx->critical_events)) {
        queue = &ctx->critical_events;
    }

    return queue_remove(queue);
}

static gboolean colod_critical_pending(ColodContext *ctx) {
    return !queue_empty(&ctx->critical_events);
}

#define colod_qmp_event_wait_co(result, ctx, timeout, event, errp) \
    co_call_co(result, _colod_qmp_event_wait_co, ctx, timeout, event, errp)
static int _colod_qmp_event_wait_co(Coroutine *coroutine, ColodContext *ctx,
                                    guint timeout, const gchar* event,
                                    GError **errp) {
    int ret;
    GError *local_errp = NULL;

    while (TRUE) {
        ret = _qmp_wait_event_co(coroutine, ctx->qmp, timeout, event,
                                 &local_errp);
        if (coroutine->yield) {
            return -1;
        }
        if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_INTERRUPT)) {
            assert(colod_event_pending(ctx));
            if (!colod_critical_pending(ctx)) {
                g_error_free(local_errp);
                local_errp = NULL;
                continue;
            }
            g_propagate_error(errp, local_errp);
            return ret;
        } else if (ret < 0) {
            g_propagate_error(errp, local_errp);
            return ret;
        }

        break;
    }

    return ret;
}

typedef enum ColodMessage {
    MESSAGE_FAILOVER,
    MESSAGE_FAILED
} ColodMessage;

static void colod_cpg_deliver(cpg_handle_t handle,
                              G_GNUC_UNUSED const struct cpg_name *group_name,
                              uint32_t nodeid,
                              G_GNUC_UNUSED uint32_t pid,
                              void *msg,
                              size_t msg_len) {
    ColodContext *ctx;
    uint32_t conv;
    uint32_t myid;

    cpg_context_get(handle, (void**) &ctx);
    cpg_local_get(handle, &myid);

    if (msg_len != sizeof(conv)) {
        log_error_fmt("Got message of invalid length %zu", msg_len);
        return;
    }
    conv = ntohl((*(uint32_t*)msg));

    switch (conv) {
        case MESSAGE_FAILOVER:
            if (nodeid == myid) {
                colod_event_queue(ctx, EVENT_FAILOVER_WIN);
            } else {
                colod_event_queue(ctx, EVENT_PEER_FAILOVER);
            }
        break;

        case MESSAGE_FAILED:
            if (nodeid != myid) {
                colod_event_queue(ctx, EVENT_DO_FAILOVER);
            }
        break;
    }
}

static void colod_cpg_confchg(cpg_handle_t handle,
    G_GNUC_UNUSED const struct cpg_name *group_name,
    G_GNUC_UNUSED const struct cpg_address *member_list,
    G_GNUC_UNUSED size_t member_list_entries,
    G_GNUC_UNUSED const struct cpg_address *left_list,
    size_t left_list_entries,
    G_GNUC_UNUSED const struct cpg_address *joined_list,
    G_GNUC_UNUSED size_t joined_list_entries) {
    ColodContext *ctx;

    cpg_context_get(handle, (void**) &ctx);

    if (left_list_entries) {
        colod_event_queue(ctx, EVENT_DO_FAILOVER);
    }
}

static void colod_cpg_totem_confchg(G_GNUC_UNUSED cpg_handle_t handle,
                                    G_GNUC_UNUSED struct cpg_ring_id ring_id,
                                    G_GNUC_UNUSED uint32_t member_list_entries,
                                    G_GNUC_UNUSED const uint32_t *member_list) {

}

static void colod_cpg_send(ColodContext *ctx, uint32_t message) {
    struct iovec vec;
    uint32_t conv = htonl(message);

    if (ctx->disable_cpg) {
        if (message == MESSAGE_FAILOVER) {
            colod_event_queue(ctx, EVENT_FAILOVER_WIN);
        }
        return;
    }

    vec.iov_len = sizeof(conv);
    vec.iov_base = &conv;
    cpg_mcast_joined(ctx->cpg_handle, CPG_TYPE_AGREED, &vec, 1);
}

#define colod_stop_co(result, ctx, errp) \
    co_call_co((result), _colod_stop_co, ctx, errp)
static int _colod_stop_co(Coroutine *coroutine, ColodContext *ctx,
                          GError **errp) {
    ColodQmpResult *result;

    result = _colod_execute_co(coroutine, ctx, errp,
                               "{'execute': 'stop'}\n");
    if (coroutine->yield) {
        return -1;
    }
    if (!result) {
        return -1;
    }
    qmp_result_free(result);

    return 0;
}

#define colod_failover_co(result, ctx) \
    co_call_co((result), _colod_failover_co, ctx)
static ColodResult _colod_failover_co(Coroutine *coroutine, ColodContext *ctx) {
    ColodCo *co = co_stack(colodco);
    int ret;
    GError *local_errp = NULL;

    co_begin(gboolean, G_SOURCE_CONTINUE);

    colod_cpg_send(ctx, MESSAGE_FAILOVER);

    while (TRUE) {
        ColodEvent event;
        colod_event_wait(event, ctx);
        if (event == EVENT_FAILOVER_WIN) {
            break;
        } else if (colod_event_failed(event)) {
            return colod_failed_result(event);
        } else if (colod_event_quit(event)) {
            return RESULT_QUIT;
        }
    }

    qmp_yank_co(ret, ctx->qmp, &local_errp);
    if (ret < 0) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return RESULT_FAILED;
    }

    if (ctx->role == ROLE_PRIMARY) {
        CO commands = ctx->failover_primary_commands;
    } else {
        CO commands = ctx->failover_secondary_commands;
    }
    colod_execute_array(ret, ctx, CO commands, TRUE, &local_errp);
    if (ret < 0) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return RESULT_FAILED;
    }

    co_end;

    return RESULT_DID_FAILOVER;
}

#define colod_start_migration_co(result, ctx) \
    co_call_co((result), _colod_start_migration_co, (ctx));
static ColodResult _colod_start_migration_co(Coroutine *coroutine,
                                             ColodContext *ctx) {
    ColodQmpState *qmp = ctx->qmp;
    ColodQmpResult *qmp_result;
    ColodResult result;
    int ret;
    GError *local_errp = NULL;

    co_begin(ColodResult, RESULT_SUCCESS);
    colod_execute_co(qmp_result, ctx, &local_errp,
                     "{'execute': 'migrate-set-capabilities',"
                     "'arguments': {'capabilities': ["
                        "{'capability': 'events', 'state': true },"
                        "{'capability': 'pause-before-switchover', 'state': true }]}}\n");
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
        goto qmp_error;
    } else if (!qmp_result) {
        goto qemu_failed;
    }
    qmp_result_free(qmp_result);
    if (colod_critical_pending(ctx)) {
        goto handle_event;
    }

    colod_qmp_event_wait_co(ret, ctx, 5*60*1000,
                            "MIGRATION_STATUS_PRE_SWITCHOVER", &local_errp);
    if (ret < 0) {
        goto qmp_error;
    }

    colod_execute_array(ret, ctx, ctx->migration_commands, FALSE,
                        &local_errp);
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
        goto qmp_error;
    } else if (!qmp_result) {
        goto qemu_failed;
    }
    if (colod_critical_pending(ctx)) {
        goto handle_event;
    }

    qmp_set_timeout(qmp, ctx->qmp_timeout_high);

    colod_execute_co(qmp_result, ctx, &local_errp,
                     "{'execute': 'migrate-continue',"
                     "'arguments': {'state': 'pre-switchover'}}\n");
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
        qmp_set_timeout(qmp, ctx->qmp_timeout_low);
        goto qmp_error;
    } else if (!qmp_result) {
        qmp_set_timeout(qmp, ctx->qmp_timeout_low);
        goto qemu_failed;
    }
    qmp_result_free(qmp_result);
    if (colod_critical_pending(ctx)) {
        qmp_set_timeout(qmp, ctx->qmp_timeout_low);
        goto handle_event;
    }

    colod_qmp_event_wait_co(ret, ctx, 1000, "CONT", &local_errp);
    if (ret < 0) {
        qmp_set_timeout(qmp, ctx->qmp_timeout_low);
        goto qmp_error;
    }
    colod_qmp_event_wait_co(ret, ctx, ctx->checkpoint_interval + 1000, "STOP",
                            &local_errp);
    if (ret < 0) {
        qmp_set_timeout(qmp, ctx->qmp_timeout_low);
        goto qmp_error_colo;
    }
    colod_qmp_event_wait_co(ret, ctx, ctx->qmp_timeout_high, "CONT",
                            &local_errp);
    if (ret < 0) {
        qmp_set_timeout(qmp, ctx->qmp_timeout_low);
        goto qmp_error_colo;
    }

    qmp_set_timeout(qmp, ctx->qmp_timeout_low);

    return RESULT_SUCCESS;

qmp_error:
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_INTERRUPT)) {
        g_error_free(local_errp);
        local_errp = NULL;
        assert(colod_critical_pending(ctx));
        ColodEvent event;
        colod_event_wait(event, ctx);
        if (colod_event_failover(event)) {
            goto failover;
        } else {
            return colod_critical_result(event);
        }
    } else {
        log_error(local_errp->message);
        g_error_free(local_errp);
    }
    goto failover;

qmp_error_colo:
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_INTERRUPT)) {
        g_error_free(local_errp);
        local_errp = NULL;
        assert(colod_event_pending(ctx));
        ColodEvent event;
        colod_event_wait(event, ctx);
        if (colod_event_failover(event)) {
            goto failover_colo;
        } else {
            return colod_critical_result(event);
        }
    } else {
        log_error(local_errp->message);
        g_error_free(local_errp);
    }
    goto failover_colo;

qemu_failed:
    log_error(local_errp->message);
    g_error_free(local_errp);
    return RESULT_FAILED;

handle_event:
    assert(colod_critical_pending(ctx));
    ColodEvent event;
    colod_event_wait(event, ctx);
    if (colod_event_failover(event)) {
        goto failover;
    } else {
        return colod_critical_result(event);
    }

failover:
    colod_execute_co(qmp_result, ctx, &local_errp,
                     "{'execute': 'migrate_cancel'}\n");
    if (!qmp_result) {
        goto qemu_failed;
    }
    qmp_result_free(qmp_result);
    colod_failover_co(result, ctx);
    return result;

failover_colo:
    colod_failover_co(result, ctx);
    return result;

    co_end;

    return RESULT_FAILED;
}

#define colod_replication_wait_co(result, ctx) \
    co_call_co((result), _colod_replication_wait_co, (ctx))
static ColodResult _colod_replication_wait_co(Coroutine *coroutine,
                                              ColodContext *ctx) {
    ColodEvent event;
    int ret;
    GError *local_errp = NULL;

    co_begin(ColodResult, RESULT_FAILED);

    while (TRUE) {
        colod_qmp_event_wait_co(ret, ctx, 0, "CONT", &local_errp);
        if (ret < 0) {
            g_error_free(local_errp);
            assert(colod_event_pending(ctx));
            colod_event_wait(event, ctx);
            if (colod_event_failed(event) || colod_event_quit(event)) {
                return colod_critical_result(event);
            }
            continue;
        }
        break;
    }

    co_end;

    return RESULT_SUCCESS;
}

#define colod_replication_running_co(result, ctx) \
    co_call_co((result), _colod_replication_running_co, (ctx))
static ColodResult _colod_replication_running_co(Coroutine *coroutine,
                                                 ColodContext *ctx) {
    co_begin(ColodResult, RESULT_FAILED);

    while (TRUE) {
        ColodEvent event;
        colod_event_wait(event, ctx);
        if (colod_event_failover(event)) {
            ColodResult result;
            colod_failover_co(result, ctx);
            return result;
        } else if (colod_event_failed(event) || colod_event_quit(event)) {
            return colod_critical_result(event);
        }
    }

    co_end;

    return RESULT_FAILED;
}

int colod_start_migration(ColodContext *ctx) {
    if (ctx->pending_action || ctx->replication) {
        return -1;
    }

    colod_event_queue(ctx, EVENT_START_MIGRATION);
    return 0;
}

static gboolean _colod_main_co(Coroutine *coroutine, ColodContext *ctx);
static gboolean colod_main_co(gpointer data) {
    ColodContext *ctx = data;
    Coroutine *coroutine = ctx->main_coroutine;
    gboolean ret;

    co_enter(ret, coroutine, _colod_main_co, ctx);
    if (coroutine->yield) {
        return GPOINTER_TO_INT(coroutine->yield_value);
    }

    g_source_remove_by_user_data(coroutine);
    assert(!g_source_remove_by_user_data(coroutine));
    ctx->main_coroutine = NULL;
    g_free(coroutine);
    return ret;
}

static gboolean colod_main_co_wrap(
        G_GNUC_UNUSED GIOChannel *channel,
        G_GNUC_UNUSED GIOCondition revents,
        gpointer data) {
    return colod_main_co(data);
}

static gboolean _colod_main_co(Coroutine *coroutine, ColodContext *ctx) {
    ColodResult result;
    int ret;
    GError *local_errp = NULL;

    co_begin(gboolean, G_SOURCE_CONTINUE);

    if (ctx->role == ROLE_SECONDARY) {
        while (TRUE) {
            colod_replication_wait_co(result, ctx);
            if (result == RESULT_FAILED || result == RESULT_PEER_FAILOVER) {
                goto failed;
            } else if (result == RESULT_DID_FAILOVER) {
                break;
            } else if (result == RESULT_QUIT) {
                return G_SOURCE_REMOVE;
            }

            ctx->replication = TRUE;
            colod_replication_running_co(result, ctx);
            assert(result != RESULT_SUCCESS);
            if (result == RESULT_FAILED || result == RESULT_PEER_FAILOVER) {
                goto failed;
            } else if (result == RESULT_DID_FAILOVER) {
                break;
            } else if (result == RESULT_QUIT) {
                return G_SOURCE_REMOVE;
            }
        }
    }

    // Now running primary standalone
    ctx->role = ROLE_PRIMARY;
    ctx->replication = FALSE;

    while (TRUE) {
        ColodEvent event;
        colod_event_wait(event, ctx);
        if (event == EVENT_START_MIGRATION) {
            ctx->pending_action = TRUE;
            colod_start_migration_co(result, ctx);
            ctx->pending_action = FALSE;
            if (result == RESULT_FAILED || result == RESULT_PEER_FAILOVER) {
                goto failed;
            } else if (result == RESULT_DID_FAILOVER) {
                continue;
            } else if (result == RESULT_QUIT) {
                return G_SOURCE_REMOVE;
            }

            ctx->replication = TRUE;
            colod_replication_running_co(result, ctx);
            assert(result != RESULT_SUCCESS);
            if (result == RESULT_FAILED || result == RESULT_PEER_FAILOVER) {
                goto failed;
            } else if (result == RESULT_DID_FAILOVER) {
                ctx->replication = FALSE;
                continue;
            } else if (result == RESULT_QUIT) {
                return G_SOURCE_REMOVE;
            }
            ctx->replication = FALSE;
        } else if (colod_event_failed(event)) {
            result = colod_failed_result(event);
            goto failed;
        } else if (colod_event_quit(event)) {
            return G_SOURCE_REMOVE;
        }
    }

failed:
    ctx->failed = TRUE;
    colod_stop_co(ret, ctx, &local_errp);
    if (ret < 0) {
        if (result == RESULT_PEER_FAILOVER) {
            log_error_fmt("Failed to stop qemu in response to "
                          "peer failover: %s", local_errp->message);
        }
        g_error_free(local_errp);
        local_errp = NULL;
    }

    colod_cpg_send(ctx, MESSAGE_FAILED);

    while (TRUE) {
        ColodEvent event;
        colod_event_wait(event, ctx);
        if (colod_event_quit(event)) {
            return G_SOURCE_REMOVE;
        }
    }

    co_end;

    return G_SOURCE_REMOVE;
}

static Coroutine *colod_main_coroutine(ColodContext *ctx) {
    Coroutine *coroutine;

    assert(!ctx->main_coroutine);

    coroutine = g_new0(Coroutine, 1);
    coroutine->cb.plain = colod_main_co;
    coroutine->cb.iofunc = colod_main_co_wrap;
    ctx->main_coroutine = coroutine;

    g_idle_add(colod_main_co, ctx);
    return coroutine;
}

static void colod_main_free(ColodContext *ctx) {
    colod_event_queue(ctx, EVENT_QUIT);

    while (ctx->main_coroutine) {
        g_main_context_iteration(g_main_context_default(), TRUE);
    }
}

static void colod_mainloop(ColodContext *ctx) {
    GError *local_errp = NULL;

    // g_main_context_default creates the global context on demand
    ctx->mainctx = g_main_context_default();
    ctx->mainloop = g_main_loop_new(ctx->mainctx, FALSE);

    ctx->qmp = qmp_new(ctx->qmp1_fd, ctx->qmp2_fd, ctx->qmp_timeout_low,
                            &local_errp);
    if (local_errp) {
        colod_syslog(LOG_ERR, "Failed to initialize qmp: %s",
                     local_errp->message);
        g_error_free(local_errp);
        exit(EXIT_FAILURE);
    }

    ctx->listener = client_listener_new(ctx->mngmt_listen_fd, ctx);
    ctx->watchdog = colod_watchdog_new(ctx);
    colod_main_coroutine(ctx);
    if (!ctx->disable_cpg) {
        ctx->cpg_source_id = g_unix_fd_add(ctx->cpg_fd, G_IO_IN | G_IO_HUP,
                                           colod_cpg_readable, ctx);
    }

    g_main_loop_run(ctx->mainloop);
    g_main_loop_unref(ctx->mainloop);
    ctx->mainloop = NULL;

    if (ctx->cpg_source_id) {
        g_source_remove(ctx->cpg_source_id);
    }
    colod_main_free(ctx);
    colo_watchdog_free(ctx->watchdog);
    client_listener_free(ctx->listener);
    qmp_free(ctx->qmp);

    g_main_context_unref(ctx->mainctx);
}

cpg_model_v1_data_t cpg_data = {
    CPG_MODEL_V1,
    colod_cpg_deliver,
    colod_cpg_confchg,
    colod_cpg_totem_confchg,
    0
};

static int colod_open_cpg(ColodContext *ctx, GError **errp) {
    cs_error_t ret;
    int fd;
    struct cpg_name name;

    if (strlen(ctx->instance_name) >= sizeof(name.value)) {
        colod_error_set(errp, "Instance name too long");
        return -1;
    }
    strcpy(name.value, ctx->instance_name);
    name.length = strlen(name.value);

    ret = cpg_model_initialize(&ctx->cpg_handle, CPG_MODEL_V1,
                               (cpg_model_data_t*) &cpg_data, ctx);
    if (ret != CS_OK) {
        colod_error_set(errp, "Failed to initialize cpg: %s", cs_strerror(ret));
        return -1;
    }

    ret = cpg_join(ctx->cpg_handle, &name);
    if (ret != CS_OK) {
        colod_error_set(errp, "Failed to join cpg group: %s", cs_strerror(ret));
        goto err;
    }

    ret = cpg_fd_get(ctx->cpg_handle, &fd);
    if (ret != CS_OK) {
        colod_error_set(errp, "Failed to get cpg file descriptor: %s",
                        cs_strerror(ret));
        goto err_joined;
    }

    ctx->cpg_fd = fd;
    return 0;

err_joined:
    cpg_leave(ctx->cpg_handle, &name);
err:
    cpg_finalize(ctx->cpg_handle);
    return -1;
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
    struct sockaddr_un address = { 0 };
    int* fds[2] = { &ctx->qmp1_fd, &ctx->qmp2_fd };

    if (strlen(ctx->qmp_path) >= sizeof(address.sun_path)) {
        colod_error_set(errp, "Qmp unix path too long");
        return -1;
    }
    strcpy(address.sun_path, ctx->qmp_path);
    address.sun_family = AF_UNIX;

    for (int i = 0; i < 2; i++) {
        int ret;
        int *fd = fds[i];

        ret = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ret < 0) {
            colod_error_set(errp, "Failed to create qmp socket: %s",
                            g_strerror(errno));
            goto err;
        }
        *fd = ret;

        ret = connect(*fd, (const struct sockaddr *) &address,
                      sizeof(address));
        if (ret < 0) {
            colod_error_set(errp, "Failed to connect qmp socket: %s",
                            g_strerror(errno));
            goto err;
        }
    }

    return 0;

err:
    for (int i = 0; i < 2; i++) {
        int *fd = fds[i];
        if (*fd) {
            close(*fd);
            *fd = 0;
        }
    }
    return -1;
}

static int colod_daemonize(ColodContext *ctx) {
    GError *local_errp = NULL;
    char *path;
    int logfd, pipefd, ret;

    if (!ctx->daemonize) {
        return 0;
    }

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

int main(int argc, char **argv) {
    GError *errp = NULL;
    char *node_name, *instance_name, *base_dir, *qmp_path;
    ColodContext ctx_struct = { 0 };
    ColodContext *ctx = &ctx_struct;
    int ret;

    if (argc != 5) {
        fprintf(stderr, "Usage: %s <node name> <instance name> "
                        "<base directory> <qmp unix socket>\n",
                        argv[0]);
        exit(EXIT_FAILURE);
    }

    node_name = argv[1];
    instance_name = argv[2];
    base_dir = argv[3];
    qmp_path = argv[4];

    ctx->daemonize = TRUE;
    do_syslog = FALSE;
    ctx->node_name = node_name;
    ctx->instance_name = instance_name;
    ctx->base_dir = base_dir;
    ctx->qmp_path = qmp_path;

    int pipefd = colod_daemonize(ctx);
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    signal(SIGPIPE, SIG_IGN); // TODO: Handle this properly

    ret = colod_open_qmp(ctx, &errp);
    if (ret < 0) {
        goto err;
    }

    ret = colod_open_mngmt(ctx, &errp);
    if (ret < 0) {
        goto err;
    }

    if (!ctx->disable_cpg) {
        ret = colod_open_cpg(ctx, &errp);
        if (ret < 0) {
            goto err;
        }
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
