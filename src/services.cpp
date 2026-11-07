#include "services.h"

#include <ArduinoJson.h>

namespace services {
  namespace {
    // A method reads `in` and writes its result to `out`; false means it failed (then `out` holds the reason).
    using Method = bool (*)(JsonVariantConst in, JsonDocument& out);

    struct Entry {
      const char* name;
      Method method;
    };

    // Returns the input unchanged.
    bool echo(const JsonVariantConst in, JsonDocument& out) { return out.set(in); }

    // The AC should run when the average temperature is above its limit or the humidity is above its limit.
    // in: {"temps": [number, ...], "humidity": number, "tLim": number, "hLim": number}; out: true or false
    bool calcAc(const JsonVariantConst in, JsonDocument& out) {
      const JsonArrayConst temps = in["temps"];
      // inputs type validation
      if (temps.size() == 0 || !in["humidity"].is<double>() || !in["tLim"].is<double>() || !in["hLim"].is<double>()) {
        out["error"] = "bad_input";
        return false;
      }
      double sum = 0;
      for (const JsonVariantConst temp : temps) {
        if (!temp.is<double>()) {
          out["error"] = "bad_input";
          return false;
        }
        sum += temp.as<double>();
      }
      const double average = sum / temps.size();
      return out.set(average > in["tLim"].as<double>() || in["humidity"].as<double>() > in["hLim"].as<double>());
    }

    // To add a method: write it above and add a row here.
    constexpr Entry methods[] = {
        {"echo", echo},
        {"calc_ac", calcAc},
    };
  }  // namespace

  bool call(const std::string& methodName, const std::string& inputJson, std::string& resultJson) {
    JsonDocument in, out;
    if (deserializeJson(in, inputJson)) {
      resultJson = R"({"error":"bad_input"})";
      return false;
    }
    for (const auto& [name, method] : methods) {
      if (name != methodName) {
        continue;
      }
      const bool ok = method(in.as<JsonVariantConst>(), out);
      resultJson.clear();
      serializeJson(out, resultJson);
      return ok;
    }
    resultJson = R"({"error":"unknown_method"})";
    return false;
  }
}  // namespace services
