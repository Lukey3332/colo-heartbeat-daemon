#ifndef MAIN_COROUTINE_H
#define MAIN_COROUTINE_H

#include "daemon.h"

typedef enum ColodEvent ColodEvent;

enum ColodEvent {
    EVENT_NONE = 0,
    EVENT_FAILED,
    EVENT_QEMU_QUIT,
    EVENT_PEER_FAILOVER,
    EVENT_FAILOVER_SYNC,
    EVENT_PEER_FAILED,
    EVENT_FAILOVER_WIN,
    EVENT_QUIT,
    EVENT_AUTOQUIT,
    EVENT_YELLOW,
    EVENT_START_MIGRATION,
    EVENT_DID_FAILOVER
};

typedef struct ColodState {
    gboolean primary;
    gboolean replication, failed, peer_failover, peer_failed;
} ColodState;

#define colod_event_queue(ctx, event, reason) \
    _colod_event_queue((ctx), (event), (reason), __func__, __LINE__)
void _colod_event_queue(ColodMainCoroutine *ctx, ColodEvent event,
                        const gchar *reason, const gchar *func,
                        int line);

#define colod_check_health_co(...) \
    co_wrap(_colod_check_health_co(__VA_ARGS__))
int _colod_check_health_co(Coroutine *coroutine, ColodMainCoroutine *this,
                           GError **errp);
void colod_query_status(ColodMainCoroutine *this, ColodState *ret);

void colod_peer_failed(ColodMainCoroutine *this);
void colod_clear_peer_status(ColodMainCoroutine *this);

void colod_set_migration_commands(ColodContext *ctx, JsonNode *commands);
void colod_set_primary_commands(ColodContext *ctx, JsonNode *commands);
void colod_set_secondary_commands(ColodContext *ctx, JsonNode *commands);

int colod_start_migration(ColodMainCoroutine *this);
void colod_autoquit(ColodMainCoroutine *this);
void colod_quit(ColodMainCoroutine *this);
void colod_qemu_failed(ColodMainCoroutine *this);

#define colod_yank(...) \
    co_wrap(_colod_yank_co(__VA_ARGS__))
int _colod_yank_co(Coroutine *coroutine, ColodMainCoroutine *this, GError **errp);

#define colod_execute_nocheck_co(...) \
    co_wrap(_colod_execute_nocheck_co(__VA_ARGS__))
ColodQmpResult *_colod_execute_nocheck_co(Coroutine *coroutine,
                                          ColodMainCoroutine *this,
                                          GError **errp,
                                          const gchar *command);

#define colod_execute_co(...) \
    co_wrap(_colod_execute_co(__VA_ARGS__))
ColodQmpResult *_colod_execute_co(Coroutine *coroutine,
                                  ColodMainCoroutine *this,
                                  GError **errp,
                                  const gchar *command);


void colod_raise_timeout_coroutine_free(ColodContext *ctx);
Coroutine *colod_raise_timeout_coroutine(ColodContext *ctx);

ColodMainCoroutine *colod_main_coroutine(ColodContext *ctx);
void colod_main_free(ColodMainCoroutine *this);

#endif // MAIN_COROUTINE_H
