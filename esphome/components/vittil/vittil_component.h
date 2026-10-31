#pragma once

#include "esp_err.h"
#include "esphome/core/component.h"

namespace esphome {
namespace vittil {

/// Starts the vittil SDK once the device is up: it reports the last boot's
/// crash (the coredump kept in flash) and pings from then on, on its own
/// task; nothing here runs in the loop. Listens on ESPHome's logger so the
/// crash report carries the last lines before the crash.
class VittilComponent : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  /// One line ESPHome's logger wrote, into the SDK's log tail in ESP-IDF's shape.
  void on_log_(uint8_t level, const char *tag, const char *message, size_t len);
  esp_err_t started_{ESP_FAIL};
};

}  // namespace vittil
}  // namespace esphome
