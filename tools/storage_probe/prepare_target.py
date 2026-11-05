"""Stage the unchanged product plus an isolated, synthetic SQLite probe."""
import hashlib
import json
import shutil
from pathlib import Path

root = Path(__file__).resolve().parents[2]
stage = root / "build/storage-probe-source"
source = root / "build/sqlite-download"
stage.mkdir(parents=True, exist_ok=True)
for folder in ("main", "components"):
    shutil.copytree(root/folder, stage/folder, dirs_exist_ok=True)
for name in ("CMakeLists.txt", "sdkconfig.defaults", "dependencies.lock", "partitions.csv"):
    shutil.copyfile(root/name, stage/name)
component = stage / "components/sqlite_probe"
component.mkdir(exist_ok=True)
compat = component/"compat"
compat.mkdir(exist_ok=True)
(compat/"assert.h").write_text(
 '#include_next <assert.h>\n'
 '/* IDF may evaluate NDEBUG assertions; SQLite references DEBUG-only names.\n'
 ' * Restore standard NDEBUG semantics for the upstream library only. */\n'
 '#ifdef NDEBUG\n#undef assert\n#define assert(expression) ((void)0)\n#endif\n', encoding="utf-8")
for name in ("sqlite3.c", "sqlite3.h", "test_demovfs.c"):
    shutil.copyfile(source/name, component/name)
for name in ("sqlite_workload.c", "target_probe.c"):
    shutil.copyfile(root/"tools/storage_probe"/name, component/name)

# The upstream demo's no-op truncate must not claim successful durable I/O.
vfs = (component/"test_demovfs.c").read_text(encoding="utf-8")
vfs = '#include <stdio.h>\n' + vfs
vfs = vfs.replace('#include <sys/file.h>', '/* No flock: one owning task/connection only. */')
vfs = vfs.replace('#if 0\n  if( ftruncate', '#if 1\n  if( ftruncate')
# SPIFFS is flat: unlink synchronously updates its metadata; no directory fd
# exists to fsync. Device power-loss semantics remain explicitly unverified.
vfs = vfs.replace('if( rc==0 && dirSync ){', 'if( rc==0 && dirSync && 0 ){')
# IDF VFS dispatches by exact mount prefix; POSIX-style // normalization is
# not provided. The upstream demo otherwise turns /feishu/x into //feishu/x.
vfs = vfs.replace("if( zPath[0]=='/' ){\n    zDir[0] = '\\0';",
                  "if( zPath[0]=='/' ){\n    sqlite3_snprintf(nPathOut, zPathOut, \"%s\", zPath);\n    return SQLITE_OK;")
vfs = vfs.replace('if( p->fd<0 ){', 'if( p->fd<0 ){\n    printf("STORE_PROBE_OPEN errno=%d path=%s flags=%d\\n", errno, zName, oflags);')
vfs = vfs.replace('p->fd = open(zName, oflags, 0600);', '''/* IDF SPIFFS ignores O_CREAT unless combined with EXCL/TRUNC/APPEND.
   * Preserve existing contents, then create exclusively only when missing. */
  if( (oflags&O_CREAT) && !(oflags&O_EXCL) ){
    p->fd = open(zName, oflags & ~O_CREAT, 0600);
    if( p->fd<0 && errno==ENOENT ) p->fd = open(zName, oflags|O_EXCL, 0600);
  }else{
    p->fd = open(zName, oflags, 0600);
  }''')
# SPIFFS seek beyond EOF fails instead of establishing a sparse offset. SQLite
# expects reads there to return zero-filled SHORT_READ, not a fatal I/O error.
anchor = '  ofst = lseek(p->fd, iOfst, SEEK_SET);'
read_start = vfs.index('static int demoRead(')
read_end = vfs.index('\n}', read_start)
part = vfs[read_start:read_end]
part = part.replace(anchor, '''  struct stat info;
  if( fstat(p->fd, &info)!=0 ) return SQLITE_IOERR_FSTAT;
  if( iOfst>=info.st_size ){
    memset(zBuf, 0, iAmt);
    return SQLITE_IOERR_SHORT_READ;
  }
''' + anchor)
vfs = vfs[:read_start]+part+vfs[read_end:]
# Explicitly extend holes for writes, preserving existing bytes.
write_start = vfs.index('static int demoDirectWrite(')
write_end = vfs.index('\n}', write_start)
part = vfs[write_start:write_end]
part = part.replace(anchor, '''  struct stat info;
  if( fstat(p->fd, &info)!=0 ) return SQLITE_IOERR_FSTAT;
  if( iOfst>info.st_size ){
    char zeros[256] = {0};
    off_t end = lseek(p->fd, 0, SEEK_END);
    if( end<0 ) return SQLITE_IOERR_WRITE;
    while( end<iOfst ){
      int amount = iOfst-end < sizeof(zeros) ? (int)(iOfst-end) : sizeof(zeros);
      int wrote = write(p->fd, zeros, amount);
      if( wrote<=0 ) return SQLITE_IOERR_WRITE;
      end += wrote;
    }
  }
''' + anchor)
vfs = vfs[:write_start]+part+vfs[write_end:]
(component/"test_demovfs.c").write_text(vfs, encoding="utf-8")
definitions = [
 "SQLITE_OS_OTHER=1", "SQLITE_THREADSAFE=0", "SQLITE_OMIT_LOAD_EXTENSION",
 "SQLITE_OMIT_WAL", "SQLITE_OMIT_SHARED_CACHE", "SQLITE_OMIT_UTF16",
 "SQLITE_OMIT_DEPRECATED", "SQLITE_OMIT_JSON", "SQLITE_TEMP_STORE=3",
 "SQLITE_DEFAULT_PAGE_SIZE=1024", "SQLITE_DEFAULT_CACHE_SIZE=8",
 "SQLITE_MAX_LENGTH=16384", "SQLITE_MAX_SQL_LENGTH=4096",
 "SQLITE_DEMOVFS_BUFFERSZ=1024", "SQLITE_MAX_MMAP_SIZE=0",
]
(component/"CMakeLists.txt").write_text(
 'idf_component_register(SRCS "sqlite3.c" "test_demovfs.c" "sqlite_workload.c" "target_probe.c"\n'
 ' INCLUDE_DIRS "." "compat" REQUIRES spiffs esp_timer freertos)\n'
 'target_compile_definitions(${COMPONENT_LIB} PRIVATE '+" ".join(definitions)+')\n'
 'target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wno-unused-parameter)\n', encoding="utf-8")
main = stage/"main/main.c"
text = main.read_text(encoding="utf-8")
anchor = "void app_main(void)\n{"
assert anchor in text
text = text.replace(anchor, 'void sqlite_probe_start(void);\n'+anchor)
# Storage-only qualification: the larger image changes linked Wi-Fi function
# sizes beyond this emulator's exact HLE profile. Keep BSP/UI/audio initialized,
# but do not start radio onboarding; networking coexistence is a later gate.
text = text.replace('        product_onboarding_enter(onboarding_complete);',
                    '        /* Storage fixture: no radio onboarding. */')
anchor = '    ESP_LOGI(TAG, "Ready: Button=%d Audio=%d Battery=%d",'
assert anchor in text
text = text.replace(anchor, '    sqlite_probe_start();\n'+anchor)
main.write_text(text, encoding="utf-8")
cmake=stage/"main/CMakeLists.txt"
text=cmake.read_text(encoding="utf-8").replace("REQUIRES bsp ", "REQUIRES sqlite_probe bsp ")
cmake.write_text(text, encoding="utf-8")
provenance={"scope":"emulator-only SQLite backend qualification, not production integration",
 "sqlite_version":"3.54.1", "radio":"not started; emulator profile mismatch; no networking coexistence claim",
 "vfs_changes":["one owning task/connection", "real ftruncate", "flat-SPIFFS unlink semantics"],
 "sources":{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
            for p in [root/"main/main.c", root/"main/CMakeLists.txt", source/"sqlite3.c", source/"sqlite3.h", source/"test_demovfs.c"]}}
(stage/"probe-provenance.json").write_text(json.dumps(provenance,indent=2),encoding="utf-8")
print(stage)
