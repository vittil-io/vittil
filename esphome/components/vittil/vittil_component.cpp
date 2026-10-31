#include "vittil_component.h"
#include "esphome/components/logger/logger.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "vittil.h"

namespace esphome {
namespace vittil {

static const char *const TAG = "vittil";

void VittilComponent::setup() {
  // the one public call of the SDK: the identity and the boot facts are read here, the
  // upload and the pings happen on the SDK's own task, which waits for the network itself
  this->started_ = vittil_start();
  if (this->started_ != ESP_OK) {
    ESP_LOGE(TAG, "not started (%s): no crash reports from this boot", esp_err_to_name(this->started_));
    this->status_set_warning();
  }
  // ESPHome's lines never pass ESP-IDF's log hook, where the SDK reads by itself: each
  // line the logger writes is handed to the SDK's log tail, so the crash report carries
  // the last lines before the crash
  if (logger::global_logger != nullptr) {
    logger::global_logger->add_log_callback(
        this, [](void *self, uint8_t level, const char *tag, const char *message, size_t len) {
          static_cast<VittilComponent *>(self)->on_log_(level, tag, message, len);
        });
  }
}

// ESPHome's line is "<colour>[I][tag:123]: body<reset>" (on another task the task's name in its
// own colour between the header and the colon). The dashboard reads the tail in ESP-IDF's shape,
// "I (1234) tag: body", into its columns, so the body is cut out and written in that shape with
// the level's letter and the log clock. Nothing is logged from here (the logger's own guard
// would drop it, and the SDK's call prints nothing).
void VittilComponent::on_log_(uint8_t level, const char *tag, const char *message, size_t len) {
  static const char LETTERS[] = "?EWIIDVV";  // NONE ERROR WARN INFO CONFIG DEBUG VERBOSE VERY_VERBOSE
  if (message == nullptr || len == 0)
    return;
  const char *end = message + len;
  // the body starts after the header's ": " (the tag's own ':' is followed by the line number)
  const char *body = message;
  for (; body + 1 < end; body++) {
    if (body[0] == ':' && body[1] == ' ') {
      body += 2;
      break;
    }
  }
  if (body >= end)
    body = message;
  size_t n = (size_t) (end - body);
  // the trailing colour reset, when there is one
  if (n >= 4 && memcmp(end - 4, "\033[0m", 4) == 0)
    n -= 4;
  while (n > 0 && (body[n - 1] == '\n' || body[n - 1] == '\r'))
    n--;
  char line[160];
  int w = snprintf(line, sizeof(line), "%c (%lu) %s: %.*s", LETTERS[level < sizeof(LETTERS) - 1 ? level : 0],
                   (unsigned long) esp_log_timestamp(), tag != nullptr ? tag : "", (int) n, body);
  if (w <= 0)
    return;
  vittil_log_line(line, (size_t) w < sizeof(line) ? (size_t) w : sizeof(line) - 1);
}

void VittilComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Vittil:");
  ESP_LOGCONFIG(TAG, "  Server: %s", CONFIG_VITTIL_SERVER_URL);
  ESP_LOGCONFIG(TAG, "  Ping interval: %d s", CONFIG_VITTIL_PING_INTERVAL_S);
  ESP_LOGCONFIG(TAG, "  Started: %s", this->started_ == ESP_OK ? "yes" : esp_err_to_name(this->started_));
}

}  // namespace vittil
}  // namespace esphome
