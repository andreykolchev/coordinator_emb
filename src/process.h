#pragma once

#include <string>
#include <vector>

#include "config.h"

namespace core {
  enum class NodeType {
    Unknown,
    Start,         // where an execution begins
    Stop,          // where an execution ends
    ExternalTask,  // runs on a worker
    InternalTask,  // runs on the coordinator
    Router,        // picks one of its outgoing routes
    Fork,          // takes all its outgoing routes
    Join           // waits for an execution on every incoming route
  };

  // What an internal task does.
  enum class InternalTaskType { Unknown, Delay, Increment, SetVariable, Service };

  // Enum for compare operations
  enum class Compare { Equal, NotEqual, Less, LessOrEqual, Greater, GreaterOrEqual };

  // A test on a variable, written on a route that leaves a Router.
  struct Condition {
    std::string variable;
    Compare op = Compare::Equal;
    std::string value = "null";  // JSON: 5, "text", true, ...
  };

  // Route
  struct Route {
    int from = -1;  // index of previous Node in Process::nodes
    int to = -1;    // index of next Node in Process::nodes
    bool hasCondition = false;
    Condition condition;
  };

  // One node of the process.
  struct Node {
    std::string id;
    NodeType type = NodeType::Unknown;

    // outgoing routes leaving this node, in the order written.
    std::vector<Route> routes;

    // number of routes ending at this node
    uint16_t incoming = 0;

    // External task
    std::string externalTaskType;
    std::string context = "{}";
    uint32_t timeoutMs = DEFAULT_TASK_TIMEOUT_MS;
    std::string resultVariable;  // store the worker's result here
    std::string workerId;        // run on this worker only; empty means any worker

    // Internal task
    InternalTaskType internalTaskType = InternalTaskType::Unknown;
    uint32_t delayMs = DEFAULT_TASK_TIMEOUT_MS;  // Delay ms
    int32_t increment = 1;                       // Increment by value
    std::string variable;                        // Process data variable name
    std::string valueJson;                       // Default value to add to process aggregated data
    std::string method;                          // Service: name of the dispatcher's method to run
                                                 // (also uses context and resultVariable)
  };

  // Process graph
  struct Process {
    std::string id;
    std::vector<Node> nodes;
  };

  // Parse `json` into a `process`.
  // Returns false, with `error` set, if not valid JSON.
  bool parseProcess(const char* json, Process& process, std::string& error);
}  // namespace core
