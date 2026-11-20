#pragma once

namespace flowie::app {
  enum class RequestAction { open, send, stop, sample, validate_json, format_json, refresh_ports };

  struct RequestCommand {
    const wchar_t *label = nullptr;
    bool enabled = false;
  };
} // namespace flowie::app
