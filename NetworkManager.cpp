#include "NetworkManager.h"

#include <cstring>
#include <iostream>

NetworkManager& NetworkManager::GetInstance() {
  static NetworkManager instance;
  return instance;
}

NetworkManager::~NetworkManager() { Shutdown(); }

bool NetworkManager::Initialize() {
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    std::cerr << "[!] WSAStartup failed" << std::endl;
    return false;
  }
  std::cout << "[*] Network initialized" << std::endl;
  return true;
}

void NetworkManager::Shutdown() {
  Disconnect();
  WSACleanup();
  std::cout << "[*] Network shutdown" << std::endl;
}

bool NetworkManager::Connect(const std::string& host, int port) {
  if (is_connected) {
    Disconnect();
  }

  client_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (client_socket == INVALID_SOCKET) {
    std::cerr << "[!] Failed to create socket, error: " << WSAGetLastError()
              << std::endl;
    return false;
  }

  // Простой TCP keepalive (без SIO_KEEPALIVE_VALS)
  int keepalive = 1;
  setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE,
             reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

  if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
    std::cerr << "[!] Invalid address: " << host << std::endl;
    closesocket(client_socket);
    client_socket = INVALID_SOCKET;
    return false;
  }

  std::cout << "[*] Connecting to " << host << ":" << port << "..."
            << std::endl;

  if (connect(client_socket, reinterpret_cast<sockaddr*>(&server_addr),
              sizeof(server_addr)) == SOCKET_ERROR) {
    std::cerr << "[!] Connection failed, error: " << WSAGetLastError()
              << std::endl;
    closesocket(client_socket);
    client_socket = INVALID_SOCKET;
    return false;
  }

  is_connected = true;
  is_running = true;

  // Запуск потоков
  receive_thread =
      std::make_unique<std::thread>(&NetworkManager::ReceiveLoop, this);
  heartbeat_thread =
      std::make_unique<std::thread>(&NetworkManager::HeartbeatLoop, this);

  if (connect_callback) {
    connect_callback();
  }

  std::cout << "[*] Connected to " << host << ":" << port << std::endl;
  return true;
}

void NetworkManager::Disconnect() {
  if (!is_connected) return;

  std::cout << "[*] Disconnecting..." << std::endl;

  is_running = false;
  is_connected = false;

  // Ожидание завершения потоков
  if (receive_thread && receive_thread->joinable()) {
    receive_thread->join();
  }
  receive_thread.reset();

  if (heartbeat_thread && heartbeat_thread->joinable()) {
    heartbeat_thread->join();
  }
  heartbeat_thread.reset();

  if (client_socket != INVALID_SOCKET) {
    closesocket(client_socket);
    client_socket = INVALID_SOCKET;
  }

  // Очистка очереди
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    while (!message_queue.empty()) {
      message_queue.pop();
    }
    receive_buffer.clear();
  }

  if (disconnect_callback) {
    disconnect_callback();
  }

  std::cout << "[*] Disconnected" << std::endl;
}

bool NetworkManager::Send(const std::string& message) {
  if (!is_connected || client_socket == INVALID_SOCKET) {
    std::cerr << "[!] Not connected" << std::endl;
    return false;
  }

  std::string msg_with_newline = message;
  if (msg_with_newline.empty() || msg_with_newline.back() != '\n') {
    msg_with_newline += '\n';
  }

  int result = send(client_socket, msg_with_newline.c_str(),
                    static_cast<int>(msg_with_newline.length()), 0);

  if (result == SOCKET_ERROR) {
    std::cerr << "[!] Send failed, error: " << WSAGetLastError() << std::endl;
    if (disconnect_callback) {
      disconnect_callback();
    }
    Disconnect();
    return false;
  }

  return true;
}

void NetworkManager::Update() { ProcessReceivedData(); }

void NetworkManager::ReceiveLoop() {
  char buffer[kBufferSize];
  std::cout << "[*] Receive thread started" << std::endl;

  while (is_running && is_connected) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(client_socket, &read_set);

    timeval timeout{0, 100000};  // 100 мс

    int select_result = select(0, &read_set, nullptr, nullptr, &timeout);

    if (select_result == SOCKET_ERROR) {
      std::cerr << "[!] select failed: " << WSAGetLastError() << std::endl;
      break;
    }

    if (select_result > 0 && FD_ISSET(client_socket, &read_set)) {
      memset(buffer, 0, kBufferSize);
      int bytes_received = recv(client_socket, buffer, kBufferSize - 1, 0);

      if (bytes_received == 0) {
        std::cout << "[*] Server closed connection" << std::endl;
        break;
      }

      if (bytes_received == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
          std::cerr << "[!] recv failed: " << error << std::endl;
          break;
        }
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        receive_buffer += std::string(buffer, bytes_received);

        size_t pos;
        while ((pos = receive_buffer.find('\n')) != std::string::npos) {
          std::string message = receive_buffer.substr(0, pos);
          receive_buffer.erase(0, pos + 1);

          if (!message.empty() && message_queue.size() < kMaxMessageQueueSize) {
            message_queue.push(message);
          }
        }

        if (receive_buffer.size() > kBufferSize * 10) {
          std::cerr << "[!] Receive buffer overflow, clearing" << std::endl;
          receive_buffer.clear();
        }
      }
    }
  }

  if (is_connected) {
    if (disconnect_callback) {
      disconnect_callback();
    }
    is_connected = false;
    is_running = false;
  }

  std::cout << "[*] Receive thread stopped" << std::endl;
}

void NetworkManager::HeartbeatLoop() {
  std::cout << "[*] Heartbeat thread started (interval: "
            << kHeartbeatIntervalSec << " sec)" << std::endl;

  while (is_running && is_connected) {
    // Ждём интервал
    std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatIntervalSec));

    if (!is_connected || !is_running) break;

    // Отправляем heartbeat ВСЕГДА каждые 25 секунд
    if (Send("HEARTBEAT")) {
      // Ничего не обновляем, просто отправили
    }
  }

  std::cout << "[*] Heartbeat thread stopped" << std::endl;
}

void NetworkManager::ProcessReceivedData() {
  std::lock_guard<std::mutex> lock(queue_mutex);

  while (!message_queue.empty()) {
    std::string message = message_queue.front();
    message_queue.pop();

    // Пропускаем heartbeat сообщения
    if (message == "HEARTBEAT" || message.find("HEARTBEAT") == 0) {
      continue;
    }

    if (message_callback) {
      message_callback(message);
    }
  }
}
