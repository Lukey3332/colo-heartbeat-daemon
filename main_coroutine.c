/*
 * COLO background daemon
 *
 * Copyright (c) Lukas Straub <lukasstraub2@web.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdlib.h>

#include <glib-2.0/glib.h>

#include "main_coroutine.h"
#include "daemon.h"
#include "coroutine.h"
#include "coroutine_stack.h"
#include "client.h"
#include "watchdog.h"
#include "cpg.h"
#include "json_util.h"

struct ColodMainCoroutine {
    Coroutine coroutine;
    ColodContext *ctx;
    ColodQueue events, critical_events;
    ColodQmpState *qmp;

    gboolean pending_action, transitioning;
    gboolean failed, yellow, qemu_quit;
    gboolean primary;
    gboolean replication, peer_failover, peer_failed;
};

void colod_query_status(ColodMainCoroutine *this, ColodState *ret) {
    ret->primary = this->primary;
    ret->replication = this->replication;
    ret->failed = this->failed;
    ret->peer_failover = this->peer_failover;
    ret->peer_failed = this->peer_failed;
}

void colod_peer_failed(ColodMainCoroutine *this) {
    this->peer_failed = TRUE;
}

void colod_clear_peer_status(ColodMainCoroutine *this) {
    this->peer_failed = FALSE;
}

static const gchar *event_str(ColodEvent event) {
    switch (event) {
        case EVENT_NONE: return "EVENT_NONE";
        case EVENT_FAILED: return "EVENT_FAILED";
        case EVENT_QEMU_QUIT: return "EVENT_QEMU_QUIT";
        case EVENT_PEER_FAILOVER: return "EVENT_PEER_FAILOVER";
        case EVENT_FAILOVER_SYNC: return "EVENT_FAILOVER_SYNC";
        case EVENT_PEER_FAILED: return "EVENT_PEER_FAILED";
        case EVENT_FAILOVER_WIN: return "EVENT_FAILOVER_WIN";
        case EVENT_QUIT: return "EVENT_QUIT";
        case EVENT_AUTOQUIT: return "EVENT_AUTOQUIT";
        case EVENT_YELLOW: return "EVENT_YELLOW";
        case EVENT_START_MIGRATION: return "EVENT_START_MIGRATION";
        case EVENT_DID_FAILOVER: return "EVENT_DID_FAILOVER";
    }
    abort();
}

static gboolean event_escalate(ColodEvent event) {
    switch (event) {
        case EVENT_NONE:
        case EVENT_FAILED:
        case EVENT_QEMU_QUIT:
        case EVENT_PEER_FAILOVER:
        case EVENT_QUIT:
        case EVENT_AUTOQUIT:
        case EVENT_YELLOW:
        case EVENT_START_MIGRATION:
        case EVENT_DID_FAILOVER:
            return TRUE;
        break;

        default:
            return FALSE;
        break;
    }
}

static gboolean event_critical(ColodEvent event) {
    switch (event) {
        case EVENT_NONE:
        case EVENT_FAILOVER_WIN:
        case EVENT_YELLOW:
        case EVENT_START_MIGRATION:
        case EVENT_DID_FAILOVER:
            return FALSE;
        break;

        default:
            return TRUE;
        break;
    }
}

static gboolean event_failed(ColodEvent event) {
    switch (event) {
        case EVENT_FAILED:
        case EVENT_QEMU_QUIT:
        case EVENT_PEER_FAILOVER:
            return TRUE;
        break;

        default:
            return FALSE;
        break;
    }
}

static gboolean event_failover(ColodEvent event) {
    return event == EVENT_FAILOVER_SYNC || event == EVENT_PEER_FAILED;
}

static gboolean colod_event_pending(ColodMainCoroutine *this) {
    return !queue_empty(&this->events) || !queue_empty(&this->critical_events);
}

#define colod_event_queue(ctx, event, reason) \
    _colod_event_queue((ctx), (event), (reason), __func__, __LINE__)
void _colod_event_queue(ColodMainCoroutine *this, ColodEvent event,
                        const gchar *reason, const gchar *func,
                        int line) {
    ColodQueue *queue;

    colod_trace("%s:%u: queued %s (%s)\n", func, line, event_str(event),
                reason);

    if (event_critical(event)) {
        queue = &this->critical_events;
    } else {
        queue = &this->events;
    }

    if (queue_empty(queue)) {
        colod_trace("%s:%u: Waking main coroutine\n", __func__, __LINE__);
        g_idle_add(this->coroutine.cb.plain, this);
    }

    if (!queue_empty(queue)) {
        // ratelimit
        if (queue_peek(queue) == event) {
            colod_trace("%s:%u: Ratelimiting events\n", __func__, __LINE__);
            return;
        }
    }

    queue_add(queue, event);
    assert(colod_event_pending(this));
}

#define colod_event_wait(coroutine, ctx) \
    co_wrap(_colod_event_wait(coroutine, ctx, __func__, __LINE__))
static ColodEvent _colod_event_wait(Coroutine *coroutine,
                                    ColodMainCoroutine *this,
                                    const gchar *func, int line) {
    ColodQueue *queue = &this->events;

    if (!colod_event_pending(this)) {
        coroutine->yield = TRUE;
        coroutine->yield_value = GINT_TO_POINTER(G_SOURCE_REMOVE);
        return EVENT_FAILED;
    }

    if (!queue_empty(&this->critical_events)) {
        queue = &this->critical_events;
    }

    ColodEvent event = queue_remove(queue);
    colod_trace("%s:%u: got %s\n", func, line, event_str(event));
    return event;
}

static gboolean colod_critical_pending(ColodMainCoroutine *this) {
    return !queue_empty(&this->critical_events);
}

#define colod_qmp_event_wait_co(...) \
    co_wrap(_colod_qmp_event_wait_co(__VA_ARGS__))
static int _colod_qmp_event_wait_co(Coroutine *coroutine,
                                    ColodMainCoroutine *this,
                                    guint timeout, const gchar* match,
                                    GError **errp) {
    int ret;
    GError *local_errp = NULL;

    while (TRUE) {
        ret = _qmp_wait_event_co(coroutine, this->qmp, timeout, match,
                                 &local_errp);
        if (coroutine->yield) {
            return -1;
        }
        if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_INTERRUPT)) {
            assert(colod_event_pending(this));
            if (!colod_critical_pending(this)) {
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

int _colod_yank_co(Coroutine *coroutine, ColodMainCoroutine *this, GError **errp) {
    int ret;
    GError *local_errp = NULL;

    ret = _qmp_yank_co(coroutine, this->qmp, &local_errp);
    if (coroutine->yield) {
        return -1;
    }
    if (ret < 0) {
        colod_event_queue(this, EVENT_FAILED, local_errp->message);
        g_propagate_error(errp, local_errp);
    } else {
        qmp_clear_yank(this->qmp);
        colod_event_queue(this, EVENT_FAILOVER_SYNC, "did yank");
    }

    return ret;
}

ColodQmpResult *_colod_execute_nocheck_co(Coroutine *coroutine,
                                          ColodMainCoroutine *this,
                                          GError **errp,
                                          const gchar *command) {
    ColodQmpResult *result;
    int ret;
    GError *local_errp = NULL;

    colod_watchdog_refresh(this->ctx->watchdog);

    result = _qmp_execute_nocheck_co(coroutine, this->qmp, &local_errp, command);
    if (coroutine->yield) {
        return NULL;
    }
    if (!result) {
        colod_event_queue(this, EVENT_FAILED, local_errp->message);
        g_propagate_error(errp, local_errp);
        return NULL;
    }

    ret = qmp_get_error(this->qmp, &local_errp);
    if (ret < 0) {
        qmp_result_free(result);
        colod_event_queue(this, EVENT_FAILED, local_errp->message);
        g_propagate_error(errp, local_errp);
        return NULL;
    }

    if (qmp_get_yank(this->qmp)) {
        qmp_clear_yank(this->qmp);
        colod_event_queue(this, EVENT_FAILOVER_SYNC, "did yank");
    }

    return result;
}

ColodQmpResult *_colod_execute_co(Coroutine *coroutine,
                                  ColodMainCoroutine *this,
                                  GError **errp,
                                  const gchar *command) {
    ColodQmpResult *result;

    result = _colod_execute_nocheck_co(coroutine, this, errp, command);
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


#define colod_execute_array_co(...) \
    co_wrap(_colod_execute_array_co(__VA_ARGS__))
static int _colod_execute_array_co(Coroutine *coroutine, ColodMainCoroutine *this,
                                   JsonNode *array_node, gboolean ignore_errors,
                                   GError **errp) {
    struct {
        JsonArray *array;
        gchar *line;
        guint i, count;
    } *co;
    int ret = 0;
    GError *local_errp = NULL;

    co_frame(co, sizeof(*co));
    co_begin(int, -1);

    assert(!errp || !*errp);
    assert(JSON_NODE_HOLDS_ARRAY(array_node));

    CO array = json_node_get_array(array_node);
    CO count = json_array_get_length(CO array);
    for (CO i = 0; CO i < CO count; CO i++) {
        JsonNode *node = json_array_get_element(CO array, CO i);
        assert(node);

        gchar *tmp = json_to_string(node, FALSE);
        CO line = g_strdup_printf("%s\n", tmp);
        g_free(tmp);

        ColodQmpResult *result;
        co_recurse(result = colod_execute_co(coroutine, this, &local_errp, CO line));
        if (ignore_errors &&
                g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
            colod_syslog(LOG_WARNING, "Ignoring qmp error: %s",
                         local_errp->message);
            g_error_free(local_errp);
            local_errp = NULL;
        } else if (!result) {
            g_propagate_error(errp, local_errp);
            g_free(CO line);
            ret = -1;
            break;
        }
        qmp_result_free(result);
    }

    co_end;

    return ret;
}

static gboolean qemu_runnng(const gchar *status) {
    return !strcmp(status, "running")
            || !strcmp(status, "finish-migrate")
            || !strcmp(status, "colo")
            || !strcmp(status, "prelaunch")
            || !strcmp(status, "paused");
}

#define qemu_query_status_co(...) \
    co_wrap(_qemu_query_status_co(__VA_ARGS__))
static int _qemu_query_status_co(Coroutine *coroutine, ColodMainCoroutine *this,
                                 gboolean *primary, gboolean *replication,
                                 GError **errp) {
    struct {
        ColodQmpResult *qemu_status, *colo_status;
    } *co;

    co_frame(co, sizeof(*co));
    co_begin(int, -1);

    co_recurse(CO qemu_status = colod_execute_co(coroutine, this, errp,
                                                 "{'execute': 'query-status'}\n"));
    if (!CO qemu_status) {
        return -1;
    }

    co_recurse(CO colo_status = colod_execute_co(coroutine, this, errp,
                                                 "{'execute': 'query-colo-status'}\n"));
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
        *primary = FALSE;
        *replication = FALSE;
    } else if (qemu_runnng(status) && !strcmp(colo_mode, "none")
               && (!strcmp(colo_reason, "none")
                   || !strcmp(colo_reason, "request"))) {
        *primary = TRUE;
        *replication = FALSE;
    } else if (qemu_runnng(status) &&!strcmp(colo_mode, "primary")) {
        *primary = TRUE;
        *replication = TRUE;
    } else if (qemu_runnng(status) && !strcmp(colo_mode, "secondary")) {
        *primary = FALSE;
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

int _colod_check_health_co(Coroutine *coroutine, ColodMainCoroutine *this,
                           GError **errp) {
    gboolean primary;
    gboolean replication;
    int ret;
    GError *local_errp = NULL;

    ret = _qemu_query_status_co(coroutine, this, &primary, &replication,
                                &local_errp);
    if (coroutine->yield) {
        return -1;
    }
    if (ret < 0) {
        colod_event_queue(this, EVENT_FAILED, local_errp->message);
        g_propagate_error(errp, local_errp);
        return -1;
    }

    if (!this->transitioning &&
            (this->primary != primary ||
             this->replication != replication)) {
        colod_error_set(&local_errp, "qemu status mismatch: (%s, %s)"
                        " Expected: (%s, %s)",
                        bool_to_json(primary), bool_to_json(replication),
                        bool_to_json(this->primary),
                        bool_to_json(this->replication));
        colod_event_queue(this, EVENT_FAILED, local_errp->message);
        g_propagate_error(errp, local_errp);
        return -1;
    }

    return 0;
}

int colod_start_migration(ColodMainCoroutine *this) {
    if (this->pending_action || this->replication) {
        return -1;
    }

    colod_event_queue(this, EVENT_START_MIGRATION, "client request");
    return 0;
}

void colod_autoquit(ColodMainCoroutine *this) {
    colod_event_queue(this, EVENT_AUTOQUIT, "client request");
}

void colod_qemu_failed(ColodMainCoroutine *this) {
    colod_event_queue(this, EVENT_FAILED, "?");
}

typedef struct ColodRaiseCoroutine {
    Coroutine coroutine;
    ColodContext *ctx;
} ColodRaiseCoroutine;

static gboolean _colod_raise_timeout_co(Coroutine *coroutine,
                                        ColodContext *ctx);
static gboolean colod_raise_timeout_co(gpointer data) {
    ColodRaiseCoroutine *raiseco = data;
    Coroutine *coroutine = &raiseco->coroutine;
    ColodContext *ctx = raiseco->ctx;
    gboolean ret;

    co_enter(coroutine, ret = _colod_raise_timeout_co(coroutine, ctx));
    if (coroutine->yield) {
        return GPOINTER_TO_INT(coroutine->yield_value);
    }

    qmp_set_timeout(ctx->qmp, ctx->qmp_timeout_low);

    g_source_remove_by_user_data(coroutine);
    assert(!g_source_remove_by_user_data(coroutine));
    g_free(ctx->raise_timeout_coroutine);
    ctx->raise_timeout_coroutine = NULL;
    return ret;
}

static gboolean colod_raise_timeout_co_wrap(
        G_GNUC_UNUSED GIOChannel *channel,
        G_GNUC_UNUSED GIOCondition revents,
        gpointer data) {
    return colod_raise_timeout_co(data);
}

static gboolean _colod_raise_timeout_co(Coroutine *coroutine,
                                        ColodContext *ctx) {
    int ret;

    co_begin(gboolean, G_SOURCE_CONTINUE);

    co_recurse(ret = qmp_wait_event_co(coroutine, ctx->qmp, 0,
                                       "{'event': 'STOP'}", NULL));
    if (ret < 0) {
        return G_SOURCE_REMOVE;
    }

    co_recurse(ret = qmp_wait_event_co(coroutine, ctx->qmp, 0,
                                       "{'event': 'RESUME'}", NULL));
    if (ret < 0) {
        return G_SOURCE_REMOVE;
    }

    co_end;

    return G_SOURCE_REMOVE;
}

void colod_raise_timeout_coroutine_free(ColodContext *ctx) {
    if (!ctx->raise_timeout_coroutine) {
        return;
    }

    g_idle_add(colod_raise_timeout_co, ctx->raise_timeout_coroutine);

    while (ctx->raise_timeout_coroutine) {
        g_main_context_iteration(g_main_context_default(), TRUE);
    }
}

Coroutine *colod_raise_timeout_coroutine(ColodContext *ctx) {
    ColodRaiseCoroutine *raiseco;
    Coroutine *coroutine;

    if (ctx->raise_timeout_coroutine) {
        return NULL;
    }

    qmp_set_timeout(ctx->qmp, ctx->qmp_timeout_high);

    raiseco = g_new0(ColodRaiseCoroutine, 1);
    coroutine = &raiseco->coroutine;
    coroutine->cb.plain = colod_raise_timeout_co;
    coroutine->cb.iofunc = colod_raise_timeout_co_wrap;
    raiseco->ctx = ctx;
    ctx->raise_timeout_coroutine = coroutine;

    g_idle_add(colod_raise_timeout_co, raiseco);
    return coroutine;
}

#define colod_stop_co(...) \
    co_wrap(_colod_stop_co(__VA_ARGS__))
static int _colod_stop_co(Coroutine *coroutine, ColodMainCoroutine *this,
                          GError **errp) {
    ColodQmpResult *result;

    result = _colod_execute_co(coroutine, this, errp, "{'execute': 'stop'}\n");
    if (coroutine->yield) {
        return -1;
    }
    if (!result) {
        return -1;
    }
    qmp_result_free(result);

    return 0;
}

#define colod_failover_co(...) \
    co_wrap(_colod_failover_co(__VA_ARGS__))
static ColodEvent _colod_failover_co(Coroutine *coroutine, ColodMainCoroutine *this) {
    struct {
        JsonNode *commands;
    } *co;
    int ret;
    GError *local_errp = NULL;

    co_frame(co, sizeof(*co));
    co_begin(ColodEvent, EVENT_FAILED);

    co_recurse(ret = qmp_yank_co(coroutine, this->qmp, &local_errp));
    if (ret < 0) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return EVENT_FAILED;
    }

    if (this->primary) {
        CO commands = this->ctx->failover_primary_commands;
    } else {
        CO commands = this->ctx->failover_secondary_commands;
    }
    this->transitioning = TRUE;
    co_recurse(ret = colod_execute_array_co(coroutine, this, CO commands,
                                            TRUE, &local_errp));
    this->transitioning = FALSE;
    if (ret < 0) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return EVENT_FAILED;
    }

    co_end;

    return EVENT_DID_FAILOVER;
}

#define colod_failover_sync_co(...) \
    co_wrap(_colod_failover_sync_co(__VA_ARGS__))
static ColodEvent _colod_failover_sync_co(Coroutine *coroutine,
                                          ColodMainCoroutine *this) {

    co_begin(ColodEvent, EVENT_FAILED);

    colod_cpg_send(this->ctx->cpg, MESSAGE_FAILOVER);

    while (TRUE) {
        ColodEvent event;
        co_recurse(event = colod_event_wait(coroutine, this));
        if (event == EVENT_FAILOVER_WIN) {
            break;
        } else if (event == EVENT_PEER_FAILED) {
            break;
        } else if (event_critical(event) && event_escalate(event)) {
            return event;
        }
    }

    ColodEvent event;
    co_recurse(event = colod_failover_co(coroutine, this));
    return event;

    co_end;

    return EVENT_FAILED;
}

#define colod_start_migration_co(...) \
    co_wrap(_colod_start_migration_co(__VA_ARGS__))
static ColodEvent _colod_start_migration_co(Coroutine *coroutine,
                                            ColodMainCoroutine *this) {
    struct {
        guint event;
    } *co;
    ColodQmpState *qmp = this->qmp;
    ColodQmpResult *qmp_result;
    ColodEvent result;
    int ret;
    GError *local_errp = NULL;

    co_frame(co, sizeof(*co));
    co_begin(ColodEvent, EVENT_FAILED);
    co_recurse(qmp_result = colod_execute_co(coroutine, this, &local_errp,
                    "{'execute': 'migrate-set-capabilities',"
                    "'arguments': {'capabilities': ["
                        "{'capability': 'events', 'state': true },"
                        "{'capability': 'pause-before-switchover', 'state': true}]}}\n"));
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
        goto qmp_error;
    } else if (!qmp_result) {
        goto qemu_failed;
    }
    qmp_result_free(qmp_result);
    if (colod_critical_pending(this)) {
        goto handle_event;
    }

    co_recurse(ret = colod_qmp_event_wait_co(coroutine, this, 5*60*1000,
                    "{'event': 'MIGRATION',"
                    " 'data': {'status': 'pre-switchover'}}",
                    &local_errp));
    if (ret < 0) {
        goto qmp_error;
    }

    co_recurse(ret = colod_execute_array_co(coroutine, this,
                                            this->ctx->migration_commands,
                                            FALSE, &local_errp));
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
        goto qmp_error;
    } else if (ret < 0) {
        goto qemu_failed;
    }
    if (colod_critical_pending(this)) {
        goto handle_event;
    }

    colod_raise_timeout_coroutine(this->ctx);

    co_recurse(qmp_result = colod_execute_co(coroutine, this, &local_errp,
                    "{'execute': 'migrate-continue',"
                    "'arguments': {'state': 'pre-switchover'}}\n"));
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
        qmp_set_timeout(qmp, this->ctx->qmp_timeout_low);
        goto qmp_error;
    } else if (!qmp_result) {
        qmp_set_timeout(qmp, this->ctx->qmp_timeout_low);
        goto qemu_failed;
    }
    qmp_result_free(qmp_result);
    if (colod_critical_pending(this)) {
        qmp_set_timeout(qmp, this->ctx->qmp_timeout_low);
        goto handle_event;
    }

    this->transitioning = TRUE;
    co_recurse(ret = colod_qmp_event_wait_co(coroutine, this, 10000,
                    "{'event': 'MIGRATION',"
                    " 'data': {'status': 'colo'}}",
                    &local_errp));
    this->transitioning = FALSE;
    if (ret < 0) {
        qmp_set_timeout(qmp, this->ctx->qmp_timeout_low);
        goto qmp_error;
    }

    return EVENT_NONE;

qmp_error:
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_INTERRUPT)) {
        g_error_free(local_errp);
        local_errp = NULL;
        assert(colod_critical_pending(this));
        co_recurse(CO event = colod_event_wait(coroutine, this));
        if (event_failover(CO event)) {
            goto failover;
        } else {
            return CO event;
        }
    } else {
        log_error(local_errp->message);
        g_error_free(local_errp);
    }
    CO event = EVENT_PEER_FAILED;
    goto failover;

qemu_failed:
    log_error(local_errp->message);
    g_error_free(local_errp);
    return EVENT_FAILED;

handle_event:
    assert(colod_critical_pending(this));
    co_recurse(CO event = colod_event_wait(coroutine, this));
    if (event_failover(CO event)) {
        goto failover;
    } else {
        return CO event;
    }

failover:
    co_recurse(qmp_result = colod_execute_co(coroutine, this, &local_errp,
                    "{'execute': 'migrate_cancel'}\n"));
    if (!qmp_result) {
        goto qemu_failed;
    }
    qmp_result_free(qmp_result);
    assert(event_failover(CO event));
    if (CO event == EVENT_FAILOVER_SYNC) {
        co_recurse(result = colod_failover_sync_co(coroutine, this));
    } else {
        co_recurse(result = colod_failover_co(coroutine, this));
    }
    return result;

    co_end;

    return EVENT_FAILED;
}

#define colod_replication_wait_co(...) \
    co_wrap(_colod_replication_wait_co(__VA_ARGS__))
static ColodEvent _colod_replication_wait_co(Coroutine *coroutine,
                                             ColodMainCoroutine *this) {
    ColodEvent event;
    int ret;
    ColodQmpResult *qmp_result;
    GError *local_errp = NULL;

    co_begin(ColodEvent, EVENT_FAILED);

    co_recurse(qmp_result = colod_execute_co(coroutine, this, &local_errp,
                        "{'execute': 'migrate-set-capabilities',"
                        "'arguments': {'capabilities': ["
                            "{'capability': 'events', 'state': true }]}}\n"));
    if (!qmp_result) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return EVENT_FAILED;
    }
    qmp_result_free(qmp_result);

    while (TRUE) {
        this->transitioning = TRUE;
        co_recurse(ret = colod_qmp_event_wait_co(coroutine, this, 0,
                                      "{'event': 'RESUME'}", &local_errp));
        this->transitioning = FALSE;
        if (ret < 0) {
            g_error_free(local_errp);
            assert(colod_event_pending(this));
            co_recurse(event = colod_event_wait(coroutine, this));
            if (event_critical(event) && event_escalate(event)) {
                return event;
            }
            continue;
        }
        break;
    }

    colod_raise_timeout_coroutine(this->ctx);

    co_end;

    return EVENT_NONE;
}

#define colod_replication_running_co(...) \
    co_wrap(_colod_replication_running_co(__VA_ARGS__))
static ColodEvent _colod_replication_running_co(Coroutine *coroutine,
                                                ColodMainCoroutine *this) {
    co_begin(ColodEvent, EVENT_FAILED);

    while (TRUE) {
        ColodEvent event;
        co_recurse(event = colod_event_wait(coroutine, this));
        if (event == EVENT_FAILOVER_SYNC) {
            co_recurse(event = colod_failover_sync_co(coroutine, this));
            return event;
        } else if (event == EVENT_PEER_FAILED) {
            co_recurse(event = colod_failover_co(coroutine, this));
            return event;
        } else if (event_critical(event) && event_escalate(event)) {
            return event;
        }
    }

    co_end;

    return EVENT_FAILED;
}

void colod_quit(ColodMainCoroutine *this) {
    g_main_loop_quit(this->ctx->mainloop);
}

static void do_autoquit(ColodMainCoroutine *this) {
    client_listener_free(this->ctx->listener);
    exit(EXIT_SUCCESS);
}

static gboolean _colod_main_co(Coroutine *coroutine, ColodMainCoroutine *this);
static gboolean colod_main_co(gpointer data) {
    ColodMainCoroutine *this = data;
    Coroutine *coroutine = data;
    gboolean ret;

    co_enter(coroutine, ret = _colod_main_co(coroutine, this));
    if (coroutine->yield) {
        return GPOINTER_TO_INT(coroutine->yield_value);
    }

    g_source_remove_by_user_data(coroutine);
    assert(!g_source_remove_by_user_data(coroutine));
    this->ctx->main_coroutine = NULL;
    g_free(coroutine);
    return ret;
}

static gboolean colod_main_co_wrap(
        G_GNUC_UNUSED GIOChannel *channel,
        G_GNUC_UNUSED GIOCondition revents,
        gpointer data) {
    return colod_main_co(data);
}

static gboolean _colod_main_co(Coroutine *coroutine, ColodMainCoroutine *this) {
    ColodEvent event = EVENT_NONE;
    int ret;
    GError *local_errp = NULL;

    co_begin(gboolean, G_SOURCE_CONTINUE);

    if (!this->primary) {
        colod_syslog(LOG_INFO, "starting in secondary mode");
        while (TRUE) {
            co_recurse(event = colod_replication_wait_co(coroutine, this));
            assert(event_escalate(event));
            if (event_failed(event)) {
                goto failed;
            } else if (event == EVENT_QUIT) {
                return G_SOURCE_REMOVE;
            } else if (event == EVENT_AUTOQUIT) {
                goto autoquit;
            } else if (event == EVENT_DID_FAILOVER) {
                break;
            }
            this->replication = TRUE;

            co_recurse(event = colod_replication_running_co(coroutine, this));
            assert(event_escalate(event));
            assert(event != EVENT_NONE);
            if (event_failed(event)) {
                goto failed;
            } else if (event == EVENT_QUIT) {
                return G_SOURCE_REMOVE;
            } else if (event == EVENT_AUTOQUIT) {
                goto autoquit;
            } else if (event == EVENT_DID_FAILOVER) {
                break;
            } else {
                abort();
            }
        }
    } else {
        colod_syslog(LOG_INFO, "starting in primary mode");
    }

    // Now running primary standalone
    this->primary = TRUE;
    this->replication = FALSE;

    while (TRUE) {
        co_recurse(event = colod_event_wait(coroutine, this));
        if (event == EVENT_START_MIGRATION) {
            this->pending_action = TRUE;
            co_recurse(event = colod_start_migration_co(coroutine, this));
            assert(event_escalate(event));
            this->pending_action = FALSE;
            if (event_failed(event)) {
                goto failed;
            } else if (event == EVENT_QUIT) {
                return G_SOURCE_REMOVE;
            } else if (event == EVENT_AUTOQUIT) {
                goto autoquit;
            } else if (event == EVENT_DID_FAILOVER) {
                continue;
            }
            this->replication = TRUE;

            co_recurse(event = colod_replication_running_co(coroutine, this));
            assert(event_escalate(event));
            assert(event != EVENT_NONE);
            if (event_failed(event)) {
                 goto failed;
             } else if (event == EVENT_QUIT) {
                 return G_SOURCE_REMOVE;
             } else if (event == EVENT_AUTOQUIT) {
                 goto autoquit;
             } else if (event == EVENT_DID_FAILOVER) {
                this->replication = FALSE;
                continue;
             } else {
                 abort();
             }
        } else if (event_failed(event)) {
            if (event != EVENT_PEER_FAILOVER) {
                goto failed;
            }
        } else if (event == EVENT_QUIT) {
            return G_SOURCE_REMOVE;
        } else if (event == EVENT_AUTOQUIT) {
            goto autoquit;
        }
    }

failed:
    qmp_set_timeout(this->qmp, this->ctx->qmp_timeout_low);
    ret = qmp_get_error(this->qmp, &local_errp);
    if (ret < 0) {
        log_error_fmt("qemu failed: %s", local_errp->message);
        g_error_free(local_errp);
        local_errp = NULL;
    }

    this->failed = TRUE;
    colod_cpg_send(this->ctx->cpg, MESSAGE_FAILED);

    if (event == EVENT_NONE) {
        log_error("Failed with EVENT_NONE");
    }
    if (event == EVENT_PEER_FAILOVER) {
        this->peer_failover = TRUE;
    }
    if (event != EVENT_QEMU_QUIT) {
        co_recurse(ret = colod_stop_co(coroutine, this, &local_errp));
        if (ret < 0) {
            if (event == EVENT_PEER_FAILOVER) {
                log_error_fmt("Failed to stop qemu in response to "
                              "peer failover: %s", local_errp->message);
            }
            g_error_free(local_errp);
            local_errp = NULL;
        }
    }

    while (TRUE) {
        ColodEvent event;
        co_recurse(event = colod_event_wait(coroutine, this));
        if (event == EVENT_PEER_FAILOVER) {
            this->peer_failover = TRUE;
        } else if (event == EVENT_QUIT) {
            return G_SOURCE_REMOVE;
        } else if (event == EVENT_AUTOQUIT) {
            if (this->qemu_quit) {
                do_autoquit(this);
            } else {
                goto autoquit;
            }
        }
    }

autoquit:
    this->failed = TRUE;
    colod_cpg_send(this->ctx->cpg, MESSAGE_FAILED);

    while (TRUE) {
        ColodEvent event;
        co_recurse(event = colod_event_wait(coroutine, this));
        if (event == EVENT_PEER_FAILOVER) {
            this->peer_failover = TRUE;
        } else if (event == EVENT_QUIT) {
            return G_SOURCE_REMOVE;
        } else if (event == EVENT_QEMU_QUIT) {
            do_autoquit(this);
        }
    }

    co_end;

    return G_SOURCE_REMOVE;
}

static gboolean colod_hup_cb(G_GNUC_UNUSED GIOChannel *channel,
                             G_GNUC_UNUSED GIOCondition revents,
                             gpointer data) {
    ColodMainCoroutine *this = data;

    log_error("qemu quit");
    this->qemu_quit = TRUE;
    colod_event_queue(this, EVENT_QEMU_QUIT, "qmp hup");
    return G_SOURCE_REMOVE;
}

static void colod_qmp_event_cb(gpointer data, ColodQmpResult *result) {
    ColodMainCoroutine *this = data;
    const gchar *event;

    event = get_member_str(result->json_root, "event");

    if (!strcmp(event, "QUORUM_REPORT_BAD")) {
        const gchar *node, *type;
        node = get_member_member_str(result->json_root, "data", "node-name");
        type = get_member_member_str(result->json_root, "data", "type");

        if (!strcmp(node, "nbd0")) {
            if (!!strcmp(type, "read")) {
                colod_event_queue(this, EVENT_FAILOVER_SYNC,
                                  "nbd write/flush error");
            }
        } else {
            if (!!strcmp(type, "read")) {
                colod_event_queue(this, EVENT_YELLOW,
                                  "local disk write/flush error");
            }
        }
    } else if (!strcmp(event, "COLO_EXIT")) {
        const gchar *reason;
        reason = get_member_member_str(result->json_root, "data", "reason");

        if (!strcmp(reason, "error")) {
            colod_event_queue(this, EVENT_FAILOVER_SYNC, "COLO_EXIT");
        }
    } else if (!strcmp(event, "RESET")) {
        colod_raise_timeout_coroutine(this->ctx);
    }
}

ColodMainCoroutine *colod_main_coroutine(ColodContext *ctx) {
    ColodMainCoroutine *this;
    Coroutine *coroutine;

    assert(!ctx->main_coroutine);

    this = g_new0(ColodMainCoroutine, 1);
    coroutine = &this->coroutine;
    coroutine->cb.plain = colod_main_co;
    coroutine->cb.iofunc = colod_main_co_wrap;
    this->ctx = ctx;
    this->qmp = ctx->qmp;
    ctx->main_coroutine = this;

    this->primary = ctx->primary_startup;
    qmp_add_notify_event(this->qmp, colod_qmp_event_cb, this);
    qmp_hup_source(this->qmp, colod_hup_cb, this);

    g_idle_add(colod_main_co, this);
    return this;
}

void colod_main_free(ColodMainCoroutine *this) {
    ColodContext *ctx = this->ctx;
    colod_event_queue(this, EVENT_QUIT, "teardown");
    qmp_del_notify_event(this->qmp, colod_qmp_event_cb, this);

    while (ctx->main_coroutine) {
        g_main_context_iteration(g_main_context_default(), TRUE);
    }
}
