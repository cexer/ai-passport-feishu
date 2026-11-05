"""Stage the matching storage-only fixture with an unchanged FlashDB library."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--raw", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
stage = root / ("build/flashdb-raw-source" if args.raw else "build/flashdb-probe-source")
upstream = root / "build/flashdb-source"
stage.mkdir(parents=True, exist_ok=True)
for folder in ("main", "components"):
    shutil.copytree(root/folder, stage/folder, dirs_exist_ok=True)
for name in ("CMakeLists.txt", "sdkconfig.defaults", "dependencies.lock", "partitions.csv"):
    shutil.copyfile(root/name, stage/name)
c = stage / "components/flashdb_probe"
c.mkdir(exist_ok=True)
for p in (upstream/"inc").glob("*.h"):
    shutil.copyfile(p, c/p.name)
for name in ("fdb.c", "fdb_file.c", "fdb_kvdb.c", "fdb_utils.c", "fdb_read_cache.c"):
    shutil.copyfile(upstream/"src"/name, c/name)
for name in ("fdb_cfg.h", "flashdb_workload.c", "flashdb_target.c"):
    shutil.copyfile(root/"tools/storage_probe"/name, c/name)
extra = ""
if args.raw:
    for p in (upstream/"port/fal/inc").glob("*.h"):
        shutil.copyfile(p, c/p.name)
    for p in (upstream/"port/fal/src").glob("*.c"):
        shutil.copyfile(p, c/p.name)
        extra += ' "'+p.name+'"'
    for name in ("fal_cfg.h", "flashdb_raw_port.c"):
        shutil.copyfile(root/"tools/storage_probe"/name, c/name)
    extra += ' "flashdb_raw_port.c"'
    cfg = (c/"fdb_cfg.h").read_text().replace("#define FDB_USING_FILE_LIBC_MODE", "#define FDB_USING_FAL_MODE")
    (c/"fdb_cfg.h").write_text(cfg, encoding="utf-8")
(c/"CMakeLists.txt").write_text(
    'idf_component_register(SRCS "fdb.c" "fdb_file.c" "fdb_kvdb.c" "fdb_utils.c" "fdb_read_cache.c" "flashdb_workload.c" "flashdb_target.c"'+extra+'\n'
    ' INCLUDE_DIRS "." REQUIRES spiffs esp_timer freertos esp_partition)\n'
    'target_compile_options(${COMPONENT_LIB} PRIVATE -Os)\n', encoding="utf-8")
main = stage/"main/main.c"
t = main.read_text(encoding="utf-8")
t = t.replace("void app_main(void)\n{", "void sqlite_probe_start(void);\nvoid app_main(void)\n{")
t = t.replace("        product_onboarding_enter(onboarding_complete);", "        /* Storage-only fixture: radio not started. */")
t = t.replace('    ESP_LOGI(TAG, "Ready: Button=', '    sqlite_probe_start();\n    ESP_LOGI(TAG, "Ready: Button=')
main.write_text(t, encoding="utf-8")
cmake = stage/"main/CMakeLists.txt"
cmake.write_text(cmake.read_text().replace("REQUIRES bsp ", "REQUIRES flashdb_probe bsp "), encoding="utf-8")
(stage/"probe-provenance.json").write_text(json.dumps({
    "scope":"emulator-only; file backend; radio not started; upstream sources unchanged",
    "sources":{str(p.relative_to(upstream)): hashlib.sha256(p.read_bytes()).hexdigest()
               for p in list((upstream/"src").glob("*.c")) + list((upstream/"inc").glob("*.h"))},
}, indent=2), encoding="utf-8")
print(stage)
