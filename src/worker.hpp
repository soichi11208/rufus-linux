// worker.hpp — internal bridge between worker.cpp and ui.cpp.
//
// worker_run_format() (declared in rufus.h under RUFUS_USE_STILUS) starts a
// background std::thread and buffers progress messages in a mutex-protected
// queue. worker_poll() drains that queue and invokes the registered
// callbacks on whichever thread calls it — ui.cpp calls it once per
// animation frame, which is always the UI/main thread, so on_progress and
// on_done never run concurrently with the UI's own widget-tree access.
#pragma once

void worker_poll();
