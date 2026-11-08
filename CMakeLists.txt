cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(FoloToy-AI-Passport)

# INSERT augmentation must precede the SDK's complete section script. SQLite's
# large initialized tables otherwise push SDK globals out of RISC-V's GP window.
set(feishu_sqlite_layout "${CMAKE_CURRENT_SOURCE_DIR}/components/feishu_sqlite/sqlite_data.ld")
target_link_options(${CMAKE_PROJECT_NAME}.elf BEFORE PRIVATE "SHELL:-T ${feishu_sqlite_layout}")
set_property(TARGET ${CMAKE_PROJECT_NAME}.elf APPEND PROPERTY LINK_DEPENDS "${feishu_sqlite_layout}")

# Preserve the reference SDK radio build accepted by PassportSim. Optimize the
# large GUI component independently rather than changing the SDK-wide ABI profile.
idf_component_get_property(feishu_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${feishu_lvgl_lib} PRIVATE -Os)

# LVGL 9.5's glyph scratch failure asserts forever by default. Generate two
# guarded sources without modifying the managed dependency or disabling asserts.
idf_component_get_property(feishu_lvgl_dir lvgl__lvgl COMPONENT_DIR)
get_target_property(feishu_lvgl_sources ${feishu_lvgl_lib} SOURCES)
foreach(feishu_name lv_draw_label.c lv_draw_buf.c)
    set(feishu_original "${feishu_lvgl_dir}/src/draw/${feishu_name}")
    set(feishu_patched "${CMAKE_BINARY_DIR}/feishu-lvgl-recovery/${feishu_name}")
    execute_process(COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_lvgl_glyph_recovery.py"
                    "${feishu_original}" "${feishu_patched}" COMMAND_ERROR_IS_FATAL ANY)
    list(FILTER feishu_lvgl_sources EXCLUDE REGEX "(^|/)${feishu_name}$")
    list(APPEND feishu_lvgl_sources "${feishu_patched}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${feishu_original}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_lvgl_glyph_recovery.py")
endforeach()
set_property(TARGET ${feishu_lvgl_lib} PROPERTY SOURCES "${feishu_lvgl_sources}")
target_include_directories(${feishu_lvgl_lib} PRIVATE "${feishu_lvgl_dir}/src/draw")
if("$ENV{FEISHU_LVGL_RECOVERY_AUTOTEST}" STREQUAL "1")
    target_compile_definitions(${feishu_lvgl_lib} PRIVATE FEISHU_LVGL_RECOVERY_AUTOTEST=1)
endif()


# Read-only font assets use spare Flash without moving app, identity or data.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/feishu-14-4bpp.bin"
               "${CMAKE_BINARY_DIR}/feishu-font.bin" COPYONLY)
esptool_py_flash_to_partition(flash feishu_font "${CMAKE_BINARY_DIR}/feishu-font.bin")

# Source/hash-guarded ESP-IDF async select recovery; shared SDK stays untouched.
idf_component_get_property(feishu_tls_lib esp-tls COMPONENT_LIB)
idf_component_get_property(feishu_tls_dir esp-tls COMPONENT_DIR)
get_target_property(feishu_tls_sources ${feishu_tls_lib} SOURCES)
set(feishu_tls_original "${feishu_tls_dir}/esp_tls.c")
set(feishu_tls_patched "${CMAKE_BINARY_DIR}/feishu-tls-recovery/esp_tls.c")
execute_process(COMMAND ${PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_esp_tls_async_poll.py"
    "${feishu_tls_original}" "${feishu_tls_patched}" COMMAND_ERROR_IS_FATAL ANY)
list(FILTER feishu_tls_sources EXCLUDE REGEX "(^|/)esp_tls[.]c$")
list(APPEND feishu_tls_sources "${feishu_tls_patched}")
set_property(TARGET ${feishu_tls_lib} PROPERTY SOURCES "${feishu_tls_sources}")
target_include_directories(${feishu_tls_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tools/patches/include")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${feishu_tls_original}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_esp_tls_async_poll.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/patches/include/feishu_tls_poll.h")
if("$ENV{FEISHU_ASSISTANT_AUTOTEST}" STREQUAL "4")
    target_compile_definitions(${feishu_tls_lib} PRIVATE FEISHU_TLS_POLL_AUTOTEST=1)
endif()
if(DEFINED ENV{PASSPORT_RETAIN_GATE_BUILD})
    configure_file("${feishu_tls_patched}" "$ENV{PASSPORT_RETAIN_GATE_BUILD}/sdk-patches/esp_tls.c" COPYONLY)
endif()
