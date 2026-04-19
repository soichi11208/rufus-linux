/*
 * worker.c: run the format+write pipeline on a background thread using
 * GTask, and marshal progress updates back to the main (UI) thread.
 *
 * The worker receives a heap-allocated format_job_t snapshot — a full
 * copy of the settings made at the moment START is confirmed.  This
 * eliminates the data race between the worker thread reading g_state and
 * the UI thread modifying it (e.g. device rescan, image reselect).
 */
#define RUFUS_USE_GTK 1
#include "rufus.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    format_job_t        *job;         /* heap-allocated, freed by GTask */
    worker_done_cb_t     on_done;
    worker_progress_cb_t on_progress;
    gpointer             user_data;
} worker_ctx_t;

typedef struct {
    double        fraction;
    char          status[256];
    worker_ctx_t *ctx;
} progress_msg_t;

static gboolean deliver_progress(gpointer data)
{
    progress_msg_t *m = data;
    if (m->ctx && m->ctx->on_progress)
        m->ctx->on_progress(m->fraction, m->status, m->ctx->user_data);
    g_free(m);
    return G_SOURCE_REMOVE;
}

static void worker_progress_cb(double frac, const char *status, void *user)
{
    worker_ctx_t   *ctx = user;
    progress_msg_t *m   = g_new0(progress_msg_t, 1);
    m->fraction = frac;
    m->ctx      = ctx;
    if (status) strncpy(m->status, status, sizeof m->status - 1);
    g_idle_add(deliver_progress, m);
}

static void worker_thread_func(GTask        *task,
                               gpointer      source,
                               gpointer      task_data,
                               GCancellable *cancellable)
{
    (void)source; (void)cancellable;
    worker_ctx_t *ctx = task_data;
    int rc = format_and_write(ctx->job, worker_progress_cb, ctx);
    g_task_return_int(task, rc);
}

static void worker_task_done(GObject      *src,
                             GAsyncResult *res,
                             gpointer      user)
{
    (void)src; (void)user;
    worker_ctx_t *ctx = g_task_get_task_data(G_TASK(res));
    GError *err = NULL;
    int rc = (int)g_task_propagate_int(G_TASK(res), &err);
    if (err) { rufus_log("worker: %s", err->message); g_error_free(err); rc = -1; }
    if (ctx->on_done) ctx->on_done(rc, ctx->user_data);
}

static void free_ctx(gpointer p)
{
    worker_ctx_t *ctx = p;
    g_free(ctx->job);
    g_free(ctx);
}

void worker_run_format(const format_job_t  *job,
                       worker_progress_cb_t on_progress,
                       worker_done_cb_t     on_done,
                       gpointer             user_data)
{
    worker_ctx_t *ctx = g_new0(worker_ctx_t, 1);
    ctx->job         = g_memdup2(job, sizeof *job);  /* owned copy */
    ctx->on_progress = on_progress;
    ctx->on_done     = on_done;
    ctx->user_data   = user_data;

    GTask *task = g_task_new(NULL, NULL, worker_task_done, NULL);
    g_task_set_task_data(task, ctx, free_ctx);
    g_task_run_in_thread(task, worker_thread_func);
    g_object_unref(task);
}
