#include "worker_registry.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <algorithm>

#include "config.h"
#include "web_socket.h"

namespace worker_registry {
  namespace {
    // Indexed by WebSocket client number, so a client's slot is its worker.
    Worker workers[web_socket::MAX_CLIENTS];

    ResultHandler resultHandler;
    uint32_t taskCounter = 0;

    // Tell the worker to stop its current task. Sent at most once per task.
    void cancelWorkerTask(const uint8_t client) {
      Worker& worker = workers[client];
      if (worker.cancelled) {
        return;
      }
      worker.cancelled = true;
      JsonDocument doc;
      doc["type"] = "cancel_task";
      doc["task_id"] = worker.taskId;
      std::string text;
      serializeJson(doc, text);
      web_socket::sendText(client, text);
    }
  }  // namespace

  void setResultHandler(ResultHandler handler) { resultHandler = std::move(handler); }

  Worker& at(const uint8_t client) { return workers[client]; }

  bool isWorker(const uint8_t client) { return !workers[client].id.empty(); }

  std::string stateMessage() {
    JsonDocument doc;
    doc["type"] = "state";
    const auto list = doc["workers"].to<JsonArray>();
    for (const Worker& worker : workers) {
      if (worker.id.empty()) {
        continue;
      }
      auto item = list.add<JsonObject>();
      item["id"] = worker.id;
      item["ip"] = worker.ip;
      item["busy"] = worker.busy;
      item["status"] = worker.status;
      item["task_id"] = worker.taskId;
      item["task_name"] = worker.taskName;
      item["result"] = worker.result.empty() ? "-" : worker.result;
      auto capabilities = item["capabilities"].to<JsonArray>();
      for (const std::string& capability : worker.capabilities) {
        capabilities.add(capability);
      }
    }
    std::string text;
    serializeJson(doc, text);
    return text;
  }

  void broadcastState() { web_socket::broadcast(stateMessage()); }

  core::DispatchResult dispatchTask(const std::string& capability, const std::string& workerId,
                                    const std::string& inputsJson, const uint32_t timeoutMs, const bool manual,
                                    std::string& taskId) {
    bool capable = false;
    for (uint8_t client = 0; client < web_socket::MAX_CLIENTS; client++) {
      Worker& worker = workers[client];
      if (worker.id.empty() || (!workerId.empty() && worker.id != workerId) ||
          std::ranges::find(worker.capabilities, capability) == worker.capabilities.end()) {
        continue;
      }
      capable = true;
      if (worker.busy) {
        continue;
      }

      taskId = "task-" + std::to_string(++taskCounter);
      JsonDocument doc;
      doc["type"] = "task";
      doc["task_id"] = taskId;
      doc["name"] = capability;
      doc["inputs"] = serialized(inputsJson);
      doc["timeout_ms"] = timeoutMs;
      std::string text;
      serializeJson(doc, text);
      // A failed send can disconnect the client inside sendText, which resets
      // this slot, so only claim the worker if it is still there.
      if (!web_socket::sendText(client, text) || worker.id.empty()) {
        continue;
      }

      worker.busy = true;
      worker.manual = manual;
      worker.cancelled = false;
      worker.status = "Busy";
      worker.taskId = taskId;
      worker.taskName = capability;
      worker.result.clear();
      worker.deadline = millis() + timeoutMs;
      broadcastState();

      Serial.printf("   sent %s (%s) to %s\n", taskId.c_str(), capability.c_str(), worker.id.c_str());
      return core::DispatchResult::Sent;
    }
    return capable ? core::DispatchResult::Busy : core::DispatchResult::NoWorker;
  }

  void cancelTask(const std::string& taskId) {
    for (uint8_t client = 0; client < web_socket::MAX_CLIENTS; client++) {
      Worker& worker = workers[client];
      if (!worker.busy || worker.taskId != taskId) {
        continue;
      }
      // The worker stays busy until it answers or the deadline passes, so it
      // never gets a second task while it is still running this one.
      cancelWorkerTask(client);
      worker.status = "Cancelling";
      broadcastState();
      return;
    }
  }

  void failTask(Worker& worker, const char* reason) {
    if (!worker.busy) {
      return;
    }
    worker.busy = false;
    worker.status = std::string("Failed: ") + reason;
    if (!worker.manual && !worker.cancelled && resultHandler) {
      resultHandler(worker.taskId, false, std::string("{\"error\":\"") + reason + "\"}");
    }
  }

  void onRegister(const uint8_t client, const JsonVariantConst message) {
    std::string id = message["worker_id"] | "worker";
    if (id.empty()) {
      id = "worker";  // an empty ID would make the slot look free
    }
    // A worker that reconnects may still have a task on its old connection
    for (uint8_t other = 0; other < web_socket::MAX_CLIENTS; other++) {
      if (other != client && workers[other].id == id) {
        failTask(workers[other], "worker re-registered");
        workers[other] = Worker();
        web_socket::disconnect(other);
      }
    }

    Worker& worker = workers[client];
    failTask(worker, "worker re-registered");
    worker = Worker();
    worker.id = id;
    worker.ip = web_socket::remoteIP(client);
    for (JsonVariantConst capability : message["capabilities"].as<JsonArrayConst>()) {
      worker.capabilities.push_back(capability | "");
    }
    Serial.printf("Worker registered: %s\n", worker.id.c_str());
    broadcastState();
  }

  void onTaskResult(const uint8_t client, const JsonVariantConst message) {
    const std::string taskId = message["task_id"] | "";
    Worker& worker = workers[client];
    // Only a registered worker answering the task it was given may report it.
    if (worker.id.empty() || taskId.empty() || taskId != worker.taskId) {
      Serial.printf("   ignored result of %s\n", taskId.c_str());
      return;
    }

    std::string result;
    serializeJson(message["result"], result);
    const bool ok = std::string(message["status"] | "") == "completed";
    // Checked even after a timeout, so a late manual result stays away from
    // the engine.
    const bool manual = worker.manual;
    if (worker.busy) {
      worker.busy = false;
      worker.status = worker.cancelled ? "Cancelled" : ok ? "Completed" : "Failed";
      worker.result = result;
      broadcastState();
    }
    // The result handler (the engine's onTaskResult) does not send tasks; the
    // engine sends the next one on its next tick, so it is safe to call from here.
    if (!manual && !worker.cancelled && resultHandler) {
      resultHandler(taskId, ok, result);
    }
  }

  void onDisconnected(const uint8_t client) {
    Worker& worker = workers[client];
    if (!worker.id.empty()) {
      Serial.printf("Worker disconnected: %s\n", worker.id.c_str());
      failTask(worker, "worker disconnected");
      worker = Worker();
      broadcastState();
    }
  }

  void checkTimeouts() {
    // Give up on a worker that does not answer in time: cancel its task and
    // free it for the next one. The engine times out the task on its own.
    bool changed = false;
    for (uint8_t client = 0; client < web_socket::MAX_CLIENTS; client++) {
      Worker& worker = workers[client];
      if (worker.busy && static_cast<int32_t>(millis() - worker.deadline) >= 0) {
        cancelWorkerTask(client);
        worker.busy = false;
        worker.status = "Failed: timeout";
        changed = true;
      }
    }
    if (changed) {
      broadcastState();
    }
  }

  void printWorkers() {
    int count = 0;
    for (const Worker& worker : workers) {
      if (!worker.id.empty()) {
        count++;
      }
    }
    Serial.printf("Workers registered: %d\n", count);

    for (const Worker& worker : workers) {
      if (worker.id.empty()) {
        continue;
      }

      std::string caps;
      for (const std::string& capability : worker.capabilities) {
        if (!caps.empty()) {
          caps += ", ";
        }
        caps += capability;
      }

      Serial.printf("  %-16s %-15s  %s\n", worker.id.c_str(), worker.ip.c_str(), worker.status.c_str());
      Serial.printf("    capabilities: %s\n", caps.c_str());
      Serial.printf("    last result:  %s\n", worker.result.empty() ? "-" : worker.result.c_str());
    }
  }
}  // namespace worker_registry
