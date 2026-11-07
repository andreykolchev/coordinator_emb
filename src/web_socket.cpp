#include "web_socket.h"

#include <Arduino.h>

#include "config.h"
#include "dashboard.h"
#include "task_dispatcher.h"
#include "worker_registry.h"

namespace web_socket {
  namespace {
    WebSocketsServer server_(WS_PORT);

    // Indexed by client number.
    bool connected_[MAX_CLIENTS];

    void handleEvent(const uint8_t client, const WStype_t type, uint8_t* payload, const size_t length) {
      if (type == WStype_CONNECTED) {
        connected_[client] = true;
        dashboard::onConnected(client);
        task_dispatcher::onConnected(client);
      } else if (type == WStype_TEXT) {
        task_dispatcher::onText(client, payload, length);
      } else if (type == WStype_DISCONNECTED) {
        connected_[client] = false;
        task_dispatcher::onDisconnected(client);
      }
    }
  }  // namespace

  void begin() {
    server_.begin();
    server_.onEvent(handleEvent);
    // Drop half-open connections (e.g. a worker lost power): ping every 15 s,
    // disconnect after 2 missed pongs.
    server_.enableHeartbeat(15000, 3000, 2);
  }

  void loop() { server_.loop(); }

  bool isConnected(const uint8_t client) { return connected_[client]; }

  bool sendText(const uint8_t client, const std::string& text) {
    return server_.sendTXT(client, text.c_str(), text.length());
  }

  // send text only for dashboard update
  void broadcast(const std::string& text) {
    for (uint8_t client = 0; client < MAX_CLIENTS; client++) {
      // A connected client that has not registered as a worker is a dashboard.
      if (isConnected(client) && !worker_registry::isWorker(client)) {
        sendText(client, text);
      }
    }
  }

  void disconnect(const uint8_t client) { server_.disconnect(client); }

  std::string remoteIP(const uint8_t client) { return server_.remoteIP(client).toString().c_str(); }
}  // namespace web_socket
