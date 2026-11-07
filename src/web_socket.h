#pragma once

#include <WebSocketsServer.h>

#include <cstddef>
#include <string>

namespace web_socket {
  // Clients the server takes at once. A client's number is below this.
  constexpr uint8_t MAX_CLIENTS = WEBSOCKETS_SERVER_CLIENT_MAX;

  // Start listening on WS_PORT. Client events go to task_dispatcher.
  void begin();
  void loop();

  // Whether the client is connected, from its Connected to its Disconnected event.
  bool isConnected(uint8_t client);
  bool sendText(uint8_t client, const std::string& text);
  // Sends to every dashboard: a connected client that is not a worker.
  void broadcast(const std::string& text);
  void disconnect(uint8_t client);
  std::string remoteIP(uint8_t client);
}  // namespace web_socket
