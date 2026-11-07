#pragma once

#include <string>

#include "engine.h"

namespace dashboard {
  void init(const char* processJson);
  // Called by web_socket for each client connection event.
  void onConnected(uint8_t client);

  // Give this to the engine, so the dashboards follow its executions.
  class ExecutionView : public core::ProcessObserver {
   public:
    void processStarted(const core::Process& process) override;
    void nodeEntered(const core::Node& node, uint32_t runs) override;
    void taskCompleted(const core::Node& node, const std::string& resultJson) override;
    void variableSet(const std::string& name, const std::string& valueJson) override;
    void processFinished(core::ProcessStatus status, const std::string& message) override;
    void log(const std::string& message) override;
  };
}  // namespace dashboard
