#include "dashboard.h"

#include <ArduinoJson.h>

#include <map>
#include <vector>

#include "web_socket.h"

namespace dashboard {
  namespace {
    // What the page shows for a node the current execution has reached.
    struct NodeRun {
      std::string state;  // "running", "succeeded", "failed" or "cancelled"
      uint32_t runs = 0;
      std::string next;       // id of the node the execution moved on to
      std::string result;     // JSON, for an external task
      uint32_t activity = 0;  // when the node last entered or completed, to tell which predecessor just finished
    };

    const char* processJson_ = nullptr;
    const core::Process* process_ = nullptr;  // the running process; the engine's observer calls name nodes only

    // The current (or last) execution.
    uint32_t executionCounter = 0;
    std::string executionId;  // empty until the first execution
    std::string executionStatus;
    std::string reason;
    std::map<std::string, NodeRun> nodes;  // by node id
    uint32_t activityCounter = 0;
    JsonDocument variables;

    void send(const JsonDocument& doc) {
      std::string text;
      serializeJson(doc, text);
      web_socket::broadcast(text);
    }

    void describeNode(const JsonObject item, const std::string& id, const NodeRun& run) {
      item["execution_id"] = executionId;
      item["node_id"] = id;
      item["state"] = run.state;
      item["runs"] = run.runs;
      if (!run.next.empty()) {
        item["next"] = run.next;
      }
      if (!run.result.empty()) {
        item["result"] = serialized(run.result);
      }
    }

    void setState(const std::string& id, const char* state) {
      if (id.empty()) {
        return;
      }
      NodeRun& run = nodes[id];
      run.state = state;
      JsonDocument doc;
      const auto item = doc.to<JsonObject>();  // clears doc, so set "type" after
      item["type"] = "node_status";
      describeNode(item, id, run);
      send(doc);
    }

    // The nodes with a route into `node` that have finished but have not moved on yet.
    // The engine does not say where an execution came from, so it is worked out from the routes.
    // A node is finished when it is running (an instant node or a delay) or has its result (a task).
    void closePredecessors(const core::Node& node) {
      const int nodeIdx = static_cast<int>(&node - process_->nodes.data());
      std::vector<const core::Node*> pending;
      for (const core::Node& from : process_->nodes) {
        const auto run = nodes.find(from.id);
        if (run == nodes.end() || !run->second.next.empty() ||
            (run->second.state != "running" && run->second.state != "succeeded")) {
          continue;
        }
        for (const core::Route& route : from.routes) {
          if (route.to == nodeIdx) {
            pending.push_back(&from);
            break;
          }
        }
      }
      if (pending.empty()) {
        return;
      }
      if (node.type != core::NodeType::Join) {
        // One execution entered, so one predecessor finished: the one that was active last.
        // Another branch may still be running a delay on a route to the same node.
        const core::Node* last = pending[0];
        for (const core::Node* from : pending) {
          if (nodes[from->id].activity > nodes[last->id].activity) {
            last = from;
          }
        }
        pending.assign(1, last);
      }
      for (const core::Node* from : pending) {
        nodes[from->id].next = node.id;
        setState(from->id, "succeeded");
      }
    }

    // The process is over: every node still running ends in `state`; `failedId` ends as failed.
    void closeRunning(const char* state, const std::string& failedId = "") {
      std::vector<std::string> running;
      for (const auto& [id, run] : nodes) {
        if (run.state == "running") {
          running.push_back(id);
        }
      }
      for (const std::string& id : running) {
        setState(id, id == failedId ? "failed" : state);
      }
      if (!failedId.empty() && nodes[failedId].state != "failed") {
        setState(failedId, "failed");  // not running, e.g. a Join that can never fire
      }
    }

    void sendExecution() {
      JsonDocument doc;
      doc["type"] = "execution_status";
      doc["execution_id"] = executionId;
      doc["status"] = executionStatus;
      doc["reason"] = reason;
      send(doc);
    }

    void sendVariables() {
      JsonDocument doc;
      doc["type"] = "variables";
      doc["variables"] = variables;
      send(doc);
    }
  }  // namespace

  void init(const char* processJson) { processJson_ = processJson; }

  static std::string buildSnapshot() {
    if (!processJson_) {
      return "";
    }
    JsonDocument doc;
    doc["type"] = "snapshot";
    doc["process"] = serialized(processJson_);
    doc["variables"] = variables;
    if (executionId.empty()) {
      doc["execution"] = nullptr;
    } else {
      const auto execution = doc["execution"].to<JsonObject>();
      execution["id"] = executionId;
      execution["status"] = executionStatus;
      execution["reason"] = reason;
      const auto list = execution["nodes"].to<JsonArray>();
      for (const auto& [fst, snd] : nodes) {
        describeNode(list.add<JsonObject>(), fst, snd);
      }
    }
    std::string text;
    serializeJson(doc, text);
    return text;
  }

  void onConnected(const uint8_t client) {
    const auto snapshot = buildSnapshot();
    if (!snapshot.empty()) {
      web_socket::sendText(client, snapshot);
    }
  }

  void ExecutionView::processStarted(const core::Process& process) {
    process_ = &process;
    executionId = "exec-" + std::to_string(++executionCounter);
    executionStatus = "running";
    reason.clear();
    nodes.clear();
    variables.clear();
    sendExecution();
    sendVariables();
  }

  void ExecutionView::nodeEntered(const core::Node& node, const uint32_t runs) {
    closePredecessors(node);
    NodeRun& run = nodes[node.id];
    run.runs = runs;
    run.next.clear();
    run.result.clear();
    run.activity = ++activityCounter;
    setState(node.id, "running");
  }

  // The task is done; the node moves on right away.
  void ExecutionView::taskCompleted(const core::Node& node, const std::string& resultJson) {
    NodeRun& run = nodes[node.id];
    run.result = resultJson;
    run.activity = ++activityCounter;
    setState(node.id, "succeeded");
  }

  void ExecutionView::variableSet(const std::string& name, const std::string& valueJson) {
    variables[name] = serialized(valueJson);
    sendVariables();
  }

  void ExecutionView::processFinished(const core::ProcessStatus status, const std::string& message) {
    switch (status) {
      case core::ProcessStatus::Completed:
        executionStatus = "succeeded";
        closeRunning("succeeded");
        break;
      case core::ProcessStatus::Failed: {
        executionStatus = "failed";
        reason = message;
        // "Execution failed at '<node>': <reason>"; the other branches were cancelled with the process.
        const size_t begin = message.find('\'');
        const size_t end = message.find('\'', begin + 1);
        const std::string failedId =
            begin == std::string::npos || end == std::string::npos ? "" : message.substr(begin + 1, end - begin - 1);
        closeRunning("cancelled", failedId);
        break;
      }
      default:  // Stopped
        executionStatus = "cancelled";
        closeRunning("cancelled");
    }
    sendExecution();
  }

  void ExecutionView::log(const std::string& message) {
    JsonDocument doc;
    doc["type"] = "event";
    doc["message"] = message;
    send(doc);
  }
}  // namespace dashboard
