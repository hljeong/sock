#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <optional>
#include <thread>
#include <unistd.h>

namespace sock {
using namespace std::chrono_literals;

constexpr uint32_t operator""_KiB(const unsigned long long kib) {
  return kib * 1024;
}

union Message {
  static constexpr uint32_t MAGIC = 0x94'84'86'95u;
  static constexpr uint32_t MAX_LEN = 4_KiB;

  struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t len;
    uint8_t data[MAX_LEN - 8];
  };

  struct {
    uint8_t raw[MAX_LEN] = {0};
    uint32_t level = 0;
  };

  bool valid() const {
    return magic == MAGIC && len <= MAX_LEN && len <= level;
  }

  bool correlate() {
    for (uint32_t off = 0; off + sizeof(MAGIC) <= level; ++off) {
      if (*reinterpret_cast<uint32_t *>(&raw[off]) == MAGIC) {
        return shift(off);
      }
    }
    return false;
  }

  bool shift(std::optional<const uint32_t> off = std::nullopt) {
    if (!off) {
      if (!valid()) {
        return false;
      }
      uint32_t to_shift = len;
      return shift(to_shift);
    }

    if (!*off) {
      return true;
    }

    for (uint32_t i = 0; i + *off < level; ++i) {
      raw[i] = raw[i + *off];
      raw[i + *off] = 0;
    }
    level -= *off;
    return true;
  }

  bool clear() { return shift(level); }
};

class Server {
public:
  Server(
      const std::function<void(uint8_t *, uint32_t)> &callback =
          [](uint8_t *, uint32_t) {},
      uint16_t port = 3727)
      : m_callback(callback) {
    m_addr.sin_family = AF_INET;
    m_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    m_addr.sin_port = htons(port);
  }

  virtual ~Server() {
    stop();
    disconnect();
    close();
  }

  inline bool open() {
    if (m_socket >= 0) {
      return false;
    }

    if ((m_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      return false;
    }

    const int option = 1;
    if (fcntl(m_socket, F_SETFL, O_NONBLOCK) < 0) {
      close();
      return false;
    }

    if (::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &option,
                     sizeof(option)) < 0) {
      close();
      return false;
    }

    if (::bind(m_socket, reinterpret_cast<sockaddr *>(&m_addr),
               sizeof(m_addr)) < 0) {
      close();
      return false;
    }

    if (::listen(m_socket, 1) < 0) {
      close();
      return false;
    }

    return true;
  }

  inline bool close() {
    if (m_socket < 0) {
      return false;
    }

    if (::close(m_socket) < 0) {
      return false;
    }

    m_socket = -1;
    return true;
  }

  inline bool connect() {
    if (m_client_socket >= 0) {
      return false;
    }

    sockaddr client_addr{};
    socklen_t client_addrlen = sizeof(client_addr);
    if ((m_client_socket = accept(m_socket, &client_addr, &client_addrlen)) <
        0) {
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }

    if (fcntl(m_client_socket, F_SETFL, O_NONBLOCK) < 0) {
      disconnect();
      return false;
    }

    return true;
  }

  inline bool disconnect() {
    if (m_client_socket < 0) {
      return false;
    }

    if (::close(m_client_socket) < 0) {
      return false;
    }
    m_client_socket = -1;

    if (!msg.clear()) {
      return false;
    }

    return true;
  }

  inline bool receive() {
    ssize_t n = ::recv(m_client_socket, msg.raw + msg.level,
                       Message::MAX_LEN - msg.level, 0);
    if (n == 0) {
      return disconnect();
    } else if (n < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }

    msg.level += n;
    msg.correlate();
    while (msg.valid()) {
      m_callback(msg.data, msg.len - 8);
      msg.shift();
    }

    return true;
  }

  inline bool dispatch() {
    if (m_socket < 0) {
      return false;
    }

    if (m_client_socket < 0) {
      connect();
    } else {
      receive();
    }

    return true;
  }

  inline bool start(std::chrono::milliseconds interval = 50ms) {
    if (m_running) {
      return false;
    }

    m_running = true;
    m_thread = std::thread([&]() {
      while (m_running) {
        dispatch();
        std::this_thread::sleep_for(interval);
      }
    });
    return true;
  }

  inline bool stop() {
    if (!m_running) {
      return false;
    }

    if (!m_thread.joinable()) {
      return false;
    }

    m_running = false;
    m_thread.join();
    return true;
  }

private:
  sockaddr_in m_addr{};
  int m_socket = -1;
  int m_client_socket = -1;

  Message msg{};
  std::function<void(uint8_t *, uint32_t)> m_callback;

  std::thread m_thread{};
  std::atomic_bool m_running = false;
};
} // namespace sock

int main() {
  sock::Server s([](uint8_t *data, uint32_t len) {
    printf("len = %u\n", len);
    printf("data = ");
    for (uint32_t i = 0; i < len; ++i) {
      if (i) {
        printf(" ");
      }
      printf("%02x", data[i]);
    }
    printf("\n");
  });
  s.open();
  s.start();
  std::this_thread::sleep_for(std::chrono::seconds(15));
  s.stop();
  s.close();
}
