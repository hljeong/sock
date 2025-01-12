#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// todo: exception handling

namespace sock {

// todo: add logger hook?

constexpr uint32_t operator""_KiB(const unsigned long long kib) {
  return kib * 1024;
}

union Message {
  static constexpr uint32_t MAGIC = 0x94'84'86'95u;
  static constexpr uint32_t MAX_LEN = 8_KiB;

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
      const std::function<void(const uint8_t *, uint32_t)> &callback =
          [](auto, auto) {},
      uint16_t port = 3727, bool close_on_empty = true)
      : m_callback(callback), m_close_on_empty(close_on_empty) {
    m_addr.sin_family = AF_INET;
    m_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    m_addr.sin_port = htons(port);

    open();
  }

  virtual ~Server() {
    disconnect();
    stop();
    close();
  }

  bool open() {
    if (m_socket >= 0) {
      return false;
    }

    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
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

  bool close() {
    if (m_socket < 0) {
      return false;
    }

    if (::close(m_socket) < 0) {
      return false;
    }

    m_socket = -1;
    return true;
  }

  bool connect() {
    if (m_client_socket >= 0) {
      return false;
    }

    sockaddr client_addr{};
    socklen_t client_addrlen = sizeof(client_addr);
    m_client_socket =
        accept4(m_socket, &client_addr, &client_addrlen, SOCK_CLOEXEC);
    if (m_client_socket < 0) {
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }

    if (fcntl(m_client_socket, F_SETFL, O_NONBLOCK) < 0) {
      disconnect();
      return false;
    }

    return true;
  }

  bool disconnect() {
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

  bool send(const std::vector<uint8_t> &data) {
    return send(&data[0], data.size());
  }

  bool send(const uint8_t *data, uint32_t len) {
    if (m_socket < 0) {
      return false;
    }

    if (m_client_socket < 0) {
      return false;
    }

    if (len > Message::MAX_LEN - 8) {
      return false;
    }

    if (!send_raw(Message::MAGIC)) {
      return false;
    }

    if (!send_raw(len + 8)) {
      return false;
    }

    if (!send_raw(data, len)) {
      return false;
    }

    return true;
  }

  bool receive() {
    if (m_socket < 0) {
      return false;
    }

    if (m_client_socket < 0) {
      return false;
    }

    const ssize_t n = ::recv(m_client_socket, msg.raw + msg.level,
                             Message::MAX_LEN - msg.level, 0);
    if (n == 0) {
      return disconnect();
    } else if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      } else {
        return false;
      }
    }

    msg.level += n;
    msg.correlate();
    while (msg.valid()) {
      // todo: remove magic number
      if (msg.len == 8 && m_close_on_empty) {
        m_stop_signaled = true;
        return true;
      }

      m_callback(msg.data, msg.len - 8);
      msg.shift();
    }

    return true;
  }

  bool dispatch() {
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

  template <typename T, typename U> bool start(T timeout, U interval) {
    using namespace std::chrono;
    return start(duration_cast<seconds>(timeout),
                 duration_cast<milliseconds>(interval));
  }

  // template parameters cannot be deduced from default arguments,
  // i.e., template <typename T, typename U> bool start(T timeout = 0s, U
  // interval = 50ms) is not possible
  // manually overloading to achieve the same effect here
  // see: https://stackoverflow.com/a/18981056
  template <typename T> bool start(T timeout) {
    using namespace std::chrono;
    return start(duration_cast<seconds>(timeout), 50ms);
  }

  bool start() {
    using namespace std::chrono;
    return start(0s);
  }

  bool stop() {
    if (!m_running) {
      return false;
    }

    m_stop_signaled = true;
    wait_for_stop();
    return true;
  }

  bool running() const { return m_running; }

  bool stopped() const { return !m_running; }

  template <typename T, typename U> void wait_for_stop(T timeout, U interval) {
    using namespace std::chrono;
    return wait_for_stop(duration_cast<seconds>(timeout),
                         duration_cast<milliseconds>(interval));
  }

  template <typename T> void wait_for_stop(T timeout) {
    using namespace std::chrono;
    wait_for_stop(duration_cast<seconds>(timeout), 50ms);
  }

  void wait_for_stop() {
    using namespace std::chrono;
    wait_for_stop(0s);
  }

private:
  template <typename T> bool send_raw(const T &data) {
    return send_raw(reinterpret_cast<const uint8_t *>(&data), sizeof(T));
  }

  // clean up after server stopped
  bool reset() {
    if (m_running) {
      return false;
    }

    if (!m_thread.joinable()) {
      return false;
    }

    m_stop_signaled = false;
    m_thread.join();
    return true;
  }

  bool send_raw(const uint8_t *data, uint32_t len) {
    // todo: are these checks needed in private member functions?
    if (m_socket < 0) {
      return false;
    }

    if (m_client_socket < 0) {
      return false;
    }

    const ssize_t n = ::send(m_client_socket, data, len, 0);
    // todo: possibly split into different cases:
    //   - n < 0
    //   - n == 0
    //   - n > 0 && n < len
    //   - n > len ???
    if (n != len) {
      disconnect();
      return false;
    }

    return true;
  }

  sockaddr_in m_addr{};
  int m_socket = -1;
  int m_client_socket = -1;

  Message msg{};
  std::function<void(const uint8_t *, uint32_t)> m_callback;
  bool m_close_on_empty;

  std::thread m_thread{};
  std::atomic_bool m_stop_signaled = false;
  std::atomic_bool m_running = false;
};

template <>
inline bool Server::start<std::chrono::seconds, std::chrono::milliseconds>(
    std::chrono::seconds timeout, std::chrono::milliseconds interval) {
  using namespace std::chrono;
  using namespace std::this_thread;

  if (m_running) {
    return false;
  }

  m_running = true;
  const bool live_forever = timeout == 0s;
  const auto end = steady_clock::now() + timeout;
  m_thread = std::thread([=]() {
    while (!m_stop_signaled && (live_forever || steady_clock::now() < end)) {
      dispatch();
      sleep_for(interval);
    }
    m_running = false;
  });
  return true;
}

template <>
inline void
Server::wait_for_stop<std::chrono::seconds, std::chrono::milliseconds>(
    std::chrono::seconds timeout, std::chrono::milliseconds interval) {
  using namespace std::chrono;
  using namespace std::this_thread;

  if (!m_running) {
    return;
  }

  // todo: this timeout logic is duplicated, possible to extract?
  const bool wait_forever = timeout == 0s;
  const auto end = steady_clock::now();
  while (m_running && (wait_forever || steady_clock::now() < end)) {
    sleep_for(interval);
  }

  reset();
}

} // namespace sock
