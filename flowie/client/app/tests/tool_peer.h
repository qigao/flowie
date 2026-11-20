#pragma once
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <winsock2.h>

// One bounded loopback peer for the native Workbench tool tests.
class ToolPeer {
public:
  enum class Mode { echo, http, silent_http };
  explicit ToolPeer(Mode mode, std::string response = {})
      : mode_(mode), response_(std::move(response)) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
    try {
      listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      int length = sizeof(address);
      if (listener_ == INVALID_SOCKET ||
          bind(listener_, reinterpret_cast<const sockaddr *>(&address), length) ||
          listen(listener_, 1) ||
          getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &length))
        throw std::runtime_error("Could not bind test loopback");
      port = ntohs(address.sin_port);
      thread_ = std::thread([this] { serve(); });
    } catch (...) {
      if (listener_ != INVALID_SOCKET) closesocket(listener_);
      WSACleanup();
      throw;
    }
  }
  ~ToolPeer() {
    stop_ = true;
    thread_.join();
    closesocket(listener_);
    WSACleanup();
  }
  int port = 0;
  std::atomic<bool> received{false}, disconnected{false};
  std::string request() {
    std::lock_guard guard(lock_);
    return request_;
  }

private:
  static bool readable(SOCKET handle) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(handle, &set);
    timeval timeout{0, 25000};
    return select(0, &set, nullptr, nullptr, &timeout) == 1;
  }
  static void write(SOCKET handle, const char *data, int size) {
    while (size > 0) {
      const auto written = send(handle, data, size, 0);
      if (written <= 0) return;
      data += written;
      size -= written;
    }
  }
  void serve() noexcept {
    while (!stop_ && !readable(listener_)) {
    }
    if (stop_) return;
    const auto peer = accept(listener_, nullptr, nullptr);
    if (peer == INVALID_SOCKET) return;
    try {
      char bytes[4096];
      std::string input;
      while (!stop_) {
        if (!readable(peer)) continue;
        const int count = recv(peer, bytes, sizeof(bytes), 0);
        if (count <= 0) {
          disconnected = true;
          break;
        }
        if (input.size() + count > 128 * 1024) break;
        input.append(bytes, count);
        if (mode_ == Mode::echo) {
          {
            std::lock_guard guard(lock_);
            request_ = input;
          }
          received = true;
          write(peer, bytes, count);
          continue;
        }
        const auto end = input.find("\r\n\r\n");
        if (end == input.npos) continue;
        auto lower = input.substr(0, end);
        for (auto &ch : lower)
          if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        constexpr std::string_view header = "\r\ncontent-length:";
        const auto field = lower.find(header);
        const auto length =
            field == lower.npos ? 0 : std::stoul(lower.substr(field + header.size()));
        if (input.size() < end + 4 + length) continue;
        {
          std::lock_guard guard(lock_);
          request_ = input;
        }
        received = true;
        if (mode_ == Mode::silent_http) continue;
        const auto output = "HTTP/1.1 200 OK\r\nConnection: close\r\nX-Fixture: loopback\r\n"
                            "Content-Length: " +
                            std::to_string(response_.size()) + "\r\n\r\n" + response_;
        write(peer, output.data(), static_cast<int>(output.size()));
        break;
      }
    } catch (...) {
    }
    closesocket(peer);
  }
  Mode mode_;
  std::string response_;
  SOCKET listener_ = INVALID_SOCKET;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex lock_;
  std::string request_;
};
