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

# Read-only font assets use spare Flash without moving app, identity or data.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/feishu-14-4bpp.bin"
               "${CMAKE_BINARY_DIR}/feishu-font.bin" COPYONLY)
esptool_py_flash_to_partition(flash feishu_font "${CMAKE_BINARY_DIR}/feishu-font.bin")
