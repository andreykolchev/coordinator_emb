#pragma once

#include <string>

namespace services {
  // Run the method `name` on `inputJson`. Returns false if it is unknown or fails.
  // `resultJson` is the method's result, or {"error": "..."} on failure.
  bool call(const std::string& methodName, const std::string& inputJson, std::string& resultJson);
}  // namespace services
