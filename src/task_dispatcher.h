#pragma once

#include <string>

#include "engine.h"

namespace task_dispatcher {
  // `engine` runs `process`; both must outlive the dispatcher.
  void init(core::Engine& engine, const core::Process& process);
  void loop();

  // Called by web_socket for each client connection event.
  void onConnected(uint8_t client);
  void onText(uint8_t client, uint8_t* payload, size_t length);
  void onDisconnected(uint8_t client);

  // The name of a process status, for the serial status report.
  const char* processStatusName();

  // The engine's TaskDispatcher: runs its external tasks on idle workers.
  class WsTaskDispatcher : public core::TaskDispatcher {
   public:
    // event from the process to dispatch a task for a worker
    core::DispatchResult sendExternalTask(const core::Node& node, std::string& taskId) override;
    // event from the process to run a coordinator method
    bool sendInternalTask(const core::Node& node, const std::string& inputJson, std::string& resultJson) override;
    // event from process to cancel a dispatched task
    void cancelTask(const std::string& taskId) override;
  };
}  // namespace task_dispatcher
