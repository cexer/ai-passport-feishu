/* Synthetic backend qualification, not the product data layer. */
#include "flashdb.h"
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static struct fdb_kvdb database;
static unsigned char block[2048];
#ifdef FDBPROBE_FAULTS
extern void probe_flash_reset(void);
extern void probe_flash_fault(int, bool);
#endif

static int open_database(const char *path)
{
    uint32_t sector = 16384, maximum = 640 * 1024;
    struct fdb_default_kv defaults = {0};
    memset(&database, 0, sizeof(database));
    fdb_kvdb_control(&database, FDB_KVDB_CTRL_SET_SEC_SIZE, &sector);
#ifdef FDB_USING_FILE_MODE
    bool file_mode = true;
    fdb_kvdb_control(&database, FDB_KVDB_CTRL_SET_MAX_SIZE, &maximum);
    fdb_kvdb_control(&database, FDB_KVDB_CTRL_SET_FILE_MODE, &file_mode);
#else
    (void)maximum; /* Raw FAL qualification partition supplies the same limit. */
#endif
    return fdb_kvdb_init(&database, "probe", path, &defaults, NULL);
}

static int put(const char *key, const void *value, size_t size)
{
    struct fdb_blob blob;
    return fdb_kv_set_blob(&database, key, fdb_blob_make(&blob, value, size));
}

static int matches(const char *key, const char *expected)
{
    char value[64] = {0};
    struct fdb_blob blob;
    size_t size = fdb_kv_get_blob(&database, key, fdb_blob_make(&blob, value, sizeof(value)));
    return size == strlen(expected) + 1 && !memcmp(value, expected, size);
}

#define CHECK(call) do { rc = (call); if (rc != FDB_NO_ERR) goto done; } while (0)
#define EXPECT(test) do { if (!(test)) { rc = -1; goto done; } } while (0)

int flashdb_probe_run(const char *path)
{
    int rc = 0, full = 0, rows = 0, opened = 0;
    char key[32];
    const char *phase = "open";
    memset(block, 0xa5, sizeof(block));
    CHECK(open_database(path));
    opened = 1;
    CHECK(put("owner", "synthetic-owner", sizeof("synthetic-owner")));
    CHECK(put("pinned", "keep-active", sizeof("keep-active")));
    phase = "fill";
    for (int i = 0; i < 512; ++i) {
        snprintf(key, sizeof(key), "cache-%04d", i);
        full = put(key, block, sizeof(block));
        if (full != FDB_NO_ERR) break;
        rows++;
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
    EXPECT(full == FDB_SAVED_FULL);
    phase = "evict-and-recover";
    for (int i = 0; i < 16 && i < rows; ++i) {
        snprintf(key, sizeof(key), "cache-%04d", i);
        CHECK(fdb_kv_del(&database, key));
    }
    CHECK(put("cache-new", block, sizeof(block)));
    CHECK(put("token", "after-full", sizeof("after-full")));
    EXPECT(matches("owner", "synthetic-owner"));
    EXPECT(matches("pinned", "keep-active"));
    phase = "reopen";
    CHECK(fdb_kvdb_deinit(&database));
    opened = 0;
    CHECK(open_database(path));
    opened = 1;
    EXPECT(matches("token", "after-full"));
    EXPECT(matches("pinned", "keep-active"));
    CHECK(put("post-recovery", "ok", sizeof("ok")));
#ifdef FDBPROBE_FAULTS
    phase = "write-fault-recovery";
    for (int partial = 0; partial < 2; ++partial) {
        for (int cut = 0; cut < 12; ++cut) {
            CHECK(put("fault-token", "old-value", sizeof("old-value")));
            probe_flash_fault(cut, partial != 0);
            int fault_rc = put("fault-token", "new-value", sizeof("new-value"));
            probe_flash_fault(-1, false);
            CHECK(fdb_kvdb_deinit(&database));
            opened = 0;
            CHECK(open_database(path));
            opened = 1;
            EXPECT(matches("fault-token", "old-value") || matches("fault-token", "new-value"));
            EXPECT(matches("owner", "synthetic-owner") && matches("pinned", "keep-active"));
            CHECK(put("recovered-token", "ok", sizeof("ok")));
            EXPECT(matches("recovered-token", "ok"));
            printf("FLASHDB_PROBE_FAULT cut=%d partial=%d operation_rc=%d recovery=pass\n", cut, partial, fault_rc);
        }
    }
#endif
    phase = "complete";
done:
    if (opened) fdb_kvdb_deinit(&database);
    printf("FLASHDB_PROBE phase=%s rc=%d rows=%d full_rc=%d db_struct=%u block=%u write_gran=%d\n",
           phase, rc, rows, full, (unsigned)sizeof(database), (unsigned)sizeof(block), FDB_WRITE_GRAN);
    return rc;
}

#ifdef FDBPROBE_HOST_MAIN
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) return 2;
#ifdef FDBPROBE_FAULTS
    probe_flash_reset();
#endif
    return flashdb_probe_run(argv[1]) == 0 ? 0 : 1;
}
#endif
