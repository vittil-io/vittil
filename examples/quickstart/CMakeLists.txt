cmake_minimum_required(VERSION 3.16)

# The Wi-Fi connection helper from ESP-IDF's own examples (example_connect).
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vittil_quickstart)
