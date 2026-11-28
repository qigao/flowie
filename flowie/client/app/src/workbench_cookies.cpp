#include "workbench_cookies.h"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <datetime_parser.h>
#include <limits>
#include <optional>
#include <stdexcept>

namespace flowie::app {
  bool valid_http_cookie_name(std::string_view name) {
    constexpr std::string_view punctuation("!#$%&'*+-.^`_|~");
    return !name.empty() && std::all_of(name.begin(), name.end(), [&](unsigned char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
             punctuation.find(c) != punctuation.npos;
    });
  }

  namespace {
    constexpr size_t kCookieCount = 32;
    constexpr auto kSessionExpiry = std::numeric_limits<int64_t>::max();

    std::string_view trim(std::string_view text) {
      const auto first = text.find_first_not_of(" \t");
      if (first == text.npos) return {};
      return text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }
    std::string_view take(std::string_view &text, char separator) {
      const auto end = text.find(separator);
      const auto head = text.substr(0, end);
      text = end == text.npos ? std::string_view{} : text.substr(end + 1);
      return head;
    }
    char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : c; }
    bool equal_ascii(std::string_view a, std::string_view b) {
      return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return lower(x) == lower(y);
             });
    }
    bool cookie_value(std::string_view value) {
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        value = value.substr(1, value.size() - 2);
      return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e && c != '"' && c != ',' && c != ';' && c != '\\';
      });
    }
    bool path_matches(std::string_view cookie, std::string_view request) {
      return cookie == request ||
             (request.size() > cookie.size() && request.substr(0, cookie.size()) == cookie &&
              (cookie.back() == '/' || request[cookie.size()] == '/'));
    }
    std::optional<int64_t> age_expiry(std::string_view value, int64_t now) {
      if (value.empty()) return {};
      int64_t age = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), age);
      if (parsed.ptr != value.data() + value.size() || parsed.ec == std::errc::invalid_argument)
        return {};
      if (parsed.ec == std::errc::result_out_of_range)
        return value.front() == '-' ? int64_t{0} : kSessionExpiry;
      if (age <= 0) return 0;
      return now > kSessionExpiry - age ? kSessionExpiry : now + age;
    }
  } // namespace

  void HttpCookies::expire(int64_t now) {
    cookies_.erase(std::remove_if(cookies_.begin(), cookies_.end(),
                                  [&](const auto &cookie) { return cookie.expires <= now; }),
                   cookies_.end());
  }

  bool HttpCookies::store(std::string_view origin, std::string_view host, std::string_view path,
                          bool tls, std::string_view header) {
    if (header.size() > header_limit) return false;
    auto pair = take(header, ';');
    if (pair.find('=') == pair.npos) return true;
    const auto name = trim(take(pair, '='));
    const auto value = trim(pair);
    if (!valid_http_cookie_name(name) || !cookie_value(value)) return true;

    const auto directory = path.rfind('/');
    auto scope = directory == 0 || directory == path.npos ? std::string_view("/")
                                                          : path.substr(0, directory);
    const auto now = static_cast<int64_t>(std::time(nullptr));
    std::optional<int64_t> max_age;
    int64_t expires = kSessionExpiry;
    bool secure = false, domain = false, root_path = false;
    while (!header.empty()) {
      auto attribute = take(header, ';');
      const auto key = trim(take(attribute, '='));
      auto argument = trim(attribute);
      if (equal_ascii(key, "Secure")) secure = true;
      else if (equal_ascii(key, "Domain")) {
        if (!argument.empty() && argument.front() == '.') argument.remove_prefix(1);
        if (!equal_ascii(argument, host)) return true;
        domain = true;
      } else if (equal_ascii(key, "Path")) {
        if (!argument.empty() && argument.front() == '/') {
          scope = argument;
          root_path = scope == "/";
        }
      } else if (equal_ascii(key, "Max-Age")) {
        if (const auto expiry = age_expiry(argument, now)) max_age = expiry;
      } else if (equal_ascii(key, "Expires")) {
        datetime_t date{};
        if (!argument.empty() && datetime_parse(argument.data(), argument.size(), &date) == 0) {
          const auto timestamp = datetime_to_time(&date);
          if (date.year < 1970 || timestamp != static_cast<time_t>(-1))
            expires = date.year < 1970 ? 0 : static_cast<int64_t>(timestamp);
        }
      }
    }
    const bool host_prefix = name.substr(0, 7) == "__Host-";
    if ((secure && !tls) || (host_prefix && (!secure || domain || !root_path)) ||
        (name.substr(0, 9) == "__Secure-" && !secure))
      return true;
    expire(now);
    const auto found = std::find_if(cookies_.begin(), cookies_.end(), [&](const auto &cookie) {
      return cookie.origin == origin && cookie.path == scope && cookie.name == name;
    });
    expires = max_age.value_or(expires);
    if (expires <= now) {
      if (found != cookies_.end()) cookies_.erase(found);
      return true;
    }
    if (found != cookies_.end()) {
      found->value.assign(value);
      found->expires = expires;
      return true;
    }
    if (cookies_.size() == kCookieCount) return false;
    cookies_.push_back(
        {std::string(origin), std::string(scope), std::string(name), std::string(value), expires});
    return true;
  }

  std::string HttpCookies::header(std::string_view origin, std::string_view path) {
    expire(static_cast<int64_t>(std::time(nullptr)));
    std::vector<const Cookie *> matches;
    for (const auto &cookie : cookies_)
      if (cookie.origin == origin && path_matches(cookie.path, path)) matches.push_back(&cookie);
    std::stable_sort(matches.begin(), matches.end(),
                     [](const auto *a, const auto *b) { return a->path.size() > b->path.size(); });
    std::string result;
    for (const auto *cookie : matches) {
      const auto size = cookie->name.size() + 1 + cookie->value.size() + (result.empty() ? 0 : 2);
      if (size > header_limit - result.size())
        throw std::runtime_error("The HTTP session Cookie header exceeds 4096 bytes.");
      if (!result.empty()) result += "; ";
      result += cookie->name;
      result += '=';
      result += cookie->value;
    }
    return result;
  }

  std::optional<std::string> HttpCookies::value(std::string_view origin, std::string_view path,
                                                std::string_view name) {
    expire(static_cast<int64_t>(std::time(nullptr)));
    const Cookie *match = nullptr;
    for (const auto &cookie : cookies_) {
      if (cookie.origin == origin && cookie.name == name && path_matches(cookie.path, path) &&
          (!match || cookie.path.size() > match->path.size()))
        match = &cookie;
    }
    if (!match) return std::nullopt;
    return match->value;
  }
} // namespace flowie::app
