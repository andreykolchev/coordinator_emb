#include "task_dispatcher.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include "config.h"
#include "services.h"
#include "web_socket.h"
#include "worker_registry.h"

namespace task_dispatcher {
  namespace {
    core::Engine* engine_ = nullptr;
    const core::Process* process_ = nullptr;

    // The reason a dashboard's run_task was refused, or "" if it was sent.
    const char* dispatchTaskError(const core::DispatchResult result) {
      switch (result) {
        case core::DispatchResult::Sent:
          return "";
        case core::DispatchResult::NoWorker:
          return "no worker supports this task";
        case core::DispatchResult::Busy:
          break;
      }
      return "no idle worker for this task";
    }

    // Commands from the dashboard. Returns the reason it failed, or "" on success.
    std::string processStartStop(const std::string& command) {
      if (command == "start_process") {
        return engine_->start(*process_) ? "" : "the process could not start";
      }
      return engine_->stop() ? "" : "the process is not running";
    }

    void dashboardCommandResult(const uint8_t client, const char* requestId, const std::string& error) {
      JsonDocument doc;
      doc["type"] = "response";
      doc["request_id"] = requestId;
      doc["ok"] = error.empty();
      const auto errors = doc["errors"].to<JsonArray>();
      if (!error.empty()) {
        const auto item = errors.add<JsonObject>();
        item["code"] = "command_failed";
        item["message"] = error;
      }
      std::string text;
      serializeJson(doc, text);
      web_socket::sendText(client, text);
    }

    void handleMessage(const uint8_t client, uint8_t* payload, size_t length) {
      JsonDocument message;
      if (deserializeJson(message, payload, length)) {
        Serial.println("   ignored a message that is not JSON");
        return;
      }
      const std::string type = message["type"] | "";
      if (type == "register") {
        worker_registry::onRegister(client, message);
      } else if (type == "task_result") {
        worker_registry::onTaskResult(client, message);
      } else if (type == "run_task")  // manual task run command
      {
        std::string taskId;
        const core::DispatchResult sent = worker_registry::dispatchTask(
            message["name"] | "", message["worker_id"] | "", "{}", MANUAL_TASK_TIMEOUT_MS, true, taskId);
        dashboardCommandResult(client, message["request_id"] | "", dispatchTaskError(sent));
      } else if (type == "start_process" || type == "stop_process")  // start/stop process command
      {
        dashboardCommandResult(client, message["request_id"] | "", processStartStop(type));
      }
    }
  }  // namespace

  void init(core::Engine& engine, const core::Process& process) {
    engine_ = &engine;
    worker_registry::setResultHandler([](const std::string& taskId, const bool ok, const std::string& result) {
      engine_->onTaskResult(taskId, ok, result);
    });
    process_ = &process;
    web_socket::begin();
  }

  void loop() { worker_registry::checkTimeouts(); }

  // web socket events
  void onConnected(const uint8_t client) { web_socket::sendText(client, worker_registry::stateMessage()); }

  void onText(const uint8_t client, uint8_t* payload, const size_t length) { handleMessage(client, payload, length); }

  void onDisconnected(const uint8_t client) { worker_registry::onDisconnected(client); }

  const char* processStatusName() {
    switch (engine_->status()) {
      case core::ProcessStatus::Running:
        return "Running";
      case core::ProcessStatus::Completed:
        return "Completed";
      case core::ProcessStatus::Failed:
        return "Failed";
      case core::ProcessStatus::Stopped:
        return "Stopped";
      default:
        return "Idle";
    }
  }

  // process events
  core::DispatchResult WsTaskDispatcher::sendExternalTask(const core::Node& node, std::string& taskId) {
    return worker_registry::dispatchTask(
        node.externalTaskType, node.workerId, node.context, node.timeoutMs, false, taskId);
  }

  // event from the process to run a coordinator method
  bool WsTaskDispatcher::sendInternalTask(const core::Node& node, const std::string& inputJson,
                                          std::string& resultJson) {
    return services::call(node.method, inputJson, resultJson);
  }

  // event from process to cancel a dispatched task
  void WsTaskDispatcher::cancelTask(const std::string& taskId) { worker_registry::cancelTask(taskId); }
}  // namespace task_dispatcher
