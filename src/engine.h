#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "process.h"

namespace core {
  enum class ProcessStatus { Idle, Running, Completed, Failed, Stopped };

  // What the coordinator did with a task the engine asked to send.
  enum class DispatchResult {
    Sent,      // on its way to a worker; taskId identifies it
    NoWorker,  // no worker has the capability
    Busy       // workers have it, but none is idle right now
  };

  // Process task dispatcher interface
  class TaskDispatcher {
   public:
    virtual ~TaskDispatcher() = default;
    // Send an external task to a worker
    virtual DispatchResult sendExternalTask(const Node& node, std::string& taskId) = 0;
    // Run the coordinator's own method named node.method, synchronously.
    // `inputJson` is the resolved node context; on return `resultJson` holds the result (or the error).
    virtual bool sendInternalTask(const Node& node, const std::string& inputJson, std::string& resultJson) = 0;
    virtual void cancelTask(const std::string& taskId) = 0;
  };

  // Process status observer interface
  class ProcessObserver {
   public:
    virtual ~ProcessObserver() = default;
    virtual void processStarted(const Process& process) = 0;
    virtual void nodeEntered(const Node& node, uint32_t runs) = 0;
    virtual void taskCompleted(const Node& node, const std::string& resultJson) = 0;
    virtual void variableSet(const std::string& name, const std::string& valueJson) = 0;
    virtual void processFinished(ProcessStatus status, const std::string& message) = 0;
    virtual void log(const std::string& message) = 0;
  };

  // Phase of an execution's node
  enum class Phase {
    Ready,    // ready but not started its work
    Waiting,  // waits for a task result or a deadline
    Parked,   // arrived at a Join that still waits for the other executions
    Done      // ended or consumed by a Join; removed at the end of the tick
  };

  // One place where the process currently is.
  struct NodeExecution {
    int nodeIdx = -1;            // position of the node in process->nodes
    Phase phase = Phase::Ready;  // execution phase
    uint32_t deadline = 0;       // when the 'Waiting' node gives up
    std::string taskId;          // the task id the 'Waiting' node waits for
    bool queued = false;         // the task waits for an idle worker
  };

  class Engine {
   public:
    // `observer` may be null.
    explicit Engine(TaskDispatcher& dispatcher, ProcessObserver* observer = nullptr)
        : taskDispatcher_(dispatcher), processObserver_(observer) {}

    // Init a new process
    bool start(const Process& process);

    // Stop running process execution
    bool stop();

    // Run the executions; call from the main loop
    void tick(uint32_t now);

    // Callback for task result. Worker answer with 'taskId' and 'resultData'
    void onTaskResult(const std::string& taskId, bool ok, const std::string& resultData);

    // Getter for process status
    ProcessStatus status() const { return status_; }

   private:
    // callbacks
    TaskDispatcher& taskDispatcher_;
    ProcessObserver* processObserver_;
    // process
    const Process* process_ = nullptr;
    ProcessStatus status_ = ProcessStatus::Idle;
    // where the process is; addressed by index, as the vector may grow
    std::vector<NodeExecution> executions_;
    size_t nextExecution_ = 0;  // where the round-robin continues
    // process data layer
    std::vector<uint32_t> runs_;                      // times each node was reached
    std::map<std::string, std::string> processData_;  // aggregated process data

    void enterNode(size_t executionIdx, int nodeIdx);
    bool executeNode(size_t executionIdx, uint32_t now);
    void runInternalTask(size_t executionIdx, uint32_t now);
    int chooseRoute(const Node& router) const;
    bool conditionHolds(const Condition& condition) const;
    bool incrementVariable(const Node& node);
    bool runService(const Node& node, std::string& error);
    bool resolveValue(JsonVariantConst source, JsonVariant target) const;
    void setVariable(const std::string& name, const std::string& valueJson);
    void forkNode(size_t executionIdx);
    void leaveNode(size_t executionIdx, int to = -1);
    void setExecutionDeadline(size_t executionIdx, uint32_t deadline);
    void finishProcess(ProcessStatus status, const std::string& message);
    void executionFail(size_t executionIdx, const std::string& error);
    size_t numberOfActiveExecutions() const;
    size_t numberOfParked(int nodeIdx) const;
    void log(const std::string& message) const;
  };
}  // namespace core
