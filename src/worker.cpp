/*
 * worker.cpp: run the format+write pipeline on a background thread, and
 * marshal progress updates back to the main (UI) thread.
 *
 * The worker receives a heap-allocated format_job_t snapshot — a full copy
 * of the settings made at the moment START is confirmed. This eliminates
 * the data race between the worker thread reading g_state and the UI
 * thread modifying it (e.g. device rescan, image reselect).
 *
 * Unlike the old GTask/g_idle_add version, there is no GLib main loop here:
 * progress messages are pushed into a mutex-protected queue from the worker
 * thread, and worker_poll() (called once per animation frame from ui.cpp,
 * i.e. always on the UI thread) drains it and invokes the callbacks
 * synchronously.
 */
#define RUFUS_USE_STILUS 1
#include "rufus.h"
#include "worker.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ProgressMsg {
    double      fraction;
    std::string status;
};

// Only one format job runs at a time in this app — the START button is
// disabled while a job is in flight — so a single slot is enough; no need
// for a registry keyed by job id.
struct WorkerCtx {
    std::unique_ptr<format_job_t> job;
    worker_progress_cb_t on_progress = nullptr;
    worker_done_cb_t     on_done     = nullptr;
    void                 *user_data  = nullptr;

    std::mutex                mu;
    std::vector<ProgressMsg>  pending_progress;
    bool                      done = false;
    int                       rc   = 0;
    std::thread               thread;
};

std::unique_ptr<WorkerCtx> g_active;

void worker_progress_thunk(double fraction, const char *status, void *user) {
    auto *ctx = static_cast<WorkerCtx *>(user);
    std::lock_guard<std::mutex> lk(ctx->mu);
    ctx->pending_progress.push_back({fraction, status ? status : ""});
}

} // namespace

void worker_run_format(const format_job_t  *job,
                       worker_progress_cb_t on_progress,
                       worker_done_cb_t     on_done,
                       void                *user_data)
{
    auto ctx = std::make_unique<WorkerCtx>();
    ctx->job         = std::make_unique<format_job_t>(*job);   /* owned copy */
    ctx->on_progress = on_progress;
    ctx->on_done     = on_done;
    ctx->user_data   = user_data;

    WorkerCtx *raw = ctx.get();
    g_active = std::move(ctx);

    raw->thread = std::thread([raw] {
        int rc = format_and_write(raw->job.get(), worker_progress_thunk, raw);
        std::lock_guard<std::mutex> lk(raw->mu);
        raw->rc   = rc;
        raw->done = true;
    });
}

void worker_poll()
{
    if (!g_active) return;
    WorkerCtx *ctx = g_active.get();

    std::vector<ProgressMsg> msgs;
    bool finished;
    int  rc;
    {
        std::lock_guard<std::mutex> lk(ctx->mu);
        msgs.swap(ctx->pending_progress);
        finished = ctx->done;
        rc       = ctx->rc;
    }

    for (auto &m : msgs) {
        if (ctx->on_progress)
            ctx->on_progress(m.fraction, m.status.c_str(), ctx->user_data);
    }

    if (finished) {
        if (ctx->thread.joinable()) ctx->thread.join();
        // Call on_done while ctx is still alive (mirrors the old GTask
        // ordering: the completion callback ran before free_ctx released
        // job/ctx), then release.
        if (ctx->on_done) ctx->on_done(rc, ctx->user_data);
        g_active.reset();
    }
}
