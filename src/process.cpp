#include "process.h"

#include <ArduinoJson.h>

#include <map>

namespace core {
  namespace {
    NodeType getNodeType(const std::string& val) {
      if (val == "Start") {
        return NodeType::Start;
      }
      if (val == "Stop") {
        return NodeType::Stop;
      }
      if (val == "ExternalTask") {
        return NodeType::ExternalTask;
      }
      if (val == "InternalTask") {
        return NodeType::InternalTask;
      }
      if (val == "Router") {
        return NodeType::Router;
      }
      if (val == "Fork") {
        return NodeType::Fork;
      }
      if (val == "Join") {
        return NodeType::Join;
      }
      return NodeType::Unknown;
    }

    InternalTaskType getInternalTaskType(const std::string& val) {
      if (val == "Delay") {
        return InternalTaskType::Delay;
      }
      if (val == "Increment") {
        return InternalTaskType::Increment;
      }
      if (val == "SetVariable") {
        return InternalTaskType::SetVariable;
      }
      if (val == "Service") {
        return InternalTaskType::Service;
      }
      return InternalTaskType::Unknown;
    }

    bool getCompare(const std::string& val, Compare& op) {
      static const std::pair<const char*, Compare> ops[] = {{"==", Compare::Equal},
                                                            {"!=", Compare::NotEqual},
                                                            {"<", Compare::Less},
                                                            {"<=", Compare::LessOrEqual},
                                                            {">", Compare::Greater},
                                                            {">=", Compare::GreaterOrEqual}};
      for (const auto& [fst, snd] : ops) {
        if (val == fst) {
          op = snd;
          return true;
        }
      }
      return false;
    }

    void parseNode(const JsonObjectConst json, Node& node) {
      node.id = json["id"] | "";
      node.type = getNodeType(json["type"] | "");
      switch (node.type) {
        case NodeType::ExternalTask:
          node.externalTaskType = json["externalTaskType"] | "";
          node.timeoutMs = json["timeoutMs"] | DEFAULT_TASK_TIMEOUT_MS;
          node.resultVariable = json["resultVariable"] | "";
          node.workerId = json["workerId"] | "";
          if (!json["context"].isNull()) {
            node.context.clear();
            serializeJson(json["context"], node.context);
          }
          break;
        case NodeType::InternalTask:
          node.internalTaskType = getInternalTaskType(json["internalTaskType"] | "");
          if (node.internalTaskType == InternalTaskType::Delay) {
            node.delayMs = json["delayMs"] | DEFAULT_TASK_TIMEOUT_MS;
          }
          if (node.internalTaskType == InternalTaskType::Increment) {
            node.increment = json["increment"] | node.increment;
            node.variable = json["variable"] | "";
          }
          if (node.internalTaskType == InternalTaskType::SetVariable) {
            node.variable = json["variable"] | "";
            node.valueJson.clear();
            serializeJson(json["value"], node.valueJson);  // "null" if missing
          }
          if (node.internalTaskType == InternalTaskType::Service) {
            node.method = json["method"] | "";
            node.resultVariable = json["resultVariable"] | "";
            if (!json["context"].isNull()) {
              node.context.clear();
              serializeJson(json["context"], node.context);
            }
          }
          break;
        case NodeType::Unknown:
        case NodeType::Start:
        case NodeType::Stop:
        case NodeType::Router:
        case NodeType::Fork:
        case NodeType::Join:
          break;
      }
    }
  }  // namespace

  bool parseProcess(const char* json, Process& process, std::string& error) {
    process = Process();
    // parse json
    JsonDocument jsonDoc;
    DeserializationError jsonError = deserializeJson(jsonDoc, json);
    if (jsonError) {
      error = std::string("invalid JSON: ") + jsonError.c_str();
      return false;
    }

    // Read process object
    JsonObjectConst processJson = jsonDoc["process"];
    if (processJson.isNull()) {
      error = "no \"process\" object";
      return false;
    }
    process.id = processJson["id"] | "";

    // Read the nodes
    std::map<std::string, int> nodeIdToIdx;
    for (JsonObjectConst nodeJson : processJson["nodes"].as<JsonArrayConst>()) {
      process.nodes.emplace_back();
      const Node& node = process.nodes.back();
      // parse Node json
      parseNode(nodeJson, process.nodes.back());
      // validations
      if (node.type == NodeType::Unknown) {
        error = "node \"" + node.id + "\" has an unknown type";
        return false;
      }
      if (node.type == NodeType::InternalTask &&
          (node.internalTaskType == InternalTaskType::Unknown ||
           ((node.internalTaskType == InternalTaskType::Increment ||
             node.internalTaskType == InternalTaskType::SetVariable) &&
            node.variable.empty()) ||
           (node.internalTaskType == InternalTaskType::Service && node.method.empty()))) {
        error = "internal task \"" + node.id + "\" is invalid";
        return false;
      }
      if (nodeIdToIdx.find(node.id) != nodeIdToIdx.end()) {
        error = "duplicate node id \"" + node.id + "\"";
        return false;
      }
      nodeIdToIdx.emplace(node.id, process.nodes.size() - 1);
    }
    // routes
    for (JsonObjectConst routeJson : processJson["routes"].as<JsonArrayConst>()) {
      std::string fromId = routeJson["from"] | "";
      std::string toId = routeJson["to"] | "";
      auto from = nodeIdToIdx.find(fromId);
      auto to = nodeIdToIdx.find(toId);
      if (from == nodeIdToIdx.end() || to == nodeIdToIdx.end()) {
        error = "route " + fromId + " -> " + toId + " names an unknown node";
        return false;
      }
      Route route;
      if (JsonObjectConst condition = routeJson["condition"]; !condition.isNull()) {
        route.hasCondition = true;
        route.condition.variable = condition["variable"] | "";
        // validation
        if (!getCompare(condition["op"] | "", route.condition.op)) {
          error = "route " + fromId + " -> " + toId + " invalid compare op";
          return false;
        }
        // Kept as JSON, as the engine compares it: text stays quoted.
        route.condition.value.clear();
        serializeJson(condition["value"], route.condition.value);  // "null" if missing
      }
      route.from = from->second;
      route.to = to->second;
      process.nodes[from->second].routes.push_back(route);
      process.nodes[to->second].incoming++;  // nodes with a route into it
    }

    // Fork and Join shape checks need the routes, so they come after reading them.
    for (const Node& node : process.nodes) {
      // Fork node - routes validation
      if (node.type == NodeType::Fork) {
        bool conditional = false;
        for (const Route& route : node.routes) {
          conditional = conditional || route.hasCondition;
        }
        if (node.routes.size() < 2 || conditional) {
          error = "fork \"" + node.id + "\" is invalid";
          return false;
        }
      }
      // Join node - routes validation
      if (node.type == NodeType::Join && (node.incoming < 2 || node.routes.size() != 1)) {
        error = "join \"" + node.id + "\" is invalid";
        return false;
      }
    }

    return true;
  }
}  // namespace core
