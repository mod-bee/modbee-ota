#pragma once

/**
 * Work that runs on a worker instead of the caller - flash for
 * OtaAsyncTarget, signature checks for OtaAsyncVerifier. One worker can serve
 * several (OtaEsp32Worker::add()), so a node runs one low-priority task for
 * all of OTA.
 */
class OtaBackground {
public:
  virtual ~OtaBackground() {}
  /** Does what is waiting. The worker's thread only. */
  virtual void work() = 0;
  /** True while there is work, or an inner call must be asked again soon. */
  virtual bool busy() const = 0;
  /** Called on the caller's thread when there is new work. Keep it a notify. */
  virtual void onWork(void (*wake)(void*), void* context) = 0;
};
