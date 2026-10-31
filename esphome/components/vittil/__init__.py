"""
vittil: crash reports from ESPHome devices in the field (https://vittil.io).

One ESPHome component around the vittil ESP-IDF component, for ESPHome on
ESP32 with the esp-idf framework. When the firmware panics, ESP-IDF writes
the coredump to a flash partition; on the next boot the SDK uploads it to
app.vittil.io, where it is decoded against this build's ELF (symbolized
backtrace, the last log lines, the variables), and erases it from flash once
the server has it. Between crashes the device pings every 30 minutes so the
dashboard shows it Live.

  external_components:
    - source: github://vittil-io/vittil
      components: [vittil]

  vittil:
    project_key: !secret vittil_project_key

What this adds to the build:
  - the vittil IDF component (vittil-io/vittil, pinned),
  - the three ESP-IDF components it requires and ESPHome leaves out
    by default (espcoredump, esp_http_client, esp_eth),
  - the sdkconfig lines: the coredump to flash in the ELF format, the
    project key, ESP-IDF's root certificate bundle for the https upload,
    the esphome project version as the firmware version, and
    VITTIL_SUMMARY off,
  - a log listener on ESPHome's logger: each line ESPHome writes goes
    into the SDK's log tail (vittil_log_line), so the crash report
    carries the last lines before the crash. ESPHome's lines never pass
    ESP-IDF's log hook, which is where the SDK reads by itself.
  - a coredump partition (64 KB, coredump_size) appended to the partition table.

What it leaves alone: the panic handling. ESPHome's crash handler sits on
esp_panic_handler, and other components may chain there too. The SDK's
summary hook would be one more wrapper on the same symbol, so it is off
here; the SDK's own hooks are on the coredump's write (esp_core_dump_write,
esp_core_dump_store), which run under the panic handler after theirs.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    add_partition,
    include_builtin_idf_component,
)
from esphome.components.logger import request_log_listener
from esphome.const import CONF_ESPHOME, CONF_ID, CONF_PROJECT, CONF_VERSION
from esphome.core import CORE

CODEOWNERS = ["@vittil-io"]
DEPENDENCIES = ["esp32", "logger"]

CONF_PROJECT_KEY = "project_key"
CONF_COREDUMP_SIZE = "coredump_size"

VITTIL_REPO = "https://github.com/vittil-io/vittil"
VITTIL_REF = "v0.1.5"
# the coredump partition: 64 KB (ESP-IDF's default size for it); the SDK's writer fits the
# dump to it, the crashed task's stack and registers never cut
DEFAULT_COREDUMP_SIZE = 64 * 1024

vittil_ns = cg.esphome_ns.namespace("vittil")
VittilComponent = vittil_ns.class_("VittilComponent", cg.Component)


def _project_key(value):
    value = cv.string_strict(value)
    if not value.startswith("ecr_"):
        raise cv.Invalid("project_key is the key from Settings in the Vittil app: it starts with ecr_")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VittilComponent),
        cv.Required(CONF_PROJECT_KEY): _project_key,
        cv.Optional(CONF_COREDUMP_SIZE, default=DEFAULT_COREDUMP_SIZE): cv.All(
            cv.int_range(min=16 * 1024, max=1024 * 1024), cv.Coerce(int)
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # one slot on the logger's listener list: the component feeds the SDK's log tail
    request_log_listener()

    # the SDK, as an IDF component
    add_idf_component(name="vittil", repo=VITTIL_REPO, ref=VITTIL_REF)
    # what it requires and ESPHome excludes by default (esp32/__init__.py DEFAULT_EXCLUDED_IDF_COMPONENTS)
    for name in ("espcoredump", "esp_http_client", "esp_eth"):
        include_builtin_idf_component(name)

    # the coredump to flash, in the ELF format the server decodes
    add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH", True)
    add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_ENABLE_TO_UART", False)
    add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_ENABLE_TO_NONE", False)
    add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF", True)
    add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_CHECKSUM_CRC32", True)
    # the project key (sdkconfig only; never logged by the SDK)
    add_idf_sdkconfig_option("CONFIG_VITTIL_PROJECT_KEY", config[CONF_PROJECT_KEY])
    # the upload goes to https://app.vittil.io: the SDK verifies the server with
    # ESP-IDF's root certificate bundle, which ESPHome leaves off unless a component asks
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    add_idf_sdkconfig_option("CONFIG_VITTIL_TLS_CERT_BUNDLE", True)
    # no further wrapper on esp_panic_handler: ESPHome's crash handler (and other components)
    # already chain there; the SDK's hooks stay on the coredump's write
    add_idf_sdkconfig_option("CONFIG_VITTIL_SUMMARY", False)

    # the firmware version the dashboard shows is the image's app description; ESPHome
    # leaves it at its own version, so the esphome project's version goes in
    project = CORE.config.get(CONF_ESPHOME, {}).get(CONF_PROJECT)
    if project and project.get(CONF_VERSION):
        add_idf_sdkconfig_option("CONFIG_APP_PROJECT_VER_FROM_CONFIG", True)
        add_idf_sdkconfig_option("CONFIG_APP_PROJECT_VER", str(project[CONF_VERSION]))

    # where the dump is kept until it is uploaded
    add_partition("coredump", "data", "coredump", config[CONF_COREDUMP_SIZE])
