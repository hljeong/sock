#ifndef SOCK_H
#define SOCK_H

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <list>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <variant>

#include "../lib/cpp_utils/res/res.h"
#include "../lib/cpp_utils/sgr/sgr.h"
#include "../lib/cpp_utils/sum/sum.h"

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

class TCPServer {
public:
  class Client {
  public:
    Client(int fd) : m_fd(fd) {
      if (m_fd < 0) {
        throw std::runtime_error("invalid client fd: todo");
      }
    }

    virtual ~Client() noexcept {
      if (m_fd < 0) {
        return;
      };

      ::close(m_fd);
    }

    Client(Client &&other) noexcept : m_fd(other.m_fd) { other.m_fd = -1; };

    Client &operator=(Client &&other) noexcept {
      if (this != &other) {
        if (m_fd >= 0) {
          ::close(m_fd);
        }

        m_fd = other.m_fd;
        other.m_fd = -1;
      }

      return *this;
    }

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

    enum class RecvError { ConnectionClosed, RecvFailed };

    res::Result<uint32_t, RecvError> recv(uint8_t *data, uint32_t len) {
      const ssize_t n = ::recv(m_fd, data, len, 0);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          return res::Ok<uint32_t>(0);
        } else {
          throw res::Err(RecvError::RecvFailed);
        }
      } else if (n == 0) {
        return res::Err(RecvError::ConnectionClosed);
      }
      return res::Ok<uint32_t>(n);
    }

  private:
    int m_fd;
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

  virtual ~TCPServer() noexcept {
    if (m_fd < 0) {
      return;
    }

    ::close(m_fd);
  }

  TCPServer(TCPServer &&other) noexcept
      : m_addr(other.m_addr), m_fd(other.m_fd) {
    other.m_addr = {};
    other.m_fd = -1;
  };

  TCPServer &operator=(TCPServer &&other) noexcept {
    if (this != &other) {
      if (m_fd >= 0) {
        ::close(m_fd);
      }

      m_addr = other.m_addr;
      m_fd = other.m_fd;
      other.m_addr = {};
      other.m_fd = -1;
    }

    return *this;
  }

  struct NoNewClient {};

  // using AcceptRes = sum::OneOf<Client, NoNewClient>;
  using AcceptRes = sum::OneOf<Client, NoNewClient>;

  enum class AcceptError {
    AcceptFailed,
    SetNonblockFailed,
  };

  // todo: consider Result<Ts..., E> design
  // perhaps with a Result<Oneof<Ts...>, E> Result<Ts..., E>::as_binary()
  res::Result<AcceptRes, AcceptError> accept() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    int client_fd = accept4(m_fd, &addr, &addrlen, SOCK_CLOEXEC);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return res::Ok(AcceptRes(NoNewClient()));
      } else {
        // throw std::runtime_error("failed to accept: todo errno");
        // todo: errno info
        return res::Err(AcceptError::AcceptFailed);
      }
    }

    if (fcntl(client_fd, F_SETFL, O_NONBLOCK) < 0) {
      // throw std::runtime_error("failed to set client nonblock: todo errno");
      // todo: errno info
      return res::Err(AcceptError::SetNonblockFailed);
    }

    return res::Ok(AcceptRes(Client(client_fd)));
  }

private:
  sockaddr_in m_addr;
  int m_fd = -1;
};

template <typename Server> class CallbackServer {
public:
  using Callback = std::function<void(const uint8_t *, uint32_t)>;
  using Client = typename Server::Client;
  using ClientId = uint32_t;

  CallbackServer(Server &&server, Callback callback)
      : m_server(std::move(server)), m_callback(callback) {}

  virtual ~CallbackServer() = default;

  CallbackServer(CallbackServer &&) noexcept = default;

  CallbackServer &operator=(CallbackServer &&) noexcept = default;

  CallbackServer(CallbackServer &&other, Callback callback) noexcept
      : m_server(std::move(other.m_server)), m_callback(callback) {}

  // todo: explore send(ClientId, Data) where Data is a sum type
  void send(ClientId client_id, const std::vector<uint8_t> &data) {
    send(client_id, &data[0], data.size());
  }

  void send(ClientId client_id, const uint8_t *data, uint32_t len) {
    if (!m_clients.count(client_id)) {
      // todo: return a Result
      throw std::invalid_argument("nonexistent client id: [todo|client_id]");
    }

    if (len > Message::MAX_DATA_LEN) {
      // todo: return a Result
      throw std::invalid_argument(
          "message exceeds max size: todo bytes (max todo bytes)");
    }

    Client &client = m_clients.at(client_id);
    client.send(reinterpret_cast<uint8_t *>(&len), 4);
    client.send(data, len);
  }

  void send(const std::vector<uint8_t> &data) { send(&data[0], data.size()); }

  void send(const uint8_t *data, uint32_t len) {
    for (const auto &[client_id, _] : m_clients) {
      send(client_id, data, len);
    }
  }

  void dispatch() {
    // todo: return Result<OneOf<ClientId, NoNewClient>, ...>
    m_server.accept().match_do(
        [&](auto res) {
          res.visit(sgr::overloads{[&](Client &client) {
                                     m_clients.emplace(m_client_id++,
                                                       std::move(client));
                                   },
                                   [&](typename Server::NoNewClient) {
                                     // no-op
                                   }});
        },
        [](auto err) {
          // todo: better syntax than this? (match-like?)
          if (err == Server::AcceptError::AcceptFailed) {
            throw std::runtime_error("failed to accept");
          } else if (err == Server::AcceptError::SetNonblockFailed) {
            throw std::runtime_error("failed to set nonblock");
          } else {
            throw std::runtime_error("unknown accept failure");
          }
        });

    for (auto &[client_id, client] : m_clients) {
      service(client_id, client);
    }
  }

private:
  void service(ClientId client_id, Client &client) {
    client.recv(msg.raw + msg.level, Message::MAX_LEN - msg.level)
        .match_do(
            [&](auto len) {
              msg.level += len;
              while (msg.valid()) {
                m_callback(msg.data, msg.len);
                msg.shift();
              }
            },
            [&](auto err) {
              if (err == Client::RecvError::RecvFailed) {
                throw std::runtime_error("failed to recv");
              } else {
                m_clients.erase(client_id);
              }
            });
  }

  Server m_server;
  ClientId m_client_id = 0;
  std::map<ClientId, Client> m_clients;
  Callback m_callback;
  Message msg;
};

class TCPCallbackServer : public CallbackServer<TCPServer> {
public:
  TCPCallbackServer(uint16_t port, const Callback &callback)
      : CallbackServer<TCPServer>(TCPServer(port), callback) {}

  TCPCallbackServer(TCPCallbackServer &&other, Callback callback) noexcept
      : CallbackServer<TCPServer>(std::move(other), callback) {}
};

} // namespace sock

#endif
