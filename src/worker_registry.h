#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine.h"

namespace worker_registry {
  struct Worker {
    std::string id;  // empty while the slot is free
    std::string ip;
    std::vector<std::string> capabilities;

    // The current (or last) task, shown on the dashboard.
    bool busy = false;
    std::string status = "Idle";
    std::string taskId, taskName, result;
    bool manual = false;     // run from a dashboard, so not the engine's
    bool cancelled = false;  // the engine stopped; the answer is not reported
    uint32_t deadline = 0;   // millis() when a busy worker is given up on
  };

  // The worker slot of a WebSocket client; a client's slot is its worker.
  Worker& at(uint8_t client);

  // Whether the client slot is a registered worker.
  bool isWorker(uint8_t client);

  // The "state" message: every registered worker.
  std::string stateMessage();

  // Send the "state" message to the dashboards.
  void broadcastState();

  // Where a worker's result, or a task given up on, is reported (the engine's onTaskResult).
  using ResultHandler = std::function<void(const std::string& taskId, bool ok, const std::string& resultJson)>;
  void setResultHandler(ResultHandler handler);

  // Send a task to an idle worker with this capability (and this id, if given).
  core::DispatchResult dispatchTask(const std::string& capability, const std::string& workerId,
                                    const std::string& inputsJson, uint32_t timeoutMs, bool manual,
                                    std::string& taskId);

  // Ask the worker running `taskId` to stop it.
  void cancelTask(const std::string& taskId);

  // Worker messages; `message` is the parsed JSON the worker sent.
  void onRegister(uint8_t client, JsonVariantConst message);
  void onTaskResult(uint8_t client, JsonVariantConst message);
  void onDisconnected(uint8_t client);

  // Give up the worker's task because the worker is gone or starting over, so
  // the engine hears of it now rather than at the task's timeout.
  void failTask(Worker& worker, const char* reason);

  // Give up on workers that do not answer in time; call from the main loop.
  void checkTimeouts();

  // Print the registered workers, their capabilities and last task to Serial.
  void printWorkers();
}  // namespace worker_registry
