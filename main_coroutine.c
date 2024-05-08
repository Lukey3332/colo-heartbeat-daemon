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

typedef enum MainState {
    STATE_SECONDARY_STARTUP,
    STATE_SECONDARY_WAIT,
    STATE_SECONDARY_COLO_RUNNING,
    STATE_PRIMARY_STARTUP,
    STATE_PRIMARY_WAIT,
    STATE_PRIMARY_START_MIGRATION,
    STATE_PRIMARY_COLO_RUNNING,
    STATE_FAILOVER_SYNC,
    STATE_FAILOVER,
    STATE_FAILED_PEER_FAILOVER,
    STATE_FAILED,
    STATE_QUIT,
    STATE_AUTOQUIT
} MainState;

struct ColodMainCoroutine {
    Coroutine coroutine;
    gboolean quit;
    const ColodContext *ctx;
    ColodQueue events, critical_events;
    guint wake_source_id;
    ColodQmpState *qmp;
    Coroutine *raise_timeout_coroutine;

    gboolean pending_action, transitioning;
    gboolean failed, yellow, qemu_quit;
    gboolean primary;
    gboolean replication, peer_failover, peer_failed;

    ColodMainCoroutine *unique_ptr_for_hup_source;
    guint hup_source;
};

#define colod_trace_source(data) \
    _colod_trace_source((data), __func__, __LINE__)
static void _colod_trace_source(gpointer data, const gchar *func,
                                int line) {
    GMainContext *mainctx = g_main_context_default();
    GSource *found = g_main_context_find_source_by_user_data(mainctx, data);
    const gchar *found_name = colod_source_name_or_null(found);

    GSource *current = g_main_current_source();
    const gchar *current_name = colod_source_name_or_null(current);

    colod_trace("%s:%u: found source \"%s\", current source \"%s\"\n",
                func, line, found_name, current_name);
}

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

static void event_wake_source_destroy_cb(gpointer data) {
    ColodMainCoroutine *this = data;

    this->wake_source_id = 0;
}

#define colod_event_queue(ctx, event, reason) \
    _colod_event_queue((ctx), (event), (reason), __func__, __LINE__)
static void _colod_event_queue(ColodMainCoroutine *this, ColodEvent event,
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

    if (queue_empty(queue) && !this->wake_source_id) {
        colod_trace("%s:%u: Waking main coroutine\n", __func__, __LINE__);
        this->wake_source_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                                               this->coroutine.cb.plain, this,
                                               event_wake_source_destroy_cb);
        g_source_set_name_by_id(this->wake_source_id, "wake for event");
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

    guint source_id = g_source_get_id(g_main_current_source());
    if (source_id == this->wake_source_id) {
        this->wake_source_id = 0;
    }

    if (!colod_event_pending(this) || this->wake_source_id) {
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
    ColodMainCoroutine *mainco;
} ColodRaiseCoroutine;

static gboolean _colod_raise_timeout_co(Coroutine *coroutine,
                                        ColodMainCoroutine *mainco);
static gboolean colod_raise_timeout_co(gpointer data) {
    ColodRaiseCoroutine *raiseco = data;
    Coroutine *coroutine = &raiseco->coroutine;
    ColodMainCoroutine *mainco = raiseco->mainco;
    gboolean ret;

    co_enter(coroutine, ret = _colod_raise_timeout_co(coroutine, mainco));
    if (coroutine->yield) {
        return GPOINTER_TO_INT(coroutine->yield_value);
    }

    qmp_set_timeout(mainco->qmp, mainco->ctx->qmp_timeout_low);

    colod_assert_remove_one_source(coroutine);
    g_free(mainco->raise_timeout_coroutine);
    mainco->raise_timeout_coroutine = NULL;
    return ret;
}

static gboolean colod_raise_timeout_co_wrap(
        G_GNUC_UNUSED GIOChannel *channel,
        G_GNUC_UNUSED GIOCondition revents,
        gpointer data) {
    return colod_raise_timeout_co(data);
}

static gboolean _colod_raise_timeout_co(Coroutine *coroutine,
                                        ColodMainCoroutine *mainco) {
    int ret;

    co_begin(gboolean, G_SOURCE_CONTINUE);

    co_recurse(ret = qmp_wait_event_co(coroutine, mainco->qmp, 0,
                                       "{'event': 'STOP'}", NULL));
    if (ret < 0) {
        return G_SOURCE_REMOVE;
    }

    co_recurse(ret = qmp_wait_event_co(coroutine, mainco->qmp, 0,
                                       "{'event': 'RESUME'}", NULL));
    if (ret < 0) {
        return G_SOURCE_REMOVE;
    }

    co_end;

    return G_SOURCE_REMOVE;
}

static void colod_raise_timeout_coroutine_free(ColodMainCoroutine *mainco) {
    if (!mainco->raise_timeout_coroutine) {
        return;
    }

    g_idle_add(colod_raise_timeout_co, mainco->raise_timeout_coroutine);

    while (mainco->raise_timeout_coroutine) {
        g_main_context_iteration(g_main_context_default(), TRUE);
    }
}

static void colod_raise_timeout_coroutine(ColodMainCoroutine *mainco) {
    ColodRaiseCoroutine *raiseco;
    Coroutine *coroutine;

    if (mainco->raise_timeout_coroutine) {
        return;
    }

    qmp_set_timeout(mainco->qmp, mainco->ctx->qmp_timeout_high);

    raiseco = g_new0(ColodRaiseCoroutine, 1);
    coroutine = &raiseco->coroutine;
    coroutine->cb.plain = colod_raise_timeout_co;
    coroutine->cb.iofunc = colod_raise_timeout_co_wrap;
    raiseco->mainco = mainco;
    mainco->raise_timeout_coroutine = coroutine;

    g_idle_add(colod_raise_timeout_co, raiseco);
    return;
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
static MainState _colod_failover_co(Coroutine *coroutine,
                                    ColodMainCoroutine *this) {
    struct {
        JsonNode *commands;
    } *co;
    int ret;
    GError *local_errp = NULL;

    co_frame(co, sizeof(*co));
    co_begin(MainState, STATE_FAILED);

    co_recurse(ret = qmp_yank_co(coroutine, this->qmp, &local_errp));
    if (ret < 0) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return STATE_FAILED;
    }

    if (this->primary) {
        CO commands = this->ctx->commands->failover_primary;
    } else {
        CO commands = this->ctx->commands->failover_secondary;
    }
    this->transitioning = TRUE;
    co_recurse(ret = colod_execute_array_co(coroutine, this, CO commands,
                                            TRUE, &local_errp));
    this->transitioning = FALSE;
    if (ret < 0) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return STATE_FAILED;
    }

    co_end;

    return STATE_PRIMARY_WAIT;
}

#define colod_failover_sync_co(...) \
    co_wrap(_colod_failover_sync_co(__VA_ARGS__))
static MainState _colod_failover_sync_co(Coroutine *coroutine,
                                         ColodMainCoroutine *this) {

    co_begin(MainState, STATE_FAILED);

    colod_cpg_send(this->ctx->cpg, MESSAGE_FAILOVER);

    while (TRUE) {
        ColodEvent event;
        co_recurse(event = colod_event_wait(coroutine, this));
        if (event == EVENT_FAILOVER_WIN) {
            return STATE_FAILOVER;
        } else if (event == EVENT_PEER_FAILED) {
            return STATE_FAILOVER;
        } else if (event_critical(event) && event_escalate(event)) {
            assert(event != EVENT_NONE);
            if (event_failed(event)) {
                if (event == EVENT_PEER_FAILOVER) {
                    return STATE_FAILED_PEER_FAILOVER;
                } else {
                    return STATE_FAILED;
                }
            } else if (event == EVENT_QUIT) {
                return STATE_QUIT;
            } else if (event == EVENT_AUTOQUIT) {
                return STATE_AUTOQUIT;
            } else {
                abort();
            }
        }
    }

    co_end;

    return STATE_FAILED;
}

#define colod_secondary_startup_co(...) \
    co_wrap(_colod_secondary_startup_co(__VA_ARGS__));
static MainState _colod_secondary_startup_co(Coroutine *coroutine,
                                             ColodMainCoroutine *this) {
    GError *local_errp = NULL;
    ColodQmpResult *result;

    result =_colod_execute_co(coroutine, this, &local_errp,
                            "{'execute': 'migrate-set-capabilities',"
                            "'arguments': {'capabilities': ["
                                "{'capability': 'events', 'state': true }]}}\n");
    if (coroutine->yield) {
        return 0;
    }

    if (!result) {
        log_error(local_errp->message);
        g_error_free(local_errp);
        return STATE_FAILED;
    }
    qmp_result_free(result);

    return STATE_SECONDARY_WAIT;
}

#define colod_secondary_wait_co(...) \
    co_wrap(_colod_secondary_wait_co(__VA_ARGS__))
static MainState _colod_secondary_wait_co(Coroutine *coroutine,
                                          ColodMainCoroutine *this) {
    GError *local_errp = NULL;
    int ret;

    while (TRUE) {
        ret = _colod_qmp_event_wait_co(coroutine, this, 0,
                                      "{'event': 'RESUME'}", &local_errp);
        if (coroutine->yield) {
            return 0;
        }

        if (ret < 0) {
            // Interrupted
            g_error_free(local_errp);
            assert(colod_event_pending(this));
            ColodEvent event = _colod_event_wait(coroutine, this, __func__,
                                                 __LINE__);
            assert(!coroutine->yield);

            if (event_critical(event) && event_escalate(event)) {
                if (event_failed(event)) {
                    if (event == EVENT_PEER_FAILOVER) {
                        return STATE_FAILED_PEER_FAILOVER;
                    } else {
                        return STATE_FAILED;
                    }
                } else if (event == EVENT_QUIT) {
                    return STATE_QUIT;
                } else if (event == EVENT_AUTOQUIT) {
                    return STATE_AUTOQUIT;
                } else if (event == EVENT_DID_FAILOVER) {
                    abort();
                    return STATE_PRIMARY_WAIT;
                }
            }
            continue;
        }

        break;
    }

    colod_raise_timeout_coroutine(this);

    return STATE_SECONDARY_COLO_RUNNING;
}

#define colod_colo_running_co(...) \
    co_wrap(_colod_colo_running_co(__VA_ARGS__))
static MainState _colod_colo_running_co(Coroutine *coroutine,
                                        ColodMainCoroutine *this) {

    while (TRUE) {
        ColodEvent event = _colod_event_wait(coroutine, this, __func__, __LINE__);
        if (coroutine->yield) {
            return 0;
        }

        if (event == EVENT_FAILOVER_SYNC) {
            return STATE_FAILOVER_SYNC;
        } else if (event == EVENT_PEER_FAILED) {
            return STATE_FAILOVER;
        } else if (event_critical(event) && event_escalate(event)) {
            assert(event != EVENT_NONE);
            if (event_failed(event)) {
                if (event == EVENT_PEER_FAILOVER) {
                    return STATE_FAILED_PEER_FAILOVER;
                } else {
                    return STATE_FAILED;
                }
            } else if (event == EVENT_QUIT) {
                return STATE_QUIT;
            } else if (event == EVENT_AUTOQUIT) {
                return STATE_AUTOQUIT;
            } else {
                abort();
            }
        }
    }
}

#define colod_primary_wait_co(...) \
    co_wrap(_colod_primary_wait_co(__VA_ARGS__))
static MainState _colod_primary_wait_co(Coroutine *coroutine,
                                        ColodMainCoroutine *this) {
    while (TRUE) {
        ColodEvent event = _colod_event_wait(coroutine, this, __func__, __LINE__);
        if (coroutine->yield) {
            return 0;
        }

        if (event == EVENT_START_MIGRATION) {
            return STATE_PRIMARY_START_MIGRATION;
        } else if (event_failed(event)) {
            if (event != EVENT_PEER_FAILOVER) {
                return STATE_FAILED;
            }
        } else if (event == EVENT_QUIT) {
            return STATE_QUIT;
        } else if (event == EVENT_AUTOQUIT) {
            return STATE_AUTOQUIT;
        }
    }
}

#define colod_primary_start_migration_co(...) \
    co_wrap(_colod_primary_start_migration_co(__VA_ARGS__))
static MainState _colod_primary_start_migration_co(Coroutine *coroutine,
                                                   ColodMainCoroutine *this) {
    struct {
        ColodEvent event;
    } *co;
    ColodQmpState *qmp = this->qmp;
    ColodQmpResult *qmp_result;
    int ret;
    GError *local_errp = NULL;

    co_frame(co, sizeof(*co));
    co_begin(MainState, STATE_FAILED);
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
                                            this->ctx->commands->migration,
                                            FALSE, &local_errp));
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_QMP)) {
        goto qmp_error;
    } else if (ret < 0) {
        goto qemu_failed;
    }
    if (colod_critical_pending(this)) {
        goto handle_event;
    }

    colod_raise_timeout_coroutine(this);

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

    return STATE_PRIMARY_COLO_RUNNING;

qmp_error:
    if (g_error_matches(local_errp, COLOD_ERROR, COLOD_ERROR_INTERRUPT)) {
        g_error_free(local_errp);
        local_errp = NULL;
        assert(colod_critical_pending(this));
        co_recurse(CO event = colod_event_wait(coroutine, this));
        if (event_failover(CO event)) {
            goto failover;
        } else {
            goto misc_event;
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
    return STATE_FAILED;

handle_event:
    assert(colod_critical_pending(this));
    co_recurse(CO event = colod_event_wait(coroutine, this));
    if (event_failover(CO event)) {
        goto failover;
    } else {
        goto misc_event;
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
        return STATE_FAILOVER_SYNC;
    } else {
        return STATE_FAILOVER;
    }

misc_event:
    assert(event_escalate(CO event));
    if (event_failed(CO event)) {
        if (CO event == EVENT_PEER_FAILOVER) {
            return STATE_FAILED_PEER_FAILOVER;
        } else {
            return STATE_FAILED;
        }
    } else if (CO event == EVENT_QUIT) {
        return STATE_QUIT;
    } else if (CO event == EVENT_AUTOQUIT) {
        return STATE_AUTOQUIT;
    } else if (CO event == EVENT_DID_FAILOVER) {
        return STATE_PRIMARY_WAIT;
    }

    return STATE_PRIMARY_COLO_RUNNING;

    co_end;

    return STATE_FAILED;
}

void colod_quit(ColodMainCoroutine *this) {
    g_main_loop_quit(this->ctx->mainloop);
}

static void do_autoquit(ColodMainCoroutine *this) {
    g_main_loop_quit(this->ctx->mainloop);
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

    colod_assert_remove_one_source(this);
    this->quit = TRUE;
    return ret;
}

static gboolean colod_main_co_wrap(
        G_GNUC_UNUSED GIOChannel *channel,
        G_GNUC_UNUSED GIOCondition revents,
        gpointer data) {
    return colod_main_co(data);
}

static gboolean _colod_main_co(Coroutine *coroutine, ColodMainCoroutine *this) {
    struct {
        MainState state;
    } *co;
    int ret;
    GError *local_errp = NULL;

    co_frame(co, sizeof(*co));
    co_begin(gboolean, G_SOURCE_CONTINUE);

    if (this->primary) {
        colod_syslog(LOG_INFO, "starting in primary mode");
        CO state = STATE_PRIMARY_STARTUP;
    } else {
        colod_syslog(LOG_INFO, "starting in secondary mode");
        CO state = STATE_SECONDARY_STARTUP;
    }

    while (TRUE) {
        if (CO state == STATE_SECONDARY_STARTUP) {
            co_recurse(CO state = colod_secondary_startup_co(coroutine,
                                                                this));
        } else if (CO state == STATE_SECONDARY_WAIT) {
            co_recurse(CO state = colod_secondary_wait_co(coroutine, this));
        } else if (CO state == STATE_SECONDARY_COLO_RUNNING) {
            this->replication = TRUE;
            co_recurse(CO state = colod_colo_running_co(coroutine, this));
            this->replication = FALSE;
        } else if (CO state == STATE_PRIMARY_STARTUP) {
            CO state = STATE_PRIMARY_WAIT;
        } else if (CO state == STATE_PRIMARY_WAIT) {
            // Now running primary standalone
            this->primary = TRUE;
            this->replication = FALSE;

            co_recurse(CO state = colod_primary_wait_co(coroutine, this));
        } else if (CO state == STATE_PRIMARY_START_MIGRATION) {
            co_recurse(CO state = colod_primary_start_migration_co(coroutine,
                                                                      this));
        } else if (CO state == STATE_PRIMARY_COLO_RUNNING) {
            this->replication = TRUE;
            co_recurse(CO state = colod_colo_running_co(coroutine, this));
            this->replication = FALSE;
        } else if (CO state == STATE_FAILOVER_SYNC) {
            co_recurse(CO state = colod_failover_sync_co(coroutine, this));
        } else if (CO state == STATE_FAILOVER) {
            co_recurse(CO state = colod_failover_co(coroutine, this));
        } else if (CO state == STATE_FAILED_PEER_FAILOVER) {
            this->peer_failover = TRUE;
            CO state = STATE_FAILED;
        } else if (CO state == STATE_FAILED) {
            this->failed = TRUE;
            colod_cpg_send(this->ctx->cpg, MESSAGE_FAILED);

            qmp_set_timeout(this->qmp, this->ctx->qmp_timeout_low);
            ret = qmp_get_error(this->qmp, &local_errp);
            if (ret < 0) {
                log_error_fmt("qemu failed: %s", local_errp->message);
                g_error_free(local_errp);
                local_errp = NULL;
            }

            co_recurse(ret = colod_stop_co(coroutine, this, &local_errp));
            if (ret < 0) {
                g_error_free(local_errp);
                local_errp = NULL;
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
                        CO state = STATE_AUTOQUIT;
                        break;
                    }
                }
            }
        } else if (CO state == STATE_QUIT) {
            return G_SOURCE_REMOVE;
        } else if (CO state == STATE_AUTOQUIT) {
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
        }
    }

    co_end;

    return G_SOURCE_REMOVE;
}

static gboolean colod_hup_cb(G_GNUC_UNUSED GIOChannel *channel,
                             G_GNUC_UNUSED GIOCondition revents,
                             gpointer data) {
    ColodMainCoroutine **unique_ptr = data;
    ColodMainCoroutine *this = *unique_ptr;

    log_error("qemu quit");
    this->qemu_quit = TRUE;
    colod_event_queue(this, EVENT_QEMU_QUIT, "qmp hup");

    this->hup_source = 0;
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
        colod_raise_timeout_coroutine(this);
    }
}

static void colod_cpg_event_cb(gpointer data, ColodMessage message,
                               gboolean message_from_this_node,
                               gboolean peer_left_group) {
    ColodMainCoroutine *this = data;

    if (peer_left_group) {
        log_error("Peer failed");
        colod_peer_failed(this);
        colod_event_queue(this, EVENT_PEER_FAILED, "peer left cpg group");
    } else if (message == MESSAGE_FAILOVER) {
        if (message_from_this_node) {
            colod_event_queue(this, EVENT_FAILOVER_WIN, "Got our failover msg");
        } else {
            colod_event_queue(this, EVENT_PEER_FAILOVER, "Got peer failover msg");
        }
    } else if (message == MESSAGE_FAILED) {
        if (!message_from_this_node) {
            log_error("Peer failed");
            colod_peer_failed(this);
            colod_event_queue(this, EVENT_PEER_FAILED, "got MESSAGE_FAILED");
        }
    }
}

ColodMainCoroutine *colod_main_new(const ColodContext *ctx) {
    ColodMainCoroutine *this;
    Coroutine *coroutine;

    assert(!ctx->main_coroutine);

    this = g_new0(ColodMainCoroutine, 1);
    coroutine = &this->coroutine;
    coroutine->cb.plain = colod_main_co;
    coroutine->cb.iofunc = colod_main_co_wrap;
    this->ctx = ctx;
    this->qmp = ctx->qmp;

    this->primary = ctx->primary_startup;
    qmp_add_notify_event(this->qmp, colod_qmp_event_cb, this);

    this->unique_ptr_for_hup_source = this;
    this->hup_source = qmp_hup_source(this->qmp, colod_hup_cb,
                                      &this->unique_ptr_for_hup_source);

    colod_cpg_add_notify(ctx->cpg, colod_cpg_event_cb, this);

    g_idle_add(colod_main_co, this);
    return this;
}

void colod_main_free(ColodMainCoroutine *this) {
    colod_event_queue(this, EVENT_QUIT, "teardown");

    colod_cpg_del_notify(this->ctx->cpg, colod_cpg_event_cb, this);
    if (this->hup_source) {
        g_source_remove(this->hup_source);
    }
    qmp_del_notify_event(this->qmp, colod_qmp_event_cb, this);
    colod_raise_timeout_coroutine_free(this);

    while (!this->quit) {
        g_main_context_iteration(g_main_context_default(), TRUE);
    }

    g_free(this);
}
