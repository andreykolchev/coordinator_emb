#include "engine.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "config.h"
#include "process.h"

namespace core {
  namespace {
    bool timeReached(const uint32_t now, const uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

    bool isNumber(const JsonVariantConst value) { return value.is<double>() && !value.is<bool>(); }

    // Follow "field.sub" through a JSON value; an empty path is the value itself.
    JsonVariantConst lookupPath(JsonVariantConst value, const std::string& path) {
      size_t begin = 0;
      while (begin < path.size() && !value.isNull()) {
        size_t end = path.find('.', begin);
        if (end == std::string::npos) {
          end = path.size();
        }
        value = value[path.substr(begin, end - begin)];
        begin = end + 1;
      }
      return value;
    }
  }  // namespace

  // Initiate the process
  bool Engine::start(const Process& process) {
    if (status_ == ProcessStatus::Running) {
      log("Already running");
      return false;
    }
    // find the start node
    int startNodeIdx = -1;
    for (size_t i = 0; i < process.nodes.size() && startNodeIdx < 0; i++) {
      if (process.nodes[i].type == NodeType::Start) {
        startNodeIdx = static_cast<int>(i);
      }
    }
    if (startNodeIdx < 0) {
      log("Process has no start event");
      return false;
    }

    // Forget everything from the previous execution.
    process_ = &process;
    runs_.assign(process.nodes.size(), 0);
    processData_.clear();
    executions_.assign(1, NodeExecution());
    nextExecution_ = 0;
    status_ = ProcessStatus::Running;

    log("Execution of '" + process.id + "' started");
    if (processObserver_) {
      processObserver_->processStarted(process);
    }

    enterNode(0, startNodeIdx);
    return true;
  }

  // Stop the process (called from the coordinator)
  bool Engine::stop() {
    if (status_ != ProcessStatus::Running) {
      return false;
    }
    for (const NodeExecution& execution : executions_) {
      if (!execution.taskId.empty()) {
        taskDispatcher_.cancelTask(execution.taskId);
      }
    }
    finishProcess(ProcessStatus::Stopped, "Execution stopped");
    return true;
  }

  // Called from the coordinator main loop
  void Engine::tick(const uint32_t now) {
    // The steps of all executions share one budget, so a chain of instant nodes cannot block the main loop().
    // Each pass gives every execution that existed at its start one step. If the budget runs out, the next
    // tick continues after the last execution that stepped, so one branch cannot starve the others.
    int steps = 0;
    size_t lastStepped = 0;
    bool progressed = true;
    while (progressed && steps < STEPS_PER_TICK && status_ == ProcessStatus::Running) {
      progressed = false;
      const size_t count = executions_.size();
      for (size_t i = 0; i < count && steps < STEPS_PER_TICK && status_ == ProcessStatus::Running; i++) {
        const size_t executionIdx = (nextExecution_ + i) % count;
        if (executeNode(executionIdx, now)) {
          progressed = true;
          steps++;
          lastStepped = executionIdx;
        }
      }
    }
    if (steps >= STEPS_PER_TICK) {
      nextExecution_ = lastStepped + 1;
    }
    if (status_ != ProcessStatus::Running) {
      return;
    }

    // Drop the executions that ended, so indexes were stable during the passes.
    executions_.erase(std::remove_if(executions_.begin(),
                                     executions_.end(),
                                     [](const NodeExecution& execution) { return execution.phase == Phase::Done; }),
                      executions_.end());
    if (nextExecution_ >= executions_.size()) {
      nextExecution_ = 0;
    }

    // Every execution waits at a Join that no one else can complete.
    if (std::all_of(executions_.begin(), executions_.end(), [](const NodeExecution& execution) {
          return execution.phase == Phase::Parked;
        })) {
      executionFail(0, "deadlock");
    }
  }

  // Callback for task result. Worker answer with 'taskId' and 'resultData'
  void Engine::onTaskResult(const std::string& taskId, const bool ok, const std::string& resultData) {
    size_t executionIdx = executions_.size();
    for (size_t i = 0; i < executions_.size(); i++) {
      if (!taskId.empty() && executions_[i].taskId == taskId) {
        executionIdx = i;
      }
    }
    if (status_ != ProcessStatus::Running || executionIdx == executions_.size()) {
      log("   ignored result of " + taskId);
      return;
    }
    if (!ok) {
      executionFail(executionIdx, "task_failed " + resultData);
      return;
    }
    JsonDocument check;
    if (deserializeJson(check, resultData)) {
      executionFail(executionIdx, "bad_result " + resultData);  // conditions would never match it
      return;
    }
    log("   result " + resultData);
    {
      const Node& node = process_->nodes[executions_[executionIdx].nodeIdx];
      if (processObserver_) {
        processObserver_->taskCompleted(node, resultData);
      }
      if (!node.resultVariable.empty()) {
        setVariable(node.resultVariable, resultData);
      }
      leaveNode(executionIdx);
    }
  }

  // Make the node ready to start.
  void Engine::enterNode(const size_t executionIdx, const int nodeIdx) {
    NodeExecution& execution = executions_[executionIdx];
    execution.nodeIdx = nodeIdx;
    execution.phase = Phase::Ready;
    execution.taskId.clear();
    execution.queued = false;
    const Node& node = process_->nodes[nodeIdx];
    if (node.type == NodeType::Join) {
      // The execution waits here until one has arrived from every incoming route;
      // the last one to arrive consumes the others and goes on.
      execution.phase = Phase::Parked;
      const size_t arrived = numberOfParked(nodeIdx);
      log("-> " + node.id + " (arrived " + std::to_string(arrived) + "/" + std::to_string(node.incoming) + ")");
      if (arrived < node.incoming) {
        return;
      }
      for (NodeExecution& other : executions_) {
        if (&other != &execution && other.phase == Phase::Parked && other.nodeIdx == nodeIdx) {
          other.phase = Phase::Done;
        }
      }
      execution.phase = Phase::Ready;
    }
    const uint32_t runs = ++runs_[nodeIdx];
    log("-> " + node.id + (runs > 1 ? " (run " + std::to_string(runs) + ")" : ""));
    if (processObserver_) {
      processObserver_->nodeEntered(node, runs);
    }
    if (runs > MAX_NODE_RUNS) {
      executionFail(executionIdx, "too_many_runs");
    }
  }

  // Advance the execution by one phase. Returns true if anything changed.
  bool Engine::executeNode(const size_t executionIdx, const uint32_t now) {
    NodeExecution& execution = executions_[executionIdx];
    const Node& node = process_->nodes[execution.nodeIdx];

    if (execution.phase == Phase::Parked || execution.phase == Phase::Done) {
      return false;
    }
    if (execution.phase == Phase::Waiting) {
      // if deadline not reached return execution to the main loop
      if (!timeReached(now, execution.deadline)) {
        return false;
      }
      if (node.type == NodeType::ExternalTask) {
        taskDispatcher_.cancelTask(execution.taskId);
        executionFail(executionIdx, "timeout");
      } else {
        leaveNode(executionIdx);  // the delay is over, so move process to the next node
      }
      return true;
    }

    // Phase::Ready: begin the node's work.
    switch (node.type) {
      case NodeType::Start:
        leaveNode(executionIdx);
        break;
      case NodeType::Stop:
        execution.phase = Phase::Done;
        if (numberOfActiveExecutions() == 0) {
          finishProcess(ProcessStatus::Completed, "Execution completed");
        }
        break;
      case NodeType::Fork:
        forkNode(executionIdx);
        break;
      case NodeType::Join:  // fired: all executions have arrived
        leaveNode(executionIdx);
        break;
      case NodeType::Router: {
        const int to = chooseRoute(node);
        if (to < 0) {
          executionFail(executionIdx, "no_route");
        } else {
          leaveNode(executionIdx, to);
        }
        break;
      }
      case NodeType::InternalTask:
        runInternalTask(executionIdx, now);
        break;
      case NodeType::ExternalTask:
        switch (taskDispatcher_.sendExternalTask(node, execution.taskId)) {
          case DispatchResult::Sent:
            setExecutionDeadline(executionIdx, now + node.timeoutMs);
            break;
          case DispatchResult::NoWorker:
            executionFail(executionIdx, "no_worker");
            break;
          case DispatchResult::Busy:
            // Stay Ready and try again on a later tick. The task's timeout is how
            // long it may wait for an idle worker.
            if (!execution.queued) {
              execution.queued = true;
              execution.deadline = now + node.timeoutMs;
            } else if (timeReached(now, execution.deadline)) {
              executionFail(executionIdx, "dispatch_timeout");
            }
            return false;
        }
        break;
      default:
        executionFail(executionIdx, "unsupported_node");
    }
    return true;
  }

  void Engine::runInternalTask(const size_t executionIdx, const uint32_t now) {
    const Node& node = process_->nodes[executions_[executionIdx].nodeIdx];
    switch (node.internalTaskType) {
      case InternalTaskType::Delay:
        setExecutionDeadline(executionIdx, now + node.delayMs);
        break;
      case InternalTaskType::Increment:
        if (incrementVariable(node)) {
          leaveNode(executionIdx);
        } else {
          executionFail(executionIdx, "not_a_number");
        }
        break;
      case InternalTaskType::SetVariable:
        setVariable(node.variable, node.valueJson);
        leaveNode(executionIdx);
        break;
      case InternalTaskType::Service:
        if (std::string error; runService(node, error)) {
          leaveNode(executionIdx);
        } else {
          executionFail(executionIdx, error);
        }
        break;
      default:
        executionFail(executionIdx, "unknown_handler");
    }
  }

  // Check the routes in order:
  // the first whose condition holds wins;
  // if none does, the first one without a condition.
  // -1 if none.
  int Engine::chooseRoute(const Node& router) const {
    int otherwise = -1;
    // loop through all the routes
    for (const auto& [from, to, hasCondition, condition] : router.routes) {
      if (!hasCondition) {
        if (otherwise < 0) {
          otherwise = to;
        }
      } else if (conditionHolds(condition)) {
        return to;
      }
    }
    return otherwise;
  }

  // Check condition
  bool Engine::conditionHolds(const Condition& condition) const {
    // "temperature.temperature_c": variable "temperature", path "temperature_c"
    const size_t dot = condition.variable.find('.');
    const auto variable = processData_.find(condition.variable.substr(0, dot));
    if (variable == processData_.end()) {
      return false;
    }
    JsonDocument variableDoc, expectedDoc;
    if (deserializeJson(variableDoc, variable->second)) {
      log("   bad JSON in variable " + condition.variable + ": " + variable->second);
    }
    deserializeJson(expectedDoc, condition.value);
    const JsonVariantConst actual = lookupPath(variableDoc.as<JsonVariantConst>(),
                                               (dot == std::string::npos ? "" : condition.variable.substr(dot + 1)));
    const auto expected = expectedDoc.as<JsonVariantConst>();
    if (actual.isNull()) {
      return false;
    }

    if (isNumber(actual) && isNumber(expected)) {
      const auto a = actual.as<double>();
      const auto b = expected.as<double>();
      switch (condition.op) {
        case Compare::Equal:
          return a == b;
        case Compare::NotEqual:
          return a != b;
        case Compare::Less:
          return a < b;
        case Compare::LessOrEqual:
          return a <= b;
        case Compare::Greater:
          return a > b;
        case Compare::GreaterOrEqual:
          return a >= b;
      }
    }
    // Anything else (text, true/false, objects) can only be tested for equality.
    std::string a, b;
    serializeJson(actual, a);
    serializeJson(expected, b);
    if (condition.op == Compare::Equal) {
      return a == b;
    }
    return condition.op == Compare::NotEqual && a != b;
  }

  // Add node.increment to a number variable; a missing one starts at 0.
  bool Engine::incrementVariable(const Node& node) {
    double value = 0;
    const auto variable = processData_.find(node.variable);
    if (variable != processData_.end()) {
      JsonDocument doc;
      deserializeJson(doc, variable->second);
      if (!isNumber(doc.as<JsonVariantConst>())) {
        return false;
      }
      value = doc.as<double>();
    }
    JsonDocument result;
    result.set(value + node.increment);
    std::string text;
    serializeJson(result, text);
    setVariable(node.variable, text);
    return true;
  }

  // Copy `source` to `target`, replacing each string "$name" or "$name.path" with that variable's value.
  bool Engine::resolveValue(const JsonVariantConst source, const JsonVariant target) const {
    if (source.is<const char*>() && source.as<const char*>()[0] == '$') {
      const std::string path = source.as<const char*>() + 1;
      const size_t dot = path.find('.');
      const auto variable = processData_.find(path.substr(0, dot));
      if (variable == processData_.end()) {
        return false;
      }
      JsonDocument doc;
      if (deserializeJson(doc, variable->second)) {
        return false;
      }
      const JsonVariantConst value =
          lookupPath(doc.as<JsonVariantConst>(), dot == std::string::npos ? "" : path.substr(dot + 1));
      if (value.isNull()) {
        return false;
      }
      return target.set(value);
    }
    if (source.is<JsonArrayConst>()) {
      JsonArray array = target.to<JsonArray>();
      for (const JsonVariantConst item : source.as<JsonArrayConst>()) {
        if (!resolveValue(item, array.add<JsonVariant>())) {
          return false;
        }
      }
      return true;
    }
    if (source.is<JsonObjectConst>()) {
      JsonObject object = target.to<JsonObject>();
      for (const JsonPairConst pair : source.as<JsonObjectConst>()) {
        if (!resolveValue(pair.value(), object[std::string(pair.key().c_str())].to<JsonVariant>())) {
          return false;
        }
      }
      return true;
    }
    return target.set(source);
  }

  // Run the dispatcher's method named node.method on the resolved context; store the result like a worker's.
  bool Engine::runService(const Node& node, std::string& error) {
    JsonDocument contextDoc, inputDoc;
    std::string input, result;
    if (deserializeJson(contextDoc, node.context) ||
        !resolveValue(contextDoc.as<JsonVariantConst>(), inputDoc.to<JsonVariant>())) {
      error = "bad_context";
      return false;
    }
    serializeJson(inputDoc, input);
    if (!taskDispatcher_.sendInternalTask(node, input, result)) {
      error = "service_failed " + result;
      return false;
    }
    JsonDocument check;
    if (deserializeJson(check, result)) {
      error = "bad_result " + result;
      return false;
    }
    log("   result " + result);
    if (processObserver_) {
      processObserver_->taskCompleted(node, result);
    }
    if (!node.resultVariable.empty()) {
      setVariable(node.resultVariable, result);
    }
    return true;
  }

  // Set variable in aggregate process data
  void Engine::setVariable(const std::string& name, const std::string& valueJson) {
    processData_[name] = valueJson;
    log("   " + name + " = " + valueJson);
    if (processObserver_) {
      processObserver_->variableSet(name, valueJson);
    }
  }

  // Number of executions that have not ended
  size_t Engine::numberOfActiveExecutions() const {
    return std::count_if(executions_.begin(), executions_.end(), [](const NodeExecution& execution) {
      return execution.phase != Phase::Done;
    });
  }

  // Number of executions waiting at the Join
  size_t Engine::numberOfParked(const int nodeIdx) const {
    return std::count_if(executions_.begin(), executions_.end(), [nodeIdx](const NodeExecution& execution) {
      return execution.phase == Phase::Parked && execution.nodeIdx == nodeIdx;
    });
  }

  // Take all outgoing routes: the execution goes along the first, a new one along each of the others.
  void Engine::forkNode(const size_t executionIdx) {
    const std::vector<Route>& routes = process_->nodes[executions_[executionIdx].nodeIdx].routes;
    if (numberOfActiveExecutions() + routes.size() - 1 > MAX_EXECUTIONS) {
      executionFail(executionIdx, "too_many_executions");
      return;
    }
    enterNode(executionIdx, routes[0].to);
    for (size_t i = 1; i < routes.size() && status_ == ProcessStatus::Running; i++) {
      executions_.emplace_back();  // may reallocate: no Execution reference is held across it
      enterNode(executions_.size() - 1, routes[i].to);
    }
  }

  // The execution's node is done: go and activate next node
  void Engine::leaveNode(const size_t executionIdx, int to) {
    const Node& node = process_->nodes[executions_[executionIdx].nodeIdx];
    if (to < 0) {
      if (node.routes.empty()) {
        executionFail(executionIdx, "dead_end");
        return;
      }
      to = node.routes[0].to;  // get default next node
    }
    enterNode(executionIdx, to);
  }

  // set the execution's node deadline
  void Engine::setExecutionDeadline(const size_t executionIdx, const uint32_t deadline) {
    executions_[executionIdx].phase = Phase::Waiting;
    executions_[executionIdx].deadline = deadline;
  }

  // finish the process
  void Engine::finishProcess(const ProcessStatus status, const std::string& message) {
    status_ = status;
    executions_.clear();  // so a late result is ignored
    log(message);
    if (processObserver_) {
      processObserver_->processFinished(status, message);
    }
  }

  // node execution fail with error
  void Engine::executionFail(const size_t executionIdx, const std::string& error) {
    const std::string message =
        "Execution failed at '" + process_->nodes[executions_[executionIdx].nodeIdx].id + "': " + error;
    // The other branches die with the process, so their tasks are not wanted any more.
    for (size_t i = 0; i < executions_.size(); i++) {
      if (i != executionIdx && !executions_[i].taskId.empty()) {
        taskDispatcher_.cancelTask(executions_[i].taskId);
      }
    }
    finishProcess(ProcessStatus::Failed, message);
  }

  // Engine's own log line; goes to the observer if there is one
  void Engine::log(const std::string& message) const {
    if (processObserver_) {
      processObserver_->log(message);
    }
  }
}  // namespace core
