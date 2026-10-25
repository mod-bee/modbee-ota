#include "OtaEsp32Worker.h"

#if defined(ESP_PLATFORM) && defined(MODBEE_OTA_HAS_ASYNC)

#include "../OtaNoExcept.h"

bool OtaEsp32Worker::add(OtaBackground& work) {
  const int n = _count.load(std::memory_order_acquire);
  for (int i = 0; i < n; i++) {
    if (_jobs[i] == &work) return true;
  }
  if (n >= MODBEE_OTA_WORKER_JOBS) return false;
  _jobs[n] = &work;
  if (_task) work.onWork(&OtaEsp32Worker::notify, this);
  _count.store(n + 1, std::memory_order_release);
  return true;
}

bool OtaEsp32Worker::begin(UBaseType_t priority, BaseType_t core, uint32_t stackBytes) {
  if (_task) return true;
  const int n = _count.load(std::memory_order_acquire);
  for (int i = 0; i < n; i++) _jobs[i]->onWork(&OtaEsp32Worker::notify, this);
  return xTaskCreatePinnedToCore(&OtaEsp32Worker::run, "modbee-ota", stackBytes, this, priority,
                                 &_task, core) == pdPASS;
}

void OtaEsp32Worker::notify(void* self) {
  TaskHandle_t task = static_cast<OtaEsp32Worker*>(self)->_task;
  if (task) xTaskNotifyGive(task);
}

void OtaEsp32Worker::run(void* self) {
  OtaEsp32Worker& w = *static_cast<OtaEsp32Worker*>(self);
  for (;;) {
    // Woken by new work; while an inner call answers Pending, look again
    // soon; otherwise a long sleep as a safety net.
    const int n = w._count.load(std::memory_order_acquire);
    bool busy = false;
    for (int i = 0; i < n; i++) busy = busy || w._jobs[i]->busy();
    ulTaskNotifyTake(pdTRUE, busy ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(500));
    for (int i = 0; i < n; i++) w._jobs[i]->work();
  }
}

#endif
