"""Stage a silent synthetic codec/cache benchmark. Never seeds real credentials."""
import argparse
import shutil
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument("--persistent-journal",action="store_true")
parser.add_argument("--exclusive-locking",action="store_true")
mode=parser.add_mutually_exclusive_group()
mode.add_argument("--reset-probe",action="store_true")
mode.add_argument("--text-store-probe",action="store_true")
mode.add_argument("--page-cache-probe",action="store_true")
mode.add_argument("--db-recovery-probe",action="store_true")
mode.add_argument("--registration-probe",action="store_true")
mode.add_argument("--wifi-probe",action="store_true")
args=parser.parse_args()
root=Path(__file__).resolve().parents[1]
stage=root/"build/media-probe-source"
stage.mkdir(parents=True,exist_ok=True)
def copy_changed(source,target):
    if target.is_file() and source.read_bytes()==target.read_bytes():return
    target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source,target)

for directory in ("main","components"):
    for source in (root/directory).rglob('*'):
        if source.is_file():copy_changed(source,stage/source.relative_to(root))
for name in ("CMakeLists.txt","sdkconfig.defaults","dependencies.lock","partitions.csv"):
    copy_changed(root/name,stage/name)
probe="wifi_connection_probe.c" if args.wifi_probe else "registration_probe.c" if args.registration_probe else "recovery_probe.c" if args.db_recovery_probe else "page_cache_probe.c" if args.page_cache_probe else "text_store_probe.c" if args.text_store_probe else "media_reset_probe.c" if args.reset_probe else "media_probe.c"
shutil.copyfile(root/"tools"/probe,stage/"main/main.c")
if args.text_store_probe:
    text=(root/"tests/test_feishu_text_store.c").read_text(encoding="utf-8")
    (stage/"main/text_store_reference.c").write_text(text,encoding="utf-8")
if args.registration_probe:
    text=(root/"tests/test_feishu_registration_protocol.c").read_text(encoding="utf-8")
    text='#undef NDEBUG\n#define main registration_reference\n#include "freertos/FreeRTOS.h"\n#include "freertos/task.h"\n'+text
    text=text.replace('for(unsigned i=0;i<2000;++i)assert(decode(begin,true));',
        'for(unsigned i=0;i<2000;++i) {assert(decode(begin,true));if(i%16==0)vTaskDelay(1);}')
    (stage/"main/registration_reference.c").write_text(text,encoding="utf-8")
    p=stage/"main/CMakeLists.txt"
    p.write_text(p.read_text(encoding="utf-8").replace('SRCS "main.c"','SRCS "main.c" "registration_reference.c"'),encoding="utf-8")
if args.wifi_probe:
    p=stage/"main/feishu_network.c"
    text=p.read_text(encoding="utf-8")
    anchor='    s_wifi_started = true;'
    assert text.count(anchor)==1
    text=text.replace(anchor,anchor+'\n    extern bool wifi_probe_early_awake;\n    if(wifi_probe_early_awake)esp_wifi_set_ps(WIFI_PS_NONE);')
    p.write_text(text,encoding="utf-8")
shutil.copyfile(root/"tests/fixtures/jpeg/quadrants.jpg",stage/"main/media-fixture.jpg")
if args.persistent_journal:
    p=stage/"main/feishu_data.c"
    text=p.read_text(encoding="utf-8")
    assert text.count("journal_mode=DELETE;")+text.count("journal_mode=PERSIST;")==1
    p.write_text(text.replace("journal_mode=DELETE;","journal_mode=PERSIST;PRAGMA journal_size_limit=32768;"),encoding="utf-8")
if args.exclusive_locking:
    p=stage/"main/feishu_data.c"
    text=p.read_text(encoding="utf-8")
    assert text.count("PRAGMA cache_size=8;")==1
    if "locking_mode=EXCLUSIVE;" not in text:
        p.write_text(text.replace("PRAGMA cache_size=8;","PRAGMA cache_size=8;PRAGMA locking_mode=EXCLUSIVE;"),encoding="utf-8")
p=stage/"main/CMakeLists.txt"
p.write_text(p.read_text(encoding="utf-8")+'\ntarget_add_binary_data(${COMPONENT_LIB} "media-fixture.jpg" BINARY)\n',encoding="utf-8")
print(stage)
