# vittil, the device SDK of Vittil.
#
# The files: vittil.c the task and vittil_start(); vittil_transport.c the
# transport (ESP-IDF's HTTP client, or the application's three functions
# with CONFIG_VITTIL_TRANSPORT_APP, in which case nothing of esp_http_client
# is referenced and the linker leaves it and its TLS out, the requirement
# below notwithstanding: a requirement is a build, not a link); vittil_ping.c the identity, the ping and the retry;
# vittil_upload.c the boot facts, the staged dump streamed from the coredump
# partition, the no-dump event; vittil_record.c the record and the log ring,
# the wrap of the coredump entry; vittil_writer.c the ELF writer with the
# fit pass; vittil_summary.c the crash summary, kept in RTC or noinit memory
# when there is no dump; vittil_qemu_eth.c the emulator's network.
# Requirements: espcoredump and esp_partition for the dump, esp_timer for
# the crash time, xtensa or riscv (IDF_TARGET_ARCH) for the exception frame,
# hal for the chip revision in the dump's header, esp_rom for the panic-time
# prints, bootloader_support for esp_app_format.h, which espcoredump's
# private port header includes.
idf_build_get_property(vittil_arch IDF_TARGET_ARCH)
idf_component_register(SRCS "vittil.c" "vittil_ping.c" "vittil_transport.c"
                            "vittil_upload.c" "vittil_qemu_eth.c"
                            "vittil_record.c" "vittil_writer.c" "vittil_summary.c"
                       INCLUDE_DIRS "include"
                       LDFRAGMENTS linker.lf
                       PRIV_REQUIRES esp_http_client esp_netif esp_event esp_eth
                                     esp_system esp_hw_support esp_app_format
                                     espcoredump esp_partition esp_timer
                                     log mbedtls soc freertos ${vittil_arch} hal esp_rom
                                     bootloader_support)

# The record's fill in front of the stock coredump entry. panic.c
# calls esp_core_dump_write(); the linker sends it to
# __wrap_esp_core_dump_write (vittil_record.c), which fills the record and
# calls the stock entry. Only when the dump goes to flash: panic.c calls
# the entry for a UART dump too, and a wrap without its __wrap_ definition
# is a link error.
if(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_core_dump_write")
else()
    message(WARNING "vittil: CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH is off, so a crash writes no coredump "
                    "(a crash summary is sent instead). Add CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y to sdkconfig.")
endif()

# The crash summary's hook in front of IDF's panic handler. The
# port's panic_handler.c calls esp_panic_handler(); the linker sends it to
# __wrap_esp_panic_handler (vittil_summary.c), which keeps the summary and
# calls the stock handler. Independent of the coredump settings: it is what
# a crash leaves when no dump can be written.
if(CONFIG_VITTIL_SUMMARY)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
endif()

# The emulator transport on esp32c3: Espressif's QEMU maps its
# OpenCores Ethernet on the esp32c3 machine at 0x600CD000 on the Wi-Fi MAC's
# interrupt source (qemu esp-develop, hw/misc/esp32c3_reg.h and
# hw/riscv/esp32c3_intmatrix.h), while ESP-IDF's openeth driver takes both
# from the esp32's own headers, which the esp32c3's lack. Emulator builds
# only (CONFIG_VITTIL_QEMU_ETH); the two values go to the driver and to
# vittil_qemu_eth.c.
if(CONFIG_VITTIL_QEMU_ETH AND CONFIG_IDF_TARGET_ESP32C3)
    idf_component_get_property(vittil_eth_lib esp_eth COMPONENT_LIB)
    set(vittil_qemu_c3 DR_REG_EMAC_BASE=0x600CD000 ETS_ETH_MAC_INTR_SOURCE=ETS_WIFI_MAC_INTR_SOURCE)
    target_compile_definitions(${vittil_eth_lib} PRIVATE ${vittil_qemu_c3})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${vittil_qemu_c3})
endif()

# The writer (vittil_writer.c) replaces the stock ELF writer only. The
# stock panic path calls it through esp_core_dump_store(); the flash layer,
# the checksum and the port stay stock and are reached through espcoredump's
# private headers. On IDF 5.5.0 esp_core_dump_port.h also includes
# esp_core_dump_port_impl.h from include_core_dump/port/<arch>; 5.5.5 has
# no such directory.
if(CONFIG_VITTIL_WRITER)
    idf_component_get_property(vittil_coredump_dir espcoredump COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${vittil_coredump_dir}/include_core_dump")
    if(EXISTS "${vittil_coredump_dir}/include_core_dump/port/${vittil_arch}")
        target_include_directories(${COMPONENT_LIB} PRIVATE "${vittil_coredump_dir}/include_core_dump/port/${vittil_arch}")
    endif()
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_core_dump_store")
endif()

# The ELF upload after the link (CONFIG_VITTIL_ELF_UPLOAD): tools/elf_upload.py
# sends the app's ELF with the key from sdkconfig.json and writes the stamp
# only when the server has it; on any failure it prints nothing and removes
# the stamp, so the next build tries again, and it never fails the build. The
# app target depends on it, so `idf.py flash` runs it as `idf.py build` does.
# Added at the end of the project's CMakeLists, once the ELF's target exists;
# a build without that target or the app target gets no upload. Not in
# emulator builds, whose server is the emulator's host.
if(CONFIG_VITTIL_ELF_UPLOAD AND NOT CONFIG_VITTIL_PROJECT_KEY STREQUAL "" AND NOT CONFIG_VITTIL_QEMU_ETH
   AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.19)
    # the deferred call reads its arguments in the project's scope: they go by a property
    idf_build_get_property(vittil_python PYTHON)
    idf_build_get_property(vittil_sdkconfig_json SDKCONFIG_JSON)
    set_property(GLOBAL PROPERTY VITTIL_ELF_UPLOAD_COMMAND
                 ${vittil_python} ${CMAKE_CURRENT_LIST_DIR}/tools/elf_upload.py)
    set_property(GLOBAL PROPERTY VITTIL_ELF_UPLOAD_SDKCONFIG_JSON ${vittil_sdkconfig_json})
    function(vittil_elf_upload_add)
        get_property(command GLOBAL PROPERTY VITTIL_ELF_UPLOAD_COMMAND)
        get_property(sdkconfig_json GLOBAL PROPERTY VITTIL_ELF_UPLOAD_SDKCONFIG_JSON)
        set(elf ${CMAKE_PROJECT_NAME}.elf)
        if(NOT TARGET ${elf} OR NOT TARGET app)
            return()
        endif()
        set(stamp ${CMAKE_BINARY_DIR}/vittil_elf_upload.stamp)
        add_custom_command(OUTPUT ${stamp}
            COMMAND ${command} $<TARGET_FILE:${elf}> ${sdkconfig_json} ${stamp}
            DEPENDS ${elf}
            COMMENT "vittil: ELF upload"
            VERBATIM)
        add_custom_target(vittil_elf_upload ALL DEPENDS ${stamp})
        add_dependencies(app vittil_elf_upload)
    endfunction()
    cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL vittil_elf_upload_add)
endif()
