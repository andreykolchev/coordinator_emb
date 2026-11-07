#pragma once

// TCP port the WebSocket server listens on.
#define WS_PORT 81

// The coordinator's own WiFi network (access point). Workers and dashboards
// join it and find the coordinator at 192.168.4.1.
#define AP_SSID "ESP32-Process-Network"
#define AP_PASSWORD "********"  // at least 8 characters
#define AP_CHANNEL 1
// Devices that may join at once; as many as the task dispatcher takes WebSocket clients.
#define AP_MAX_CLIENTS 5

// Port of the dashboard web server.
#define HTTP_PORT 80

// Time a worker has to answer a task started from the dashboard.
#define MANUAL_TASK_TIMEOUT_MS 5000

// ---- Engine limits ----

// Time a worker has to answer a task of the process.
constexpr uint32_t DEFAULT_TASK_TIMEOUT_MS = 5000;

// A node that runs this many times in one execution fails it, so a loop whose
// condition never turns false cannot run forever.
constexpr uint32_t MAX_NODE_RUNS = 1000;

// Steps the engine takes in one tick(); the rest wait for the next tick,
// so a loop of instant nodes cannot block the main loop().
constexpr int STEPS_PER_TICK = 32;

// Executions (parallel branches) one process may have at once;
// a Fork that would exceed it fails the process. Bounds heap use on the device.
constexpr std::size_t MAX_EXECUTIONS = 8;
