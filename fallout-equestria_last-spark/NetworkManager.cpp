#include "NetworkManager.h"

#include <cstring>
#include <iostream>

#ifdef _WIN32
// Windows-specific includes already in header
#else
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#endif

NetworkManager& NetworkManager::GetInstance() {
  static NetworkManager instance;
  return instance;
}

NetworkManager::~NetworkManager() { Shutdown(); }

bool NetworkManager::Initialize() {
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    std::cerr << "[!] WSAStartup failed" << std::endl;
    return false;
  }
#else
  // POSIX не требует специальной инициализации
#endif
  std::cout << "[*] Network initialized" << std::endl;
  return true;
}

void NetworkManager::Shutdown() {
  Disconnect();
#ifdef _WIN32
  WSACleanup();
#endif
  std::cout << "[*] Network shutdown" << std::endl;
}

#ifdef _WIN32
void NetworkManager::SetNonBlocking(SOCKET socket) {
  u_long mode = 1;
  ioctlsocket(socket, FIONBIO, &mode);
}
#else
void NetworkManager::SetNonBlocking(SOCKET socket) {
  int flags = fcntl(socket, F_GETFL, 0);
  fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

int NetworkManager::GetLastSocketError() { return errno; }

bool NetworkManager::WouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}
#endif

bool NetworkManager::Connect(const std::string& host, int port) {
  if (is_connected) {
    Disconnect();
  }

  client_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (client_socket == INVALID_SOCKET) {
#ifdef _WIN32
    std::cerr << "[!] Failed to create socket, error: " << WSAGetLastError()
              << std::endl;
#else
    std::cerr << "[!] Failed to create socket, error: " << errno << std::endl;
#endif
    return false;
  }

  // TCP keepalive
  int keepalive = 1;
  setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE,
             reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));

#ifdef __APPLE__
  // macOS specific keepalive
  setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPALIVE, &keepalive,
             sizeof(keepalive));
#elif !defined(_WIN32)
  // Linux/Android keepalive parameters
  int keepidle = 10;
  int keepintvl = 5;
  int keepcnt = 3;
  setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle,
             sizeof(keepidle));
  setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl,
             sizeof(keepintvl));
  setsockopt(client_socket, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt,
             sizeof(keepcnt));
#endif

  SetNonBlocking(client_socket);

  sockaddr_in server_addr{};
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);

#ifdef _WIN32
  if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
#else
  if (inet_aton(host.c_str(), &server_addr.sin_addr) == 0) {
#endif
    std::cerr << "[!] Invalid address: " << host << std::endl;
#ifdef _WIN32
    closesocket(client_socket);
#else
    close(client_socket);
#endif
    client_socket = INVALID_SOCKET;
    return false;
  }

  std::cout << "[*] Connecting to " << host << ":" << port << "..."
            << std::endl;

  int connect_result =
      connect(client_socket, reinterpret_cast<sockaddr*>(&server_addr),
              sizeof(server_addr));

  if (connect_result == SOCKET_ERROR) {
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
    if (errno != EINPROGRESS) {
#endif
      std::cerr << "[!] Connection failed, error: "
#ifdef _WIN32
                << WSAGetLastError() << std::endl;
#else
                << errno << std::endl;
#endif
#ifdef _WIN32
      closesocket(client_socket);
#else
      close(client_socket);
#endif
      client_socket = INVALID_SOCKET;
      return false;
    }

    // ∆дЄм завершени€ неблокирующего connect
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(client_socket, &write_set);

    timeval timeout{5, 0};  // 5 секунд таймаут

    int select_result =
        select(client_socket + 1, nullptr, &write_set, nullptr, &timeout);

    if (select_result <= 0 || !FD_ISSET(client_socket, &write_set)) {
      std::cerr << "[!] Connection timeout" << std::endl;
#ifdef _WIN32
      closesocket(client_socket);
#else
      close(client_socket);
#endif
      client_socket = INVALID_SOCKET;
      return false;
    }
  }

  is_connected = true;
  is_running = true;

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

  if (receive_thread && receive_thread->joinable()) {
    receive_thread->join();
  }
  receive_thread.reset();

  if (heartbeat_thread && heartbeat_thread->joinable()) {
    heartbeat_thread->join();
  }
  heartbeat_thread.reset();

  if (client_socket != INVALID_SOCKET) {
#ifdef _WIN32
    closesocket(client_socket);
#else
    close(client_socket);
#endif
    client_socket = INVALID_SOCKET;
  }

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
#ifdef _WIN32
    std::cerr << "[!] Send failed, error: " << WSAGetLastError() << std::endl;
#else
    std::cerr << "[!] Send failed, error: " << errno << std::endl;
#endif
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

    int select_result =
        select(client_socket + 1, &read_set, nullptr, nullptr, &timeout);

    if (select_result == SOCKET_ERROR) {
#ifdef _WIN32
      std::cerr << "[!] select failed: " << WSAGetLastError() << std::endl;
#else
      std::cerr << "[!] select failed: " << errno << std::endl;
#endif
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
#ifdef _WIN32
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
#else
        int error = errno;
        if (error != EAGAIN && error != EWOULDBLOCK) {
#endif
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
    std::this_thread::sleep_for(std::chrono::seconds(kHeartbeatIntervalSec));

    if (!is_connected || !is_running) break;

    if (Send("HEARTBEAT")) {
      // Heartbeat sent successfully
    }
  }

  std::cout << "[*] Heartbeat thread stopped" << std::endl;
}

void NetworkManager::ProcessReceivedData() {
  std::lock_guard<std::mutex> lock(queue_mutex);

  while (!message_queue.empty()) {
    std::string message = message_queue.front();
    message_queue.pop();

    if (message == "HEARTBEAT" || message.find("HEARTBEAT") == 0) {
      continue;
    }

    if (message_callback) {
      message_callback(message);
    }
  }
}
