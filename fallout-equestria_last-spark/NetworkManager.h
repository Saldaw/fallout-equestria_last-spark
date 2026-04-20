#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
using SOCKET = int;
#endif

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

class NetworkManager {
 public:
  using MessageCallback = std::function<void(const std::string&)>;
  using ConnectCallback = std::function<void()>;
  using DisconnectCallback = std::function<void()>;

  static NetworkManager& GetInstance();

  NetworkManager(const NetworkManager&) = delete;
  NetworkManager& operator=(const NetworkManager&) = delete;

  bool Initialize();
  void Shutdown();

  bool Connect(const std::string& host, int port);
  void Disconnect();

  bool Send(const std::string& message);
  void Update();

  bool IsConnected() const { return is_connected; }

  void SetMessageCallback(MessageCallback callback) {
    message_callback = callback;
  }
  void SetConnectCallback(ConnectCallback callback) {
    connect_callback = callback;
  }
  void SetDisconnectCallback(DisconnectCallback callback) {
    disconnect_callback = callback;
  }

 private:
  NetworkManager() = default;
  ~NetworkManager();

  void ReceiveLoop();
  void HeartbeatLoop();
  void ProcessReceivedData();

#ifdef _WIN32
  void SetNonBlocking(SOCKET socket);
#else
  void SetNonBlocking(SOCKET socket);
  int GetLastSocketError();
  bool WouldBlock();
#endif

  SOCKET client_socket = INVALID_SOCKET;
  std::atomic<bool> is_connected{false};
  std::atomic<bool> is_running{false};

  std::unique_ptr<std::thread> receive_thread;
  std::unique_ptr<std::thread> heartbeat_thread;

  std::string receive_buffer;
  std::queue<std::string> message_queue;
  std::mutex queue_mutex;

  MessageCallback message_callback = nullptr;
  ConnectCallback connect_callback = nullptr;
  DisconnectCallback disconnect_callback = nullptr;

  static constexpr int kHeartbeatIntervalSec = 25;
  static constexpr int kBufferSize = 4096;
  static constexpr int kMaxMessageQueueSize = 1000;
};
