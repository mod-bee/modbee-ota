#pragma once

#include "../OtaAsyncTarget.h"
#include "../OtaBackground.h"

#if defined(ESP_PLATFORM) && defined(MODBEE_OTA_HAS_ASYNC)

#ifndef MODBEE_OTA_WORKER_JOBS
#define MODBEE_OTA_WORKER_JOBS 3
#endif

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * The FreeRTOS task that does OTA's slow work: an OtaAsyncTarget's flash, an
 * OtaAsyncVerifier's signature check - add() each one it serves.
 *
 * PRIORITY IS THE POINT. It runs below the PLC's tasks (priority 1 by
 * default, just above idle), so a 60 ms sector erase or a 130 ms boot-slot
 * switch takes time the PLC does not want. On the Node UIO an erase did not
 * stall the other core; it holds only this task's own CPU time.
 *
 * It sleeps until the target has work (a task notification), so an idle node
 * pays nothing for it.
 */
class OtaEsp32Worker {
public:
  explicit OtaEsp32Worker(OtaBackground& first) { add(first); }

  /** Another piece of background work for the same task - before begin() or
   *  after it (an OpenPLC signing block starts after the node block). */
  bool add(OtaBackground& work);

  /** Starts the task. `core` is tskNO_AFFINITY or 0/1. False if it could not.
   *  The stack covers a P-256 signature check through wolfSSL. */
  bool begin(UBaseType_t priority = 1, BaseType_t core = tskNO_AFFINITY,
             uint32_t stackBytes = 8192);

private:
  static void run(void* self);
  static void notify(void* self);

  OtaBackground* _jobs[MODBEE_OTA_WORKER_JOBS] = {};
  // The worker reads it while add() may write it: the slot is filled first,
  // then the count published.
  std::atomic<int> _count{0};
  TaskHandle_t _task = nullptr;
};

#endif
