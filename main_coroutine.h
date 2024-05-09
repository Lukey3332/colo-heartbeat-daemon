#ifndef MAIN_COROUTINE_H
#define MAIN_COROUTINE_H

#include "daemon.h"

typedef enum ColodEvent ColodEvent;

enum ColodEvent {
    EVENT_NONE = 0,
    EVENT_FAILED,
    EVENT_PEER_FAILOVER,
    EVENT_FAILOVER_SYNC,
    EVENT_PEER_FAILED,
    EVENT_FAILOVER_WIN,
    EVENT_QUIT,
    EVENT_AUTOQUIT,
    EVENT_YELLOW,
    EVENT_START_MIGRATION
};

typedef struct ColodState {
    gboolean primary;
    gboolean replication, failed, peer_failover, peer_failed;
} ColodState;

#define colod_check_health_co(...) \
    co_wrap(_colod_check_health_co(__VA_ARGS__))
int _colod_check_health_co(Coroutine *coroutine, ColodMainCoroutine *this,
                           GError **errp);
void colod_query_status(ColodMainCoroutine *this, ColodState *ret);

void colod_peer_failed(ColodMainCoroutine *this);
void colod_clear_peer_status(ColodMainCoroutine *this);

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

ColodMainCoroutine *colod_main_new(const ColodContext *ctx);
void colod_main_free(ColodMainCoroutine *this);

#endif // MAIN_COROUTINE_H
