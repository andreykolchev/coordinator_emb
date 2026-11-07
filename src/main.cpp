// The coordinator: runs the embedded process with the engine, sends its
// external tasks to WebSocket workers, and shows it all on a web dashboard.

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "dashboard.h"
#include "engine.h"
#include "process.h"
#include "process_data.h"
#include "root_page.h"
#include "task_dispatcher.h"
#include "web_socket.h"
#include "worker_registry.h"

static WebServer http(HTTP_PORT);
static task_dispatcher::WsTaskDispatcher dispatcher;
static dashboard::ExecutionView view;
static core::Engine engine(dispatcher, &view);
static core::Process process;

// The BOOT button on the ESP32 dev board.
constexpr uint8_t BUTTON_PIN = 0;
static bool buttonWasPressed = false;

// The onboard LED on most ESP32 dev boards.
constexpr uint8_t LED_PIN = 2;
constexpr uint32_t STARTUP_DELAY_MS = 10000;
constexpr uint32_t LED_BLINK_MS = 250;

// The startup delay runs in loop(), not in setup(), so the servers keep
// serving workers and dashboards while the LED blinks.
static bool starting = false;
static uint32_t startingEndsAt = 0;

// Blink the LED until the startup delay is over, so it is clear the board is running.
static void blinkWhileStarting() {
  if (!starting) {
    return;
  }
  if (static_cast<int32_t>(millis() - startingEndsAt) >= 0) {
    starting = false;
    digitalWrite(LED_PIN, LOW);
    return;
  }
  digitalWrite(LED_PIN, (millis() / LED_BLINK_MS) % 2 ? HIGH : LOW);
}

// Logs the coordinator's state to Serial once per button press.
static void printStatus() {
  Serial.println();
  Serial.printf("WiFi network: %s\n", AP_SSID);
  Serial.printf("Coordinator IP: %s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("Stations connected: %d\n", WiFi.softAPgetStationNum());
  Serial.printf("Process status: %s\n", task_dispatcher::processStatusName());
  worker_registry::printWorkers();

  // The smallest free stack the loop task has had since boot. A value near 0
  // means it is about to overflow; LOOP_STACK_SIZE in platformio.ini sets it.
  Serial.printf("Loop stack: %u bytes free at its lowest\n",
                uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
  Serial.printf("Free heap: %u bytes\n", static_cast<unsigned>(ESP.getFreeHeap()));
}

static void handleButton() {
  const bool pressed = !digitalRead(BUTTON_PIN);
  if (pressed && !buttonWasPressed) {
    printStatus();
  }
  buttonWasPressed = pressed;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Open our own WiFi network for the workers and the dashboards.
  if (WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS)) {
    Serial.printf("WiFi network \"%s\" is up, coordinator at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("Could not start the WiFi network");
  }

  // http entry point
  http.on("/", [] { http.send_P(200, "text/html", INDEX_HTML); });
  http.begin();

  // parce the process from json
  std::string error;
  if (core::parseProcess(PROCESS_JSON, process, error)) {
    dashboard::init(PROCESS_JSON);
    task_dispatcher::init(engine, process);
  } else {
    Serial.printf("Embedded process is invalid: %s\n", error.c_str());
    return;
  }

  pinMode(LED_PIN, OUTPUT);
  starting = true;
  startingEndsAt = millis() + STARTUP_DELAY_MS;
}

// main loop
void loop() {
  http.handleClient();
  web_socket::loop();
  task_dispatcher::loop();
  handleButton();
  blinkWhileStarting();
  engine.tick(millis());
  // Let other tasks (WiFi, idle) run; the loop does not need to spin.
  delay(1);
}
