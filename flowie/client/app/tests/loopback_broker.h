#pragma once
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <winsock2.h>

// Test-only peer: a single connection, bounded frames, deterministic ACKs and echoes.
class LoopbackBroker {
public:
  explicit LoopbackBroker(bool reject = false, bool silent = false, bool reject_subscribe = false)
      : reject_(reject), silent_(silent), reject_subscribe_(reject_subscribe) {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (listener_ == INVALID_SOCKET ||
        bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ||
        listen(listener_, 1)) {
      if (listener_ != INVALID_SOCKET) closesocket(listener_);
      WSACleanup();
      throw std::runtime_error("Loopback bind/listen failed");
    }
    int size = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size)) {
      closesocket(listener_);
      WSACleanup();
      throw std::runtime_error("getsockname failed");
    }
    port = ntohs(address.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~LoopbackBroker() {
    stop_.store(true);
    thread_.join();
    closesocket(listener_);
    WSACleanup();
  }
  int port = 0;
  std::atomic<int> failure{0};

private:
  bool wait(SOCKET socket) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval interval{0, 100000};
    const int result = select(0, &readable, nullptr, nullptr, &interval);
    if (result == SOCKET_ERROR) failure.store(1);
    return result > 0;
  }
  bool read(SOCKET socket, unsigned char *data, size_t size) {
    while (size && !stop_.load()) {
      if (!wait(socket)) {
        if (failure.load()) return false;
        continue;
      }
      const int count = recv(socket, reinterpret_cast<char *>(data), static_cast<int>(size), 0);
      if (count <= 0) return false;
      data += count;
      size -= static_cast<size_t>(count);
    }
    return size == 0;
  }
  bool send_frame(SOCKET socket, unsigned char header, const std::vector<unsigned char> &body) {
    std::vector<unsigned char> frame{header};
    size_t length = body.size();
    do {
      auto byte = static_cast<unsigned char>(length % 128);
      length /= 128;
      frame.push_back(byte | (length ? 128 : 0));
    } while (length);
    frame.insert(frame.end(), body.begin(), body.end());
    size_t offset = 0;
    while (offset < frame.size()) {
      const int count = send(socket, reinterpret_cast<const char *>(frame.data() + offset),
                             static_cast<int>(frame.size() - offset), 0);
      if (count <= 0) {
        failure.store(2);
        return false;
      }
      offset += count;
    }
    return true;
  }
  void serve(SOCKET socket) {
    bool mqtt5 = true;
    while (!stop_.load()) {
      unsigned char header;
      if (!read(socket, &header, 1)) return;
      size_t size = 0, multiplier = 1;
      unsigned char byte;
      unsigned digits = 0;
      do {
        if (++digits > 4 || !read(socket, &byte, 1)) return;
        size += (byte & 127) * multiplier;
        multiplier *= 128;
      } while (byte & 128);
      if (size > 1024 * 1024) {
        failure.store(3);
        return;
      }
      std::vector<unsigned char> body(size);
      if (!read(socket, body.data(), size)) return;
      const unsigned type = header >> 4;
      if (type == 1) {
        if (size < 7) {
          failure.store(4);
          return;
        }
        mqtt5 = body[6] == 5;
        if (silent_) continue;
        if (!send_frame(
                socket, 0x20,
                mqtt5
                    ? std::vector<unsigned char>{0, static_cast<unsigned char>(reject_ ? 0x87 : 0),
                                                 0}
                    : std::vector<unsigned char>{0, static_cast<unsigned char>(reject_ ? 5 : 0)}))
          return;
      } else if (type == 8 || type == 10) {
        if (size < 2) {
          failure.store(5);
          return;
        }
        std::vector<unsigned char> ack{body[0], body[1]};
        if (mqtt5) ack.push_back(0); // property length
        if (type == 8 || mqtt5) ack.push_back(type == 8 ? (reject_subscribe_ ? 0x80 : 2) : 0);
        if (!send_frame(socket, type == 8 ? 0x90 : 0xb0, ack)) return;
      } else if (type == 3) {
        if (size < 2) {
          failure.store(6);
          return;
        }
        size_t offset = 2 + (static_cast<size_t>(body[0]) << 8) + body[1];
        const unsigned qos = (header >> 1) & 3;
        if (offset + (qos ? 2 : 0) + (mqtt5 ? 1 : 0) > size) {
          failure.store(7);
          return;
        }
        std::vector<unsigned char> echo(body.begin(), body.begin() + offset);
        if (qos) {
          if (!send_frame(socket, qos == 1 ? 0x40 : 0x50, {body[offset], body[offset + 1]})) return;
          offset += 2;
        }
        if (mqtt5) {
          ++offset;
          echo.push_back(0);
        }
        echo.insert(echo.end(), body.begin() + offset, body.end());
        if (!send_frame(socket, static_cast<unsigned char>(0x30 | (header & 1)), echo)) return;
      } else if (type == 6) {
        if (size < 2 || !send_frame(socket, 0x70, {body[0], body[1]})) return;
      } else if (type == 12) {
        if (!send_frame(socket, 0xd0, {})) return;
      } else if (type == 14) return;
      else {
        failure.store(8);
        return;
      }
    }
  }
  void run() noexcept {
    try {
      while (!stop_.load() && !wait(listener_)) {
        if (failure.load()) return;
      }
      if (stop_.load()) return;
      SOCKET client = accept(listener_, nullptr, nullptr);
      if (client == INVALID_SOCKET) {
        failure.store(9);
        return;
      }
      const DWORD timeout = 2000;
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeout),
                 sizeof(timeout));
      try {
        serve(client);
      } catch (...) {
        failure.store(10);
      }
      closesocket(client);
    } catch (...) {
      failure.store(11);
    }
  }
  bool reject_, silent_, reject_subscribe_;
  SOCKET listener_ = INVALID_SOCKET;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};
