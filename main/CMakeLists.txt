set(FEISHU_STORAGE_BACKEND "flashdb" CACHE STRING "Feishu storage backend: flashdb or sqlite")
if(FEISHU_STORAGE_BACKEND STREQUAL "flashdb")
    set(feishu_storage_sources "feishu_data_flashdb.c" "feishu_root_flashdb.c" "feishu_pool_flashdb.c" "feishu_flash_port.c")
    set(feishu_storage_requires feishu_flashdb)
elseif(FEISHU_STORAGE_BACKEND STREQUAL "sqlite")
    set(feishu_storage_sources "feishu_data.c" "feishu_root.c" "feishu_pool_sqlite.c")
    set(feishu_storage_requires feishu_sqlite)
else()
    message(FATAL_ERROR "Unsupported FEISHU_STORAGE_BACKEND")
endif()
idf_component_register(
    SRCS "main.c" "feishu_qr_retry.c" "feishu_boot.c" "feishu_power.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_radio.c"
         "demo_feishu.c"
         "feishu_binding.c"
         "feishu_registration.c" "feishu_registration_protocol.c"
         "feishu_api.c"
         "feishu_voice.c"
         "feishu_auth_policy.c"
         "feishu_asr.c" "feishu_xiaozhi_protocol.c" "feishu_xiaozhi_audio.c" "feishu_asr_batch.c" "feishu_adpcm.c" "feishu_internal_asr_protocol.c"
         "feishu_feedback.c" "feishu_outgoing_mentions.c" "feishu_compose.c" "feishu_mcp.c" "feishu_mcp_wire.c" "feishu_xiaozhi_downlink.c" "feishu_xiaozhi_player.c"
         "feishu_http.c"
         "feishu_model.c" "feishu_page.c" "feishu_recovery.c"
         "feishu_network.c"
         "feishu_provision.c"
         "feishu_store.c"
         "feishu_store_legacy.c"
         "feishu_store_json.c"
         "feishu_cache.c"
         "feishu_block_codec.c"
         "feishu_json.c"
         "feishu_messages_stream.c"
         "feishu_message_store.c"
         "feishu_oauth_stream.c"
         "feishu_text_view.c"
         "feishu_text_map.c"
         "feishu_send_codec.c"
         "feishu_result_stream.c"
         "feishu_outbox.c"
         "feishu_image.c"
         "feishu_image_fs.c"
         "product_onboarding.c"
         "setup_portal.c"
         "lv_font_ai_passport_14.c"
         ${feishu_storage_sources}
    INCLUDE_DIRS "."
    REQUIRES esp_pm esp_app_format bsp esp_event esp_hw_support esp_http_client esp_http_server esp_netif esp_timer
             esp_wifi tcp_transport json mbedtls nvs_flash spiffs ${feishu_storage_requires} heatshrink esp_partition jsonsl feishu_jpeg esp_audio_codec
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
if("$ENV{FEISHU_ASSISTANT_AUTOTEST}" MATCHES "^[12345]$")
    if(NOT "$ENV{FEISHU_VOICE_AUTOTEST}" STREQUAL "1" OR NOT FEISHU_ASR_BACKEND STREQUAL "xiaozhi")
        message(FATAL_ERROR "Assistant diagnostic requires explicit XiaoZhi voice fixture injection")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_ASSISTANT_AUTOTEST=1)
    if("$ENV{FEISHU_ASSISTANT_AUTOTEST}" MATCHES "^[2345]$")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_ASSISTANT_TEST_FIRST=7)
    endif()
    if("$ENV{FEISHU_ASSISTANT_AUTOTEST}" STREQUAL "4")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_GLOBAL_AUTOTEST=1)
    endif()
    if("$ENV{FEISHU_ASSISTANT_AUTOTEST}" STREQUAL "5")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_COLLAB_AUTOTEST=1)
    endif()
    if("$ENV{FEISHU_ASSISTANT_AUTOTEST}" MATCHES "^[35]$")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_DRAFT_RAM_AUTOTEST=1)
    endif()
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../build/assistant-autotest")
endif()
if("$ENV{FEISHU_NETWORK_UI_AUTOTEST}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_NETWORK_UI_AUTOTEST=1)
endif()
if("$ENV{FEISHU_UI_AUTOTEST}" STREQUAL "1")
    if(NOT FEISHU_ASR_BACKEND STREQUAL "xiaozhi")
        message(FATAL_ERROR "UI pairing diagnostic requires the XiaoZhi backend")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_UI_AUTOTEST=1)
endif()
if("$ENV{FEISHU_MEMORY_DIAGNOSTIC}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_MEMORY_DIAGNOSTIC=1)
endif()
if("$ENV{FEISHU_NETWORK_DIAGNOSTIC}" STREQUAL "1")
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/network_probe/network_probe.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../build/network-probe")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_NETWORK_DIAGNOSTIC=1)
endif()
if("$ENV{FEISHU_DRAFT_AUTOTEST}" STREQUAL "1")
    if(NOT "$ENV{FEISHU_VOICE_AUTOTEST}" STREQUAL "1" OR
       NOT "$ENV{FEISHU_VOICE_AUTOTEST_PROVISION}" STREQUAL "1")
        message(FATAL_ERROR "Draft autotest requires explicit voice diagnostic provisioning")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_DRAFT_AUTOTEST=1)
    if("$ENV{FEISHU_DRAFT_VOICE_SUITE}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_DRAFT_VOICE_SUITE=1)
    endif()
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/voice_autotest/draft_autotest.c")
endif()
if("$ENV{FEISHU_BUSINESS_AUTOTEST}" STREQUAL "1")
    if(NOT "$ENV{FEISHU_VOICE_AUTOTEST}" STREQUAL "1" OR
       NOT "$ENV{FEISHU_VOICE_AUTOTEST_PROVISION}" STREQUAL "1")
        message(FATAL_ERROR "Business autotest requires explicit voice diagnostic provisioning")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_BUSINESS_AUTOTEST=1)
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/voice_autotest/business_autotest.c")
endif()
if("$ENV{FEISHU_VOICE_AUTOTEST}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_VOICE_AUTOTEST=1)
    if("$ENV{FEISHU_VOICE_AUTOTEST_PROVISION}" STREQUAL "1")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_VOICE_AUTOTEST_PROVISION=1)
    endif()
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../tools/voice_autotest"
        "${CMAKE_CURRENT_SOURCE_DIR}/../build/voice-autotest")
endif()

if(FEISHU_STORAGE_BACKEND STREQUAL "flashdb")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_STORE_FRESH_POOL=1)
endif()
if("$ENV{FEISHU_STORAGE_AUTOTEST}" STREQUAL "1")
    if(NOT FEISHU_STORAGE_BACKEND STREQUAL "flashdb")
        message(FATAL_ERROR "Storage autotest requires the raw backend")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_STORAGE_AUTOTEST=1)
    if("$ENV{FEISHU_STORAGE_REBOOT_ONLY}" STREQUAL "1")
        target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/storage_probe/flashdb_reboot_workload.c")
    else()
        target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools/storage_probe/flashdb_product_workload.c")
    endif()
endif()

if("$ENV{FEISHU_SEND_PERF_AUTOTEST}" STREQUAL "1")
    if(NOT "$ENV{FEISHU_BUSINESS_AUTOTEST}" STREQUAL "1")
        message(FATAL_ERROR "Send performance probe requires explicit business diagnostic")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_SEND_PERF_AUTOTEST=1)
endif()

set(FEISHU_ASR_BACKEND "feishu" CACHE STRING "Voice recognition backend: feishu (qualified baseline) or xiaozhi (experimental)")
if(FEISHU_ASR_BACKEND STREQUAL "xiaozhi")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_ASR_XIAOZHI=1)
elseif(NOT FEISHU_ASR_BACKEND STREQUAL "feishu")
    message(FATAL_ERROR "Unsupported FEISHU_ASR_BACKEND")
endif()
if("$ENV{FEISHU_MCP_DIAGNOSTIC}" STREQUAL "1")
    if(NOT FEISHU_ASR_BACKEND STREQUAL "xiaozhi" OR "$ENV{FEISHU_MEMORY_DIAGNOSTIC}" STREQUAL "1")
        message(FATAL_ERROR "MCP diagnostic requires XiaoZhi and ordinary heap configuration")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_MCP_DIAGNOSTIC=1)
endif()

if("$ENV{FEISHU_ONBOARDING_AUTOTEST}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_ONBOARDING_AUTOTEST=1 FEISHU_VOICE_AUTOTEST=1)
endif()

if("$ENV{FEISHU_ASSISTANT_TIMEOUT_AUTOTEST}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_ASSISTANT_TIMEOUT_AUTOTEST=1)
endif()

if("$ENV{FEISHU_LVGL_RECOVERY_AUTOTEST}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_LVGL_RECOVERY_AUTOTEST=1)
endif()

if("$ENV{FEISHU_COMPOSE_FLOW_AUTOTEST}" STREQUAL "1")
    if(NOT "$ENV{FEISHU_ASSISTANT_AUTOTEST}" STREQUAL "4")
        message(FATAL_ERROR "Compose flow injection requires the explicit global voice diagnostic")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_COMPOSE_FLOW_AUTOTEST=1)
endif()

if("$ENV{FEISHU_SCENARIO_AUTOTEST}" STREQUAL "1")
    if(NOT "$ENV{FEISHU_ASSISTANT_AUTOTEST}" STREQUAL "4")
        message(FATAL_ERROR "Seeded scenarios require the explicit global voice diagnostic")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FEISHU_SCENARIO_AUTOTEST=1 FEISHU_MEMORY_RECOVERY_AUTOTEST=1)
endif()
