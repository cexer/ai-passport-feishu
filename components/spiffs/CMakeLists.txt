# Override only GC selection; all other sources/configuration remain ESP-IDF's.
set(sdk_spiffs "${IDF_PATH}/components/spiffs")
file(SHA256 "${sdk_spiffs}/spiffs/src/spiffs_gc.c" gc_baseline)
if(NOT gc_baseline STREQUAL "74eeeb674469a5e5e98fef11b86b39ddafba7d94125de6103b881cb87eca7516")
    message(FATAL_ERROR "SPIFFS GC baseline changed; review the local net-gain patch before upgrading IDF")
endif()
idf_component_register(SRCS "${sdk_spiffs}/spiffs_api.c" "${sdk_spiffs}/esp_spiffs.c"
    "${sdk_spiffs}/spiffs/src/spiffs_cache.c" "${sdk_spiffs}/spiffs/src/spiffs_check.c"
    "spiffs_gc.c" "${sdk_spiffs}/spiffs/src/spiffs_hydrogen.c" "${sdk_spiffs}/spiffs/src/spiffs_nucleus.c"
    INCLUDE_DIRS "${sdk_spiffs}/include"
    PRIV_INCLUDE_DIRS "${sdk_spiffs}" "${sdk_spiffs}/spiffs/src"
    REQUIRES esp_partition PRIV_REQUIRES bootloader_support esptool_py vfs)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-stringop-truncation)
