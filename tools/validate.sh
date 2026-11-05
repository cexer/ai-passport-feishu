#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware]" >&2
}

run_static_checks() {
    local actionlint_bin
    local test_dir

    python3 tools/check_repo.py
    python3 tests/check_feishu_storage_boundary.py
    local qr_test_dir
    qr_test_dir="$(mktemp -d /tmp/ai-passport-qr-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_feishu_qr_retry.c main/feishu_qr_retry.c -o "${qr_test_dir}/test_feishu_qr_retry"
    "${qr_test_dir}/test_feishu_qr_retry"
    rm -rf "${qr_test_dir}"

    local fdb_test_dir
    fdb_test_dir="$(mktemp -d /tmp/ai-passport-fdb-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DFDB_USING_NAME_FILTER=1 \
        -Icomponents/feishu_flashdb/include -Icomponents/feishu_flashdb/vendor \
        tests/test_feishu_fdb.c components/feishu_flashdb/feishu_fdb.c \
        components/feishu_flashdb/vendor/fdb.c components/feishu_flashdb/vendor/fdb_kvdb.c \
        components/feishu_flashdb/vendor/fdb_utils.c components/feishu_flashdb/vendor/fdb_read_cache.c \
        -o "${fdb_test_dir}/test_feishu_fdb"
    "${fdb_test_dir}/test_feishu_fdb"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS \
        -DFDB_USING_NAME_FILTER=1 \
        '-DFDB_READ_CACHE_MALLOC(n)=((void)(n),(void*)0)' '-DFDB_READ_CACHE_FREE(p)=((void)(p))' \
        -Icomponents/feishu_flashdb/include -Icomponents/feishu_flashdb/vendor \
        tests/test_feishu_fdb.c components/feishu_flashdb/feishu_fdb.c \
        components/feishu_flashdb/vendor/fdb.c components/feishu_flashdb/vendor/fdb_kvdb.c \
        components/feishu_flashdb/vendor/fdb_utils.c components/feishu_flashdb/vendor/fdb_read_cache.c \
        -o "${fdb_test_dir}/no_read_window"
    "${fdb_test_dir}/no_read_window"
    local fdb_sources=(
        main/feishu_data_flashdb.c main/feishu_block_codec.c
        tests/feishu_fdb_host.c
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c
        components/feishu_flashdb/feishu_fdb.c
        components/feishu_flashdb/vendor/fdb.c components/feishu_flashdb/vendor/fdb_kvdb.c
        components/feishu_flashdb/vendor/fdb_utils.c components/feishu_flashdb/vendor/fdb_read_cache.c
    )
    local fdb_flags=(-std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS
        -DHEATSHRINK_DYNAMIC_ALLOC=0 -Imain -Itests -Icomponents/heatshrink
        -Icomponents/feishu_flashdb/include -Icomponents/feishu_flashdb/vendor)
    "${CC:-cc}" "${fdb_flags[@]}" -Dmain=storage_contract_main -c \
        tests/test_feishu_data_contract.c -o "${fdb_test_dir}/contract.o"
    "${CC:-cc}" "${fdb_flags[@]}" tests/run_feishu_data_flashdb_contract.c \
        "${fdb_test_dir}/contract.o" "${fdb_sources[@]}" -o "${fdb_test_dir}/contract"
    "${fdb_test_dir}/contract"
    "${CC:-cc}" "${fdb_flags[@]}" tests/test_feishu_data_flashdb_faults.c \
        "${fdb_sources[@]}" -o "${fdb_test_dir}/publication_faults"
    "${fdb_test_dir}/publication_faults"
    "${CC:-cc}" "${fdb_flags[@]}" tests/test_feishu_data_flashdb_slots.c \
        "${fdb_sources[@]}" -o "${fdb_test_dir}/catalog_slots"
    "${fdb_test_dir}/catalog_slots"
    "${CC:-cc}" "${fdb_flags[@]}" tests/test_feishu_data_flashdb_text.c \
        main/feishu_text_map.c "${fdb_sources[@]}" -o "${fdb_test_dir}/text"
    "${fdb_test_dir}/text"
    local fdb_root_sources=()
    for source in "${fdb_sources[@]}"; do
        if [[ "${source}" != "main/feishu_data_flashdb.c" ]]; then
            fdb_root_sources+=("${source}")
        fi
    done
    "${CC:-cc}" "${fdb_flags[@]}" tests/test_feishu_root_flashdb.c \
        main/feishu_root_flashdb.c "${fdb_root_sources[@]}" -o "${fdb_test_dir}/root"
    "${fdb_test_dir}/root"
    "${CC:-cc}" "${fdb_flags[@]}" -Itests/feishu_host -Dmain=store_contract_main \
        -c tests/test_feishu_store.c -o "${fdb_test_dir}/store_contract.o"
    "${CC:-cc}" "${fdb_flags[@]}" tests/run_feishu_store_flashdb.c main/feishu_model.c \
        "${fdb_test_dir}/store_contract.o" main/feishu_root_flashdb.c "${fdb_root_sources[@]}" \
        -o "${fdb_test_dir}/store"
    mkdir "${fdb_test_dir}/store-files"
    "${fdb_test_dir}/store" "${fdb_test_dir}/store-files"
    "${CC:-cc}" "${fdb_flags[@]}" -DFDB_USING_NAME_FILTER=1 -Itests/feishu_host \
        tests/test_feishu_flashdb_app.c main/feishu_root_flashdb.c main/feishu_outbox.c \
        main/feishu_model.c main/feishu_text_map.c "${fdb_sources[@]}" \
        -o "${fdb_test_dir}/app"
    "${fdb_test_dir}/app"
    case "${fdb_test_dir}" in
        /tmp/ai-passport-fdb-tests.*) rm -r -- "${fdb_test_dir}" ;;
        *) echo "ERROR: unexpected FlashDB test directory" >&2; return 1 ;;
    esac

    local vfs_test_dir
    vfs_test_dir="$(mktemp -d /tmp/ai-passport-vfs-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/feishu_sqlite \
        tests/test_feishu_vfs_write.c components/feishu_sqlite/feishu_vfs_write.c \
        -o "${vfs_test_dir}/test_feishu_vfs_write"
    "${vfs_test_dir}/test_feishu_vfs_write"
    if [[ -n "${IDF_PATH:-}" ]]; then
        local spiffs_src="${IDF_PATH//\\//}/components/spiffs/spiffs/src"
        "${CC:-cc}" -std=c11 -w -Itests/spiffs_host -I"${spiffs_src}" \
            tests/test_spiffs_gc_pressure.c components/spiffs/spiffs_gc.c \
            "${spiffs_src}/spiffs_nucleus.c" "${spiffs_src}/spiffs_hydrogen.c" \
            "${spiffs_src}/spiffs_check.c" "${spiffs_src}/spiffs_cache.c" \
            -o "${vfs_test_dir}/test_spiffs_gc_pressure"
        "${vfs_test_dir}/test_spiffs_gc_pressure"
    else
        echo "SPIFFS GC integration: NOT RUN (requires IDF_PATH)"
    fi
    case "${vfs_test_dir}" in
        /tmp/ai-passport-vfs-tests.*) rm -r -- "${vfs_test_dir}" ;;
        *) echo "ERROR: unexpected VFS test directory" >&2; return 1 ;;
    esac

    actionlint_bin="${ACTIONLINT_BIN:-}"
    if [[ -z "${actionlint_bin}" ]]; then
        actionlint_bin="$(command -v actionlint || true)"
    fi
    if [[ -z "${actionlint_bin}" || ! -x "${actionlint_bin}" ]]; then
        actionlint_bin="$(./tools/install-actionlint.sh)"
    fi
    "${actionlint_bin}" -color .github/workflows/*.yml

    test_dir="$(mktemp -d /tmp/ai-passport-host-tests.XXXXXX)"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_ui_pixel_math.c main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_feishu_model.c main/feishu_model.c \
        -o "${test_dir}/test_feishu_model"
    "${test_dir}/test_feishu_model"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_feishu_internal_asr_protocol.c main/feishu_internal_asr_protocol.c \
        -o "${test_dir}/test_feishu_internal_asr_protocol"
    "${test_dir}/test_feishu_internal_asr_protocol"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_feishu_asr_batch.c main/feishu_asr_batch.c -o "${test_dir}/test_feishu_asr_batch"
    "${test_dir}/test_feishu_asr_batch"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -Imain \
        tests/test_feishu_adpcm.c main/feishu_adpcm.c -o "${test_dir}/test_feishu_adpcm"
    "${test_dir}/test_feishu_adpcm"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS \
        -Itests/voice_host -Itests/feishu_host -Imain -Icomponents/feishu_jpeg \
        tests/test_feishu_voice_ram.c main/feishu_voice.c main/feishu_internal_asr_protocol.c main/feishu_model.c \
        -o "${test_dir}/test_feishu_voice_ram"
    "${test_dir}/test_feishu_voice_ram"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_feishu_page.c main/feishu_page.c -o "${test_dir}/test_feishu_page"
    "${test_dir}/test_feishu_page"
    mkdir "${test_dir}/recovery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -Imain tests/test_feishu_recovery.c main/feishu_recovery.c main/feishu_root.c -o "${test_dir}/test_feishu_recovery"
    "${test_dir}/test_feishu_recovery" "${test_dir}/recovery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
        tests/test_feishu_auth_policy.c main/feishu_auth_policy.c \
        -o "${test_dir}/test_feishu_auth_policy"
    "${test_dir}/test_feishu_auth_policy"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DJSONSL_NO_JPR -DJSONSL_STATE_GENERIC \
        -Imain -Icomponents/jsonsl tests/test_feishu_registration_protocol.c \
        main/feishu_registration_protocol.c main/feishu_json.c components/jsonsl/jsonsl.c \
        -o "${test_dir}/test_feishu_registration_protocol"
    "${test_dir}/test_feishu_registration_protocol"
    python3 -m unittest discover -s tests -p 'test_feishu_registration_config.py'
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS \
        -Imain tests/test_feishu_root.c main/feishu_root.c -o "${test_dir}/test_feishu_root"
    "${test_dir}/test_feishu_root" "${test_dir}"
    mkdir "${test_dir}/config"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS \
        -Itests/feishu_host -Imain tests/test_feishu_store.c main/feishu_root.c main/feishu_model.c \
        -o "${test_dir}/test_feishu_store"
    "${test_dir}/test_feishu_store" "${test_dir}/config"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DHEATSHRINK_DYNAMIC_ALLOC=0 \
        -Imain -Icomponents/heatshrink tests/test_feishu_block_codec.c \
        main/feishu_block_codec.c components/heatshrink/heatshrink_encoder.c \
        components/heatshrink/heatshrink_decoder.c -o "${test_dir}/test_feishu_block_codec"
    "${test_dir}/test_feishu_block_codec"
    "${CC:-cc}" -Os -std=c11 -D_CRT_SECURE_NO_WARNINGS -DSQLITE_THREADSAFE=0 \
        -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_WAL -DSQLITE_OMIT_SHARED_CACHE \
        -DSQLITE_OMIT_UTF16 -DSQLITE_OMIT_DEPRECATED -DSQLITE_OMIT_JSON \
        -DSQLITE_TEMP_STORE=3 -DSQLITE_DEFAULT_PAGE_SIZE=1024 -DSQLITE_DEFAULT_CACHE_SIZE=8 \
        -DSQLITE_MAX_LENGTH=16384 -DSQLITE_MAX_SQL_LENGTH=4096 \
        -DSQLITE_MAX_MMAP_SIZE=0 -c components/feishu_sqlite/sqlite3.c \
        -o "${test_dir}/sqlite3.o"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DHEATSHRINK_DYNAMIC_ALLOC=0 -D_CRT_SECURE_NO_WARNINGS \
        -Imain -Icomponents/heatshrink -Icomponents/feishu_sqlite \
        tests/test_feishu_data.c main/feishu_data.c main/feishu_block_codec.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_data"
    "${test_dir}/test_feishu_data" "${test_dir}/objects.db"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DHEATSHRINK_DYNAMIC_ALLOC=0 -D_CRT_SECURE_NO_WARNINGS \
        -Imain -Icomponents/heatshrink -Icomponents/feishu_sqlite \
        tests/test_feishu_data_contract.c main/feishu_data.c main/feishu_block_codec.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_data_contract"
    "${test_dir}/test_feishu_data_contract" "${test_dir}/storage-contract.db" sqlite
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DHEATSHRINK_DYNAMIC_ALLOC=0 -D_CRT_SECURE_NO_WARNINGS \
        -Imain -Icomponents/heatshrink -Icomponents/feishu_sqlite \
        tests/test_feishu_open_pressure.c main/feishu_data.c main/feishu_block_codec.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_open_pressure"
    "${test_dir}/test_feishu_open_pressure" "${test_dir}/open-pressure.db"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -DHEATSHRINK_DYNAMIC_ALLOC=0 -D_CRT_SECURE_NO_WARNINGS \
        -Imain -Icomponents/heatshrink -Icomponents/feishu_sqlite \
        tests/test_feishu_text_store.c main/feishu_text_map.c main/feishu_data.c main/feishu_block_codec.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_text_store"
    "${test_dir}/test_feishu_text_store" "${test_dir}/text-store.db"
    mkdir "${test_dir}/pending"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DHEATSHRINK_DYNAMIC_ALLOC=0 \
        -Itests/feishu_host -Imain -Icomponents/heatshrink -Icomponents/feishu_sqlite \
        tests/test_feishu_pending_cache.c main/feishu_recovery.c main/feishu_root.c main/feishu_text_map.c main/feishu_data.c main/feishu_outbox.c main/feishu_block_codec.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_pending_cache"
    "${test_dir}/test_feishu_pending_cache" "${test_dir}/pending"
    mkdir "${test_dir}/recovery-cache"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DHEATSHRINK_DYNAMIC_ALLOC=0 \
        -Itests/feishu_host -Imain -Icomponents/heatshrink -Icomponents/feishu_sqlite \
        tests/test_feishu_recovery_cache.c main/feishu_recovery.c main/feishu_root.c main/feishu_text_map.c main/feishu_data.c main/feishu_outbox.c main/feishu_block_codec.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_recovery_cache"
    "${test_dir}/test_feishu_recovery_cache" "${test_dir}/recovery-cache"
    mkdir "${test_dir}/pages"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DHEATSHRINK_DYNAMIC_ALLOC=0 \
        -Itests/feishu_host -Imain -Icomponents/heatshrink -Icomponents/feishu_sqlite \
        tests/test_feishu_page_cache.c main/feishu_recovery.c main/feishu_root.c main/feishu_page.c main/feishu_text_map.c main/feishu_data.c main/feishu_outbox.c main/feishu_block_codec.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_page_cache"
    "${test_dir}/test_feishu_page_cache" "${test_dir}/pages"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DHEATSHRINK_DYNAMIC_ALLOC=0 \
        -DJSONSL_NO_JPR -DJSONSL_STATE_GENERIC -Itests/feishu_host -Imain \
        -Icomponents/heatshrink -Icomponents/feishu_sqlite -Icomponents/jsonsl \
        tests/test_feishu_message_cache.c main/feishu_recovery.c main/feishu_root.c main/feishu_message_store.c main/feishu_text_map.c \
        main/feishu_messages_stream.c main/feishu_json.c main/feishu_model.c main/feishu_data.c \
        main/feishu_outbox.c main/feishu_block_codec.c components/jsonsl/jsonsl.c \
        components/heatshrink/heatshrink_encoder.c components/heatshrink/heatshrink_decoder.c \
        "${test_dir}/sqlite3.o" -o "${test_dir}/test_feishu_message_cache"
    python3 tests/check_feishu_message_cache.py "${test_dir}/test_feishu_message_cache" "${test_dir}/message-cache"
    "${CC:-cc}" -Os -std=c11 -Wall -Wextra -Werror -DJSONSL_NO_JPR -DJSONSL_STATE_GENERIC \
        -Imain -Icomponents/jsonsl tests/test_feishu_json.c main/feishu_json.c \
        components/jsonsl/jsonsl.c -o "${test_dir}/test_feishu_json"
    "${test_dir}/test_feishu_json"
    "${CC:-cc}" -Os -std=c11 -Wall -Wextra -Werror -DJSONSL_NO_JPR -DJSONSL_STATE_GENERIC \
        -Imain -Icomponents/jsonsl tests/test_feishu_oauth_stream.c main/feishu_oauth_stream.c \
        main/feishu_json.c components/jsonsl/jsonsl.c -o "${test_dir}/test_feishu_oauth_stream"
    "${test_dir}/test_feishu_oauth_stream"
    "${CC:-cc}" -Os -std=c11 -Wall -Wextra -Werror -DJSONSL_NO_JPR -DJSONSL_STATE_GENERIC \
        -Imain -Icomponents/jsonsl tests/test_feishu_result_stream.c main/feishu_result_stream.c \
        main/feishu_json.c components/jsonsl/jsonsl.c -o "${test_dir}/test_feishu_result_stream"
    "${test_dir}/test_feishu_result_stream"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -Imain \
        tests/test_feishu_send_codec.c main/feishu_send_codec.c -o "${test_dir}/test_feishu_send_codec"
    "${test_dir}/test_feishu_send_codec" "${test_dir}"
    python3 tests/check_feishu_send_codec.py "${test_dir}"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -Imain \
        tests/test_feishu_outbox.c main/feishu_outbox.c -o "${test_dir}/test_feishu_outbox"
    "${test_dir}/test_feishu_outbox"
    mkdir "${test_dir}/jpeg"
    "${CC:-cc}" -Os -std=c11 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS \
        -Djd_prepare=feishu_jd_prepare -Djd_decomp=feishu_jd_decomp -Icomponents/feishu_jpeg \
        tests/test_feishu_jpeg.c components/feishu_jpeg/feishu_jpeg.c components/feishu_jpeg/tjpgd.c \
        -o "${test_dir}/test_feishu_jpeg"
    python3 tests/check_feishu_jpeg.py "${test_dir}/test_feishu_jpeg" "${test_dir}/jpeg"
    "${CC:-cc}" -Os -std=c11 -Wall -Wextra -Werror -DJSONSL_NO_JPR -DJSONSL_STATE_GENERIC \
        -Imain -Icomponents/jsonsl tests/test_feishu_messages_stream.c main/feishu_messages_stream.c \
        main/feishu_json.c main/feishu_model.c components/jsonsl/jsonsl.c \
        -o "${test_dir}/test_feishu_messages_stream"
    "${test_dir}/test_feishu_messages_stream"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_feishu_text_view.c \
        main/feishu_text_view.c -o "${test_dir}/test_feishu_text_view"
    "${test_dir}/test_feishu_text_view"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain tests/test_feishu_text_map.c \
        main/feishu_text_map.c -o "${test_dir}/test_feishu_text_map"
    "${test_dir}/test_feishu_text_map"
    python3 -m unittest discover -s tests -p 'test_feishu_provision.py'
    python3 -m unittest discover -s tests -p 'test_firmware_layout.py'
    python3 -m unittest discover -s tests -p 'test_feishu_font.py'
    python3 -m unittest discover -s tests -p 'test_feishu_voice_login.py'
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_firmware_checks() (
    local validation_build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    validation_build_dir="$(mktemp -d /tmp/ai-passport-firmware.XXXXXX)"
    trap 'case "${validation_build_dir}" in /tmp/ai-passport-firmware.*) rm -rf -- "${validation_build_dir}" ;; esac' EXIT

    SDKCONFIG_DEFAULTS="${repo_root}/sdkconfig.defaults" \
        idf.py -B "${validation_build_dir}" \
        -D "SDKCONFIG=${validation_build_dir}/sdkconfig" build
    idf.py -B "${validation_build_dir}" merge-bin \
        -o "${validation_build_dir}/FoloToy-AI-Passport-full.bin"
    python3 tools/verify_firmware.py "${validation_build_dir}"
    mkdir -p "${repo_root}/build"
    install -m 0644 \
        "${validation_build_dir}/FoloToy-AI-Passport-full.bin" \
        "${repo_root}/build/FoloToy-AI-Passport-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
