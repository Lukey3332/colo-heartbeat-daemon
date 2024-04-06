
#include <glib-2.0/glib.h>

#include "base_types.h"
#include "smoketest.h"
#include "coroutine_stack.h"

struct SmokeTestcase {
    Coroutine coroutine;
    SmokeContext *ctx;
    gboolean do_quit, quit;
};

static gboolean _testcase_co(Coroutine *coroutine, SmokeTestcase *this) {
    GError *local_errp = NULL;
    GIOStatus ret;
    SmokeColodContext *sctx = &this->ctx->sctx;
    const gchar *command = "{'exec-colod': 'quit'}\n";
    gchar *line;
    gsize len;

    co_begin(gboolean, G_SOURCE_CONTINUE);

    co_recurse(ret = colod_channel_write_timeout_co(coroutine, sctx->client_ch,
                                                    command, strlen(command),
                                                    1000, &local_errp));
    assert(ret == G_IO_STATUS_NORMAL);

    co_recurse(ret = colod_channel_read_line_timeout_co(coroutine, sctx->client_ch,
                                                        &line, &len, 1000, &local_errp));
    assert(ret == G_IO_STATUS_NORMAL);
    g_free(line);

    assert(!this->do_quit);
    while (!this->do_quit) {
        progress_source_add(coroutine->cb.plain, this);
        co_yield_int(G_SOURCE_REMOVE);
    }
    this->quit = TRUE;
    co_end;

    return G_SOURCE_REMOVE;
}

static gboolean testcase_co(gpointer data) {
    SmokeTestcase *this = data;
    Coroutine *coroutine = data;
    gboolean ret;

    co_enter(coroutine, ret = _testcase_co(coroutine, this));
    if (coroutine->yield) {
        return GPOINTER_TO_INT(coroutine->yield_value);
    }

    g_source_remove_by_user_data(coroutine);
    assert(!g_source_remove_by_user_data(coroutine));
    return ret;
}

static gboolean testcase_co_wrap(
        G_GNUC_UNUSED GIOChannel *channel,
        G_GNUC_UNUSED GIOCondition revents,
        gpointer data) {
    return testcase_co(data);
}

SmokeTestcase *testcase_new(SmokeContext *ctx) {
    SmokeTestcase *this;
    Coroutine *coroutine;

    this = g_new0(SmokeTestcase, 1);
    coroutine = &this->coroutine;
    coroutine->cb.plain = testcase_co;
    coroutine->cb.iofunc = testcase_co_wrap;
    this->ctx = ctx;

    ctx->sctx.cctx.qmp_timeout_low = 20;

    g_idle_add(testcase_co, this);
    return this;
}

void testcase_free(SmokeTestcase *this) {
    this->do_quit = TRUE;

    while (!this->quit) {
        g_main_context_iteration(g_main_context_default(), TRUE);
    }

    g_free(this);
}