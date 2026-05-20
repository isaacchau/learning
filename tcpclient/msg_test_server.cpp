// ============================================================================
// msg_test_server.cpp — Interactive test server for MsgClient
// ============================================================================
// Generates configurable TCP message streams with dynamic rate controls.
// Used for development, integration testing, and throughput benchmarking.
//
// Usage: ./msg_test_server [--port P] [--msg-size S] [--msg-rate R] [--msg-count N]
//
// Interactive controls (while client connected):
//   'u' - Increase send rate by 10%
//   'd' - Decrease send rate by 10%
//   'o' - Reset to original rate
//   'q' - Quit server
//
// Wire protocol:
//   - Expects a TcpRequest (76 bytes) on connect.
//   - Responds with a stream of TcpResponse + MsgHdr + body messages.
//   - Sequence numbers are monotonically increasing per connection.
// ============================================================================

#include "log_msg.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_shutdown(false);
static std::atomic<int> g_msg_rate(0);        // Current send rate (dynamic)
static std::atomic<int> g_original_rate(0);   // Original rate from args
static std::atomic<bool> g_rate_changed(false); // Flag to notify main loop

static void signalHandler(int) {
  g_shutdown.store(true, std::memory_order_release);
}

// Send all bytes, handling partial sends
static bool sendAll(int fd, const void *data, size_t len) {
  const char *p = static_cast<const char *>(data);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

// RAII guard for terminal non-canonical mode
class TerminalModeGuard {
  struct termios old_tio_;
  bool saved_ = false;
public:
  TerminalModeGuard() {
    if (tcgetattr(STDIN_FILENO, &old_tio_) == 0) {
      saved_ = true;
    }
  }
  void setNonCanonical() {
    if (saved_) {
      struct termios new_tio = old_tio_;
      new_tio.c_lflag &= ~(ICANON | ECHO);
      new_tio.c_cc[VMIN] = 0;
      new_tio.c_cc[VTIME] = 0;
      tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    }
  }
  ~TerminalModeGuard() {
    if (saved_) {
      tcsetattr(STDIN_FILENO, TCSANOW, &old_tio_);
    }
  }
};

// Keyboard input thread for dynamic rate control
static void keyboardInputThread() {
  TerminalModeGuard term_guard;
  term_guard.setNonCanonical();

  char c;
  while (!g_shutdown.load(std::memory_order_relaxed)) {
    if (read(STDIN_FILENO, &c, 1) > 0) {
      int current_rate = g_msg_rate.load(std::memory_order_relaxed);
      int new_rate = current_rate;

      switch (c) {
        case 'u':
        case 'U':
          // Increase by 10%
          if (current_rate > 0) {
            new_rate = static_cast<int>(current_rate * 1.1);
          } else {
            // If currently at max rate (0), set to a reasonable high value
            new_rate = 10000;
          }
          g_msg_rate.store(new_rate, std::memory_order_relaxed);
          g_rate_changed.store(true, std::memory_order_relaxed);
          LOG_INFO("[Server] Rate increased: %d -> %d msgs/s", current_rate, new_rate);
          break;

        case 'd':
        case 'D':
          // Decrease by 10%
          if (current_rate > 0) {
            new_rate = static_cast<int>(current_rate * 0.9);
            if (new_rate < 1) new_rate = 1;
            g_msg_rate.store(new_rate, std::memory_order_relaxed);
            g_rate_changed.store(true, std::memory_order_relaxed);
            LOG_INFO("[Server] Rate decreased: %d -> %d msgs/s", current_rate, new_rate);
          } else {
            LOG_INFO("[Server] Currently at max rate (0), cannot decrease");
          }
          break;

        case 'o':
        case 'O':
          // Reset to original
          new_rate = g_original_rate.load(std::memory_order_relaxed);
          g_msg_rate.store(new_rate, std::memory_order_relaxed);
          g_rate_changed.store(true, std::memory_order_relaxed);
          LOG_INFO("[Server] Rate reset to original: %d msgs/s", new_rate);
          break;

        case 'q':
        case 'Q':
          LOG_INFO("[Server] Quit requested via keyboard");
          g_shutdown.store(true, std::memory_order_release);
          break;

        default:
          // Ignore other keys
          break;
      }
    }

    // Small sleep to prevent busy-waiting
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

static void printUsage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [options]\n"
          "Options:\n"
          "  --port <port>       Listen port      (default: 8888)\n"
          "  --msg-size <bytes>  Body size         (default: 256)\n"
          "  --msg-rate <msgs/s> Send rate, 0=max  (default: 1000)\n"
          "  --msg-count <num>   Total to send, 0=inf (default: 0)\n"
          "  -h, --help          Show this help\n"
          "\n"
          "Interactive controls (while client connected):\n"
          "  'u' - Increase send rate by 10%%\n"
          "  'd' - Decrease send rate by 10%%\n"
          "  'o' - Reset to original rate\n"
          "  'q' - Quit server\n",
          prog);
}

int main(int argc, char *argv[]) {
  // Environment parsing helpers
  auto getEnvInt = [](const char *name, int def) -> int {
    const char *val = std::getenv(name);
    if (val) {
      try { return std::stoi(val); } catch (...) {}
    }
    return def;
  };
  auto getEnvUll = [](const char *name, uint64_t def) -> uint64_t {
    const char *val = std::getenv(name);
    if (val) {
      try { return std::stoull(val); } catch (...) {}
    }
    return def;
  };

  // Level 3 (Defaults) overridden by Level 2 (Environment Variables)
  uint16_t port     = static_cast<uint16_t>(getEnvInt("APP_TCP_SERVER_PORT", 8888));
  size_t   msg_size = static_cast<size_t>(getEnvInt("APP_TCP_SERVER_MSG_SIZE", 256));
  int      msg_rate = getEnvInt("APP_TCP_SERVER_MSG_RATE", 1000);
  uint64_t msg_count= getEnvUll("APP_TCP_SERVER_MSG_COUNT", 0);

  for (int i = 1; i < argc; ++i) {
    if ((strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
      try {
        port = static_cast<uint16_t>(std::stoi(argv[++i]));
      } catch (...) {
        fprintf(stderr, "Error: Invalid port number: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--msg-size") == 0) && i + 1 < argc) {
      try {
        msg_size = static_cast<size_t>(std::stoi(argv[++i]));
      } catch (...) {
        fprintf(stderr, "Error: Invalid message size: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--msg-rate") == 0) && i + 1 < argc) {
      try {
        msg_rate = std::stoi(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid message rate: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--msg-count") == 0) && i + 1 < argc) {
      try {
        msg_count = std::stoull(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid message count: %s\n", argv[i]);
        return 1;
      }
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printUsage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      printUsage(argv[0]);
      return 1;
    }
  }

  // Set global rate variables for dynamic control
  g_msg_rate.store(msg_rate, std::memory_order_relaxed);
  g_original_rate.store(msg_rate, std::memory_order_relaxed);
  
  // Start keyboard input thread for dynamic rate control
  std::thread kb_thread(keyboardInputThread);
  
  LogMsg::getInstance().init(argv[0], nullptr);

  // Signal handling
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signalHandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  // Create listening socket
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    LOG_ERR("socket: %s", strerror(errno));
    return 1;
  }

  int opt = 1;
  if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    LOG_WARN("[Server] Failed to set SO_REUSEADDR: %s", strerror(errno));
  }

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);

  if (::bind(listen_fd, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
    LOG_ERR("bind: %s", strerror(errno));
    ::close(listen_fd);
    return 1;
  }

  if (::listen(listen_fd, 1) < 0) {
    LOG_ERR("listen: %s", strerror(errno));
    ::close(listen_fd);
    return 1;
  }

  // Calculate frame sizes
  size_t total_msg_len = sizeof(TcpResponse) + sizeof(MsgHdr) + msg_size;
  if (total_msg_len > MAX_MSG_LEN) {
    LOG_ERR("Total message length %zu exceeds max %zu", total_msg_len,
            MAX_MSG_LEN);
    ::close(listen_fd);
    return 1;
  }

  LOG_INFO("=== Test Server ===\n"
           "  Port:       %u\n"
           "  Body Size:  %zu bytes\n"
           "  Frame Size: %zu bytes (TcpResponse=%zu + MsgHdr=%zu + body=%zu)\n"
           "  Rate:       %d msgs/s%s\n"
           "  Count:      %s\n"
           "===================\n"
           "Interactive controls: 'u' = +10%%, 'd' = -10%%, 'o' = reset, 'q' = quit\n"
           "Waiting for client connection...",
           port, msg_size, total_msg_len, sizeof(TcpResponse), sizeof(MsgHdr),
           msg_size, msg_rate, msg_rate == 0 ? " (max)" : "",
           msg_count == 0 ? "infinite" : std::to_string(msg_count).c_str());

  while (!g_shutdown.load(std::memory_order_relaxed)) {
    // Wait for client with poll() so we can check g_shutdown
    struct pollfd pfd = {};
    pfd.fd = listen_fd;
    pfd.events = POLLIN;
    int ret = ::poll(&pfd, 1, 500);
    if (ret <= 0)
      continue;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd =
        ::accept(listen_fd, reinterpret_cast<struct sockaddr *>(&client_addr),
                 &client_len);
    if (client_fd < 0) {
      if (errno == EINTR)
        continue;
      LOG_ERR("accept: %s", strerror(errno));
      continue;
    }

    // Disable Nagle for low latency
    int flag = 1;
    if (::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
      LOG_WARN("[Server] Failed to set TCP_NODELAY: %s", strerror(errno));
    }

    // Increase send buffer to match client's receive buffer (2MB default)
    // This prevents the send buffer from becoming a bottleneck during high-throughput testing
    int sndbuf = getEnvInt("APP_TCP_SERVER_SNDBUF", 2097152);  // 2MB default
    if (sndbuf > 0) {
      if (::setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        LOG_WARN("[Server] Failed to set SO_SNDBUF: %s", strerror(errno));
      }
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    LOG_INFO("Client connected from %s:%u", client_ip,
             ntohs(client_addr.sin_port));

    // Read subscription request
    TcpRequest req;
    ssize_t n = ::recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != sizeof(req)) {
      LOG_ERR("Failed to read TcpRequest");
      ::close(client_fd);
      continue;
    }

    if (req.reqKey != getMagicKey()) {
      LOG_ERR("Invalid magic key: 0x%X (expected 0x%X)", req.reqKey, getMagicKey());
      ::close(client_fd);
      continue;
    }

    char item[33] = {};
    std::memcpy(item, req.reqItem, 32);
    char client_id[33] = {};
    std::memcpy(client_id, req.clientID, 32);
    LOG_INFO("Subscription: item=\"%s\", client=\"%s\", lastSeq=%lu", item,
             client_id, req.lastRespSeq);

    // Prepare message template
    std::vector<char> frame(total_msg_len);

    // Fill body with pattern data
    char *body_ptr = frame.data() + sizeof(TcpResponse) + sizeof(MsgHdr);
    for (size_t j = 0; j < msg_size; ++j) {
      body_ptr[j] = static_cast<char>('A' + (j % 26));
    }

    // Send messages
    uint64_t seq = req.lastRespSeq + 1;
    uint64_t sent_count = 0;
    int current_msg_rate = g_msg_rate.load(std::memory_order_relaxed);

    // Rate limiting: interval between messages
    auto interval = (current_msg_rate > 0)
                        ? std::chrono::nanoseconds(1000000000LL / current_msg_rate)
                        : std::chrono::nanoseconds(0);

    auto batch_start = std::chrono::steady_clock::now();

    while (!g_shutdown.load(std::memory_order_relaxed)) {
      // Check if rate was changed via keyboard
      if (g_rate_changed.load(std::memory_order_relaxed)) {
        g_rate_changed.store(false, std::memory_order_relaxed);
        current_msg_rate = g_msg_rate.load(std::memory_order_relaxed);
        interval = (current_msg_rate > 0)
                       ? std::chrono::nanoseconds(1000000000LL / current_msg_rate)
                       : std::chrono::nanoseconds(0);
        // Reset batch timing for smooth transition
        batch_start = std::chrono::steady_clock::now();
        sent_count = 0;
      }
      
      if (msg_count > 0 && sent_count >= msg_count) {
        LOG_INFO("Sent all %lu messages", msg_count);
        break;
      }

      // Build TcpResponse header
      TcpResponse resp;
      resp.respLen = static_cast<uint16_t>(total_msg_len);
      resp.respSeq = seq;
      std::memcpy(frame.data(), &resp, sizeof(TcpResponse));

      // Build MsgHdr
      MsgHdr hdr;
      hdr.msgSeqNum = seq;
      hdr.timestamp = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      hdr.flags = 0;
      std::memcpy(frame.data() + sizeof(TcpResponse), &hdr, sizeof(MsgHdr));

      if (!sendAll(client_fd, frame.data(), total_msg_len)) {
        LOG_ERR("Send failed at seq %lu", seq);
        break;
      }

      ++seq;
      ++sent_count;

      // Rate limiting
      if (current_msg_rate > 0 && interval.count() > 0) {
        auto target = batch_start + interval * sent_count;
        auto now = std::chrono::steady_clock::now();
        if (now < target) {
          std::this_thread::sleep_for(target - now);
        }
      }

      // Periodic stats
      if (sent_count % 10000 == 0) {
        auto now = std::chrono::steady_clock::now();
        double elapsed =
            std::chrono::duration<double>(now - batch_start).count();
        double actual_rate = sent_count / elapsed;
        LOG_INFO("[Server] sent=%lu actual_rate=%.0f msgs/s target_rate=%d msgs/s", 
                 sent_count, actual_rate, current_msg_rate);
      }
    }

    LOG_INFO("Client session ended. Sent %lu messages.", sent_count);
    ::close(client_fd);
  }

  // Signal keyboard thread to stop and wait for it
  g_shutdown.store(true, std::memory_order_release);
  kb_thread.join();
  
  ::close(listen_fd);
  LOG_INFO("Server stopped.");
  LogMsg::getInstance().shutdown();
  return 0;
}
