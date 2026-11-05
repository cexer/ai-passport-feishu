#!/usr/bin/env python3
"""Stage a synthetic HTTP-only integration variant; never flash this fixture."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument("--panic-probe",action="store_true")
parser.add_argument("--corrupt-db",action="store_true")
parser.add_argument("--live-db-faults",action="store_true")
parser.add_argument("--onboarding",action="store_true")
args=parser.parse_args()
if args.live_db_faults:args.corrupt_db=True
root = Path(__file__).resolve().parents[1]
stage = root / "build" / "simulator-source"
stage.mkdir(parents=True, exist_ok=True)
for folder in ("main", "components"):
    shutil.copytree(root / folder, stage / folder, dirs_exist_ok=True)
for name in ("CMakeLists.txt", "sdkconfig.defaults", "dependencies.lock", "partitions.csv"):
    shutil.copyfile(root / name, stage / name)

# Endpoints are the only changes to the API client. Authentication, parsing,
# storage, workers, navigation and microphone capture remain the actual code.
for name in ("feishu_api.c", "feishu_binding.c", "feishu_registration.c"):
    path = stage / "main" / name
    text = path.read_text(encoding="utf-8")
    for origin in ("https://accounts.feishu.cn", "https://open.feishu.cn"):
        if origin not in text:
            assert name=="feishu_registration.c", name
        text = text.replace(origin, "http://host.emu.internal")
    path.write_text(text, encoding="utf-8")

path = stage / "main" / "main.c"
text = path.read_text(encoding="utf-8-sig")
anchor = "void app_main(void)\n{"
assert anchor in text
path.write_text(text.replace(anchor, "void simulator_seed(void);\n" + anchor +
                             "\n    simulator_seed();"), encoding="utf-8")
path = stage / "main" / "CMakeLists.txt"
text = path.read_text(encoding="utf-8")
path.write_text(text.replace('SRCS "main.c"', 'SRCS "main.c" "simulator_seed.c"'),
                encoding="utf-8")
path = stage / "main" / "feishu_network.c"
text = path.read_text(encoding="utf-8")
anchor = "    if (err == ESP_OK) err = esp_wifi_get_config(WIFI_IF_STA, &saved);"
assert anchor in text
text = text.replace(anchor, '''    // Emulator fixture: radio HLE does not persist config across deinit/init.
    if (err == ESP_OK) {
        wifi_config_t fixture = { 0 };
        strcpy((char *)fixture.sta.ssid, "feishu-simulator-ap");
        err = esp_wifi_set_config(WIFI_IF_STA, &fixture);
    }
''' + anchor)
anchor = "    err = esp_wifi_set_ps(WIFI_PS_NONE);\n    if (err != ESP_OK) goto fail;"
assert anchor in text
text = text.replace(anchor, "    // RF power-save control is unmodeled by PassportSim 1.0.3.")
path.write_text(text, encoding="utf-8")
(stage / "main" / "simulator_seed.c").write_text(r'''
// SYNTHETIC FIXTURE ONLY. Not a user firmware or real account configuration.
#include "demo_radio.h"
#include "feishu_store.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdlib.h>

void simulator_seed(void)
{
    ESP_ERROR_CHECK(demo_radio_nvs_prepare());
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("sim_fixture", NVS_READWRITE, &handle));
    uint8_t seeded = 0;
    if (nvs_get_u8(handle, "seeded", &seeded) == ESP_OK && seeded == 1) {
        nvs_close(handle);
        return;
    }
    feishu_credentials_t *credentials = calloc(1,sizeof(*credentials));
    assert(credentials);
    strcpy(credentials->app_id, "cli_simulator_fixture");
    strcpy(credentials->app_secret, "synthetic-not-a-secret");
    strcpy(credentials->refresh_token, "simulator-refresh");
    // Exercise actual legacy NVS -> shared-pool migration on the target.
    nvs_handle_t legacy;
    ESP_ERROR_CHECK(nvs_open("feishu",NVS_READWRITE,&legacy));
    ESP_ERROR_CHECK(nvs_set_str(legacy,"app_id",credentials->app_id));
    ESP_ERROR_CHECK(nvs_set_str(legacy,"app_secret",credentials->app_secret));
    ESP_ERROR_CHECK(nvs_set_blob(legacy,"refresh",credentials->refresh_token,strlen(credentials->refresh_token)+1));
    ESP_ERROR_CHECK(nvs_set_u8(legacy,"owner_v",1));
    ESP_ERROR_CHECK(nvs_commit(legacy));nvs_close(legacy);
    ESP_ERROR_CHECK(nvs_flash_init_partition("feishu_cache"));
    ESP_ERROR_CHECK(nvs_open_from_partition("feishu_cache","tokens",NVS_READWRITE,&legacy));
    ESP_ERROR_CHECK(nvs_set_blob(legacy,"access","simulator-access",sizeof("simulator-access")));
    ESP_ERROR_CHECK(nvs_commit(legacy));nvs_close(legacy);
    ESP_ERROR_CHECK(feishu_store_load_credentials(credentials));
    assert(!strcmp(credentials->app_id,"cli_simulator_fixture"));
    assert(!strcmp(credentials->refresh_token,"simulator-refresh"));
    ESP_ERROR_CHECK(nvs_open("feishu",NVS_READONLY,&legacy));
    size_t remaining=0;
    assert(nvs_get_str(legacy,"app_secret",NULL,&remaining)==ESP_ERR_NVS_NOT_FOUND);
    nvs_close(legacy);
    memset(credentials, 0, sizeof(*credentials));free(credentials);
    ESP_ERROR_CHECK(nvs_set_u8(handle, "seeded", 1));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
}
''', encoding="utf-8")
if args.onboarding:
    seed=stage/"main/simulator_seed.c"
    source=seed.read_text(encoding="utf-8")
    anchor='    ESP_ERROR_CHECK(nvs_set_u8(handle, "seeded", 1));'
    assert anchor in source
    fixture='''    // First-run app selection fixture; never real credentials.
    ESP_ERROR_CHECK(feishu_store_clear_credentials());
    wifi_config_t wifi={0};
    strcpy((char *)wifi.sta.ssid,"feishu-simulator-ap");
    ESP_ERROR_CHECK(feishu_store_save_wifi(&wifi,sizeof(wifi)));
    ESP_ERROR_CHECK(feishu_store_require_wifi_setup(false));
'''
    seed.write_text(source.replace(anchor,fixture+anchor),encoding="utf-8")
if args.corrupt_db:
    seed=stage/"main/simulator_seed.c"
    source=seed.read_text(encoding="utf-8").replace('#include "nvs_flash.h"','#include "nvs_flash.h"\n#include "feishu_cache.h"\n#include <stdio.h>\n#include <unistd.h>')
    anchor='    memset(credentials, 0, sizeof(*credentials));free(credentials);'
    assert anchor in source
    injection=r"""    ESP_ERROR_CHECK(feishu_store_save_cache_owner("ou_simulator_self"));
    ESP_ERROR_CHECK(feishu_cache_init());
    feishu_cache_set_scope(credentials->app_id,"ou_simulator_self");
    feishu_data_writer_t body={0};feishu_outbox_t pending;
    ESP_ERROR_CHECK(feishu_cache_draft_begin(&body));
    ESP_ERROR_CHECK(feishu_cache_text_append(&body,"old protected seed",18));
    assert(feishu_outbox_create(&pending,body.id,body.raw,"recovery-seed","oc_simulator_a","","old-seed-uuid"));
    ESP_ERROR_CHECK(feishu_cache_pending_write(0,&pending,&body));
    feishu_cache_close();
    FILE *failed=fopen("/feishu/objects.db","r+b");assert(failed);
    assert(fputc('X',failed)!=EOF && !fflush(failed) && !fsync(fileno(failed)) && !fclose(failed));
    printf("RECOVERY_FIXTURE protected_seeded=1 header_corrupted=1\n");
"""
    seed.write_text(source.replace(anchor,injection+anchor),encoding="utf-8")
if args.live_db_faults:
    cache=stage/"main/feishu_cache.c"
    with cache.open("a",encoding="utf-8") as f:f.write(r"""
#include <assert.h>
#include <unistd.h>
void simulator_corrupt_store(void)
{
    LOCK();unsigned before=feishu_cache_recovery_generation();
    assert(feishu_data_close(&store)==SQLITE_OK);
    FILE *file=fopen("/feishu/objects.db","r+b");assert(file);
    assert(fputc('X',file)!=EOF && !fflush(file) && !fsync(fileno(file)) && !fclose(file));
    assert(open_impl()==ESP_ERR_INVALID_STATE);
    assert(feishu_cache_recovery_generation()==before+1);
    printf("RECOVERY_LIVE generation=%u\n",before+1);UNLOCK();
}
""")
    asr=stage/"main/feishu_asr.c";source=asr.read_text(encoding="utf-8")
    source='void simulator_corrupt_store(void);\n'+source
    anchor='''                ASR_CHUNK_SAMPLES, recognition, recognition_size,NULL,cancel_requested);
            xQueueSend(context->free_queue, &pending, portMAX_DELAY);'''
    replacement='''                ASR_CHUNK_SAMPLES, recognition, recognition_size,NULL,cancel_requested);
            if(!strcmp(recognition,"SIM_CORRUPT_CAPTURE")) {
                simulator_corrupt_store();vTaskDelay(pdMS_TO_TICKS(500));err=ESP_ERR_INVALID_STATE;break;
            }
            xQueueSend(context->free_queue, &pending, portMAX_DELAY);'''
    assert anchor in source;asr.write_text(source.replace(anchor,replacement),encoding="utf-8")
    api=stage/"main/feishu_api.c";source=api.read_text(encoding="utf-8")
    source='void simulator_corrupt_store(void);\n'+source
    anchor='        bool success=err==ESP_OK && feishu_result_stream_finish(parser) && message_id[0];'
    assert anchor in source
    api.write_text(source.replace(anchor,anchor+'\n        if(success && !strcmp(message_id,"om_simulator_recover_trigger")) {simulator_corrupt_store();vTaskDelay(pdMS_TO_TICKS(500));}'),encoding="utf-8")
provenance = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
              for folder in ("main", "components")
              for p in (root / folder).rglob("*") if p.is_file()}
(stage / "fixture-provenance.json").write_text(json.dumps({
    "source_sha256": provenance,
    "differences": ["HTTP loopback endpoints", "one-time synthetic NVS seed",
                    "virtual Wi-Fi config after each driver initialization",
                    "omit unmodeled RF power-save configuration"],
    "not_validated": ["real OAuth", "TLS", "mobile SoftAP", "physical hardware"],
}, indent=2) + "\n", encoding="utf-8")
if args.corrupt_db or args.live_db_faults:
    fixture=json.loads((stage/"fixture-provenance.json").read_text(encoding="utf-8"))
    if args.corrupt_db:fixture["differences"].append("seed a fake protected draft and corrupt the DB header before app startup")
    if args.live_db_faults:fixture["differences"].append("test-only header faults during capture and send, followed by a 500ms completion delay")
    (stage/"fixture-provenance.json").write_text(json.dumps(fixture,indent=2)+"\n",encoding="utf-8")
if args.panic_probe:
    (stage/"main/panic_probe.c").write_text(r'''
// Diagnostic fixture only: print context, then invoke the unchanged handler.
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"
#include "esp_rom_sys.h"
void __real_esp_panic_handler(panic_info_t *);
void __wrap_esp_panic_handler(panic_info_t *info) {
    panic_print_str("PROBE_PANIC reason=");
    panic_print_str(info->reason ? info->reason : "none");
    panic_print_str(" description=");
    panic_print_str(info->description ? info->description : "none");
    panic_print_str("\nPROBE_PANIC abort=");
    panic_print_str(g_panic_abort_details ? g_panic_abort_details : "none");
    const RvExcFrame *f=info->frame;
    esp_rom_printf("\nPROBE_PANIC addr=%p pc=%08lx ra=%08lx sp=%08lx\n",info->addr,
                   f ? f->mepc : 0L,f ? f->ra : 0L,f ? f->sp : 0L);
    __real_esp_panic_handler(info);
}
''',encoding="utf-8")
    path=stage/"main/CMakeLists.txt"
    path.write_text(path.read_text().replace('SRCS "main.c"','SRCS "main.c" "panic_probe.c"'),encoding="utf-8")
    path=stage/"CMakeLists.txt"
    path.write_text(path.read_text()+'\ntarget_link_options(${CMAKE_PROJECT_NAME}.elf PRIVATE "-Wl,--wrap=esp_panic_handler")\n',encoding="utf-8")
print(stage)
