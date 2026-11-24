#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace flowie::app {
  class HttpCookies {
  public:
    static constexpr size_t header_limit = 4096;

    // A tab admits one HTTP request at a time. The worker owns this jar until it is joined.
    bool store(std::string_view origin, std::string_view host, std::string_view path, bool tls,
               std::string_view header);
    std::string header(std::string_view origin, std::string_view path);
    void clear() noexcept { cookies_.clear(); }

  private:
    struct Cookie {
      std::string origin, path, name, value;
      int64_t expires;
    };
    void expire(int64_t now);
    std::vector<Cookie> cookies_;
  };
} // namespace flowie::app
