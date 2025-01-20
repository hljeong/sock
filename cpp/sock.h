#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <variant>

// todo: exception handling

namespace sock {

// todo: add logger hook?

constexpr uint32_t operator""_KiB(const unsigned long long kib) {
  return kib * 1024;
}

union Message {
  static constexpr uint32_t MAX_LEN = 128_KiB;

  struct __attribute__((packed)) Header {
    uint32_t len;
  };

  // todo: possible to deduplicate?
  struct __attribute__((packed)) {
    uint32_t len;
  };

  static constexpr uint32_t HEADER_LEN = sizeof(Header);
  static_assert(HEADER_LEN <= MAX_LEN);
  static constexpr uint32_t MAX_DATA_LEN = MAX_LEN - HEADER_LEN;

  struct __attribute__((packed)) {
    Header header;
    uint8_t data[MAX_DATA_LEN];
  };

  struct {
    uint8_t raw[MAX_LEN] = {0};
    uint32_t level = 0;
  };

  uint32_t message_len() const { return HEADER_LEN + len; }

  // return true if a full message is buffered
  bool valid() const { return message_len() <= level; }

  bool advance(uint32_t off) {
    if (off > level) {
      return false;
    }

    // important corner case:
    // the logic below would zero everything out if off == 0
    if (!off) {
      return true;
    }

    for (uint32_t i = 0; i + off < level; ++i) {
      raw[i] = raw[i + off];
      raw[i + off] = 0;
    }
    level -= off;
    return true;
  }

  bool shift() {
    if (!valid()) {
      return false;
    }

    return advance(message_len());
  }

  void clear() {
    // must return true
    advance(level);
  }
};

template <typename Dispatcher> class AutoDispatch {
public:
  AutoDispatch(std::unique_ptr<Dispatcher> dispatcher,
               std::chrono::seconds timeout, std::chrono::milliseconds interval)
      : m_dispatcher(std::move(dispatcher)) {
    using namespace std::chrono;
    using namespace std::this_thread;

    const bool live_forever = timeout == 0s;
    const auto end = steady_clock::now() + timeout;
    m_thread = std::thread([=]() {
      while (!m_signal_stop && (live_forever || steady_clock::now() < end)) {
        m_dispatcher->dispatch();
        sleep_for(interval);
      }
    });
  }

  template <typename Timeout, typename Interval>
  AutoDispatch(std::unique_ptr<Dispatcher> dispatcher, Timeout timeout,
               Interval interval)
      : AutoDispatch(
            dispatcher,
            std::chrono::duration_cast<std::chrono::seconds>(timeout),
            std::chrono::duration_cast<std::chrono::milliseconds>(interval)) {}

  template <typename Interval>
  AutoDispatch(std::unique_ptr<Dispatcher> dispatcher, Interval timeout)
      : AutoDispatch(std::move(dispatcher), timeout,
                     std::chrono::milliseconds(50)) {}

  AutoDispatch(std::unique_ptr<Dispatcher> dispatcher)
      : AutoDispatch(std::move(dispatcher), std::chrono::seconds(0)) {}

  virtual ~AutoDispatch() { stop(); }

  const Dispatcher &operator*() const { return *m_dispatcher; }

  Dispatcher &operator*() { return *m_dispatcher; }

  const Dispatcher *operator->() const { return m_dispatcher.get(); }

  Dispatcher *operator->() { return m_dispatcher.get(); }

  void signal_stop() { m_signal_stop = true; }

  void stop() {
    signal_stop();
    join();
  }

  void join() {
    if (!m_thread.joinable()) {
      return;
    }

    m_thread.join();
  }

private:
  std::unique_ptr<Dispatcher> m_dispatcher;
  std::thread m_thread;
  std::atomic_bool m_signal_stop = false;
};

class TCPServer {
public:
  class Client {
  public:
    Client(int fd) : m_fd(fd) {
      if (m_fd < 0) {
        throw std::runtime_error("invalid client fd: todo");
      }
    }

    virtual ~Client() { ::close(m_fd); }

    void send(const uint8_t *data, uint32_t len) {
      const ssize_t n = ::send(m_fd, data, len, 0);
      // todo: possibly split into different cases:
      //   - n < 0
      //   - n == 0
      //   - n > 0 && n < len
      //   - n > len ???
      if (n != len) {
        throw std::runtime_error("failed to send: todo");
      }
    }

    struct ConnectionClosed {};
    struct Data {
      uint32_t length;
    };
    using RecvResult = std::variant<ConnectionClosed, Data>;

    RecvResult recv(uint8_t *data, uint32_t len) {
      const ssize_t n = ::recv(m_fd, data, len, 0);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return Data{0};
        } else {
          throw std::runtime_error("failed to recv");
        }
      } else if (n == 0) {
        return ConnectionClosed();
      }
      return Data{static_cast<uint32_t>(n)};
    }

  private:
    const int m_fd;
  };

  TCPServer(uint16_t port)
      : m_addr{.sin_family = AF_INET,
               .sin_port = htons(port),
               .sin_addr = {.s_addr = htonl(INADDR_ANY)}},
        m_fd(socket(AF_INET, SOCK_STREAM, 0)) {
    if (m_fd < 0) {
      throw std::runtime_error("failed to open tcp socket: todo errno");
    }

    if (fcntl(m_fd, F_SETFL, O_NONBLOCK) < 0) {
      throw std::runtime_error("failed to set nonblock: todo errno");
    }

    const int option = 1;
    if (::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) <
        0) {
      throw std::runtime_error("failed to set reuseaddr: todo errno");
    }

    if (::bind(m_fd, reinterpret_cast<const sockaddr *>(&m_addr),
               sizeof(m_addr)) < 0) {
      throw std::runtime_error("failed to bind: todo errno");
    }

    if (::listen(m_fd, 1) < 0) {
      throw std::runtime_error("failed to listen: todo errno");
    }
  }

  virtual ~TCPServer() { ::close(m_fd); }

  std::unique_ptr<Client> accept() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    int client_fd = accept4(m_fd, &addr, &addrlen, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return nullptr;
      } else {
        throw std::runtime_error("failed to accept: todo errno");
      }
    }

    if (fcntl(client_fd, F_SETFL, O_NONBLOCK) < 0) {
      throw std::runtime_error("failed to set client nonblock: todo errno");
    }

    return std::make_unique<Client>(client_fd);
  }

private:
  const sockaddr_in m_addr;
  const int m_fd = -1;
};

// todo: move helper out
template <typename... Ts> struct overloads : Ts... {
  using Ts::operator()...;
};

// deduction guide needed pre c++20
template <typename... Ts> overloads(Ts...) -> overloads<Ts...>;

template <typename Server> class CallbackServer {
public:
  CallbackServer(std::unique_ptr<Server> server,
                 const std::function<void(const uint8_t *, uint32_t)> &callback)
      : m_server(std::move(server)), m_callback(callback) {}

  virtual ~CallbackServer() = default;

  void send(const std::vector<uint8_t> &data) { send(&data[0], data.size()); }

  void send(const uint8_t *data, uint32_t len) {
    if (!m_client) {
      throw std::invalid_argument("client not connected");
    }

    if (len > Message::MAX_DATA_LEN) {
      throw std::invalid_argument(
          "message exceeds max size: todo bytes (max todo bytes)");
    }

    m_client->send(reinterpret_cast<uint8_t *>(&len), 4);
    m_client->send(data, len);
  }

  void dispatch() {
    if (!m_client) {
      m_client = m_server->accept();
    } else {
      // desired syntax (std::variant::visit in c++26):
      // m_client->recv(...).visit(match{
      //     [this](typename Server::Client::ConnectionClosed) { ...; },
      //     [this](typename Server::Client::Data [len]) { ...; },
      // });
      std::visit(
          overloads{
              [this](typename Server::Client::ConnectionClosed) {
                m_client.reset();
              },
              [this](typename Server::Client::Data data) {
                auto [len] = data;
                msg.level += len;
                while (msg.valid()) {
                  m_callback(msg.data, msg.len);
                  msg.shift();
                }
              },
          },
          m_client->recv(msg.raw + msg.level, Message::MAX_LEN - msg.level));
    }
  }

private:
  std::unique_ptr<Server> m_server;
  std::unique_ptr<typename Server::Client> m_client;
  std::function<void(const uint8_t *, uint32_t)> m_callback;
  Message msg;
};

} // namespace sock
