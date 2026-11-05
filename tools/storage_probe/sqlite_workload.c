/* Synthetic backend qualification. Not the product storage implementation. */
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static int sql(sqlite3 *db, const char *text)
{
    char *error = NULL;
    int rc = sqlite3_exec(db, text, NULL, NULL, &error);
    sqlite3_free(error);
    return rc;
}

static int scalar(sqlite3 *db, const char *text, int *value)
{
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(db, text, -1, &statement, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            *value = sqlite3_column_int(statement, 0);
            rc = SQLITE_OK;
        }
    }
    sqlite3_finalize(statement);
    return rc;
}

#define REQUIRE_OK(call) do { rc = (call); if (rc != SQLITE_OK) goto done; } while (0)
#define REQUIRE(test) do { if (!(test)) { rc = SQLITE_ERROR; goto done; } } while (0)

int sqlite_probe_run(const char *path, const char *vfs, int budget)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *insert = NULL;
    unsigned char block[2048];
    sqlite3_int64 used = 0, peak = 0;
    int rc = SQLITE_OK, full_rc = SQLITE_OK, oom_rc = SQLITE_OK;
    int value = 0, rows = 0;
    const char *phase = "initialize";

    memset(block, 0xa5, sizeof(block));
    sqlite3_config(SQLITE_CONFIG_LOOKASIDE, 0, 0);
    sqlite3_hard_heap_limit64(budget);
    sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &used, &peak, 1);
    REQUIRE_OK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, vfs));
    phase = "schema";
    REQUIRE_OK(sql(db, "PRAGMA page_size=1024; PRAGMA cache_size=8;"
                       "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
                       "PRAGMA temp_store=MEMORY; PRAGMA max_page_count=640;"
                       "CREATE TABLE IF NOT EXISTS config(k TEXT PRIMARY KEY,v BLOB);"
                       "CREATE TABLE IF NOT EXISTS cache(id INTEGER PRIMARY KEY,pin INT,payload BLOB);"
                       "INSERT OR REPLACE INTO config VALUES('owner','synthetic-owner');"
                       "INSERT OR REPLACE INTO cache VALUES(0,1,x'01020304');"));
    phase = "rollback";
    REQUIRE_OK(sql(db, "BEGIN; INSERT INTO config VALUES('uncommitted','never-visible'); ROLLBACK;"));
    REQUIRE_OK(scalar(db, "SELECT count(*) FROM config WHERE k='uncommitted'", &value));
    REQUIRE(value == 0);
    phase = "fill";
    REQUIRE_OK(sqlite3_prepare_v2(db, "INSERT INTO cache(pin,payload) VALUES(0,?1)", -1, &insert, NULL));
    for (int i = 0; i < 512; ++i) {
        REQUIRE_OK(sqlite3_bind_blob(insert, 1, block, sizeof(block), SQLITE_STATIC));
        full_rc = sqlite3_step(insert);
        sqlite3_reset(insert);
        sqlite3_clear_bindings(insert);
        if (full_rc != SQLITE_DONE) break;
        rows++;
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
    sqlite3_finalize(insert);
    insert = NULL;
    REQUIRE(full_rc == SQLITE_FULL);
    phase = "evict-and-recover";
    /* Bounded eviction avoids a rollback journal as large as the whole cache. */
    REQUIRE_OK(sql(db, "DELETE FROM cache WHERE pin=0 AND id<=8;"
                       "INSERT INTO cache(pin,payload) VALUES(0,zeroblob(2048));"
                       "INSERT OR REPLACE INTO config VALUES('token','synthetic-refresh-after-full');"));
    REQUIRE_OK(scalar(db, "SELECT count(*) FROM cache WHERE id=0 AND hex(payload)='01020304'", &value));
    REQUIRE(value == 1);
    REQUIRE_OK(scalar(db, "SELECT count(*) FROM config WHERE k IN ('owner','token')", &value));
    REQUIRE(value == 2);
    phase = "nomem";
    sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &used, &peak, 0);
    sqlite3_hard_heap_limit64(used + 1);
    oom_rc = sqlite3_prepare_v2(db, "SELECT payload FROM cache ORDER BY payload", -1, &insert, NULL);
    sqlite3_finalize(insert);
    insert = NULL;
    sqlite3_hard_heap_limit64(budget);
    REQUIRE(oom_rc == SQLITE_NOMEM);
    REQUIRE_OK(scalar(db, "SELECT count(*) FROM config", &value));
    REQUIRE(value == 2);
    phase = "reopen";
    REQUIRE_OK(sqlite3_close(db));
    db = NULL;
    REQUIRE_OK(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, vfs));
    REQUIRE_OK(sql(db, "PRAGMA cache_size=8; PRAGMA synchronous=FULL;"));
    REQUIRE_OK(scalar(db, "SELECT count(*) FROM config WHERE k='token'", &value));
    REQUIRE(value == 1);
    REQUIRE_OK(sql(db, "INSERT OR REPLACE INTO config VALUES('post_recovery','ok');"));
    phase = "complete";
done:
    if (rc != SQLITE_OK && db)
        printf("STORE_PROBE_ERROR code=%d message=%s\n", sqlite3_extended_errcode(db), sqlite3_errmsg(db));
    sqlite3_finalize(insert);
    sqlite3_status64(SQLITE_STATUS_MEMORY_USED, &used, &peak, 0);
    printf("STORE_PROBE budget=%d phase=%s rc=%d rows=%d full_rc=%d oom_rc=%d sqlite_used=%lld sqlite_peak=%lld\n",
           budget, phase, rc, rows, full_rc, oom_rc, (long long)used, (long long)peak);
    if (db) sqlite3_close(db);
    sqlite3_hard_heap_limit64(0);
    sqlite3_shutdown();
    return rc;
}

#ifdef SQLPROBE_HOST_MAIN
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    return sqlite_probe_run(argv[1], NULL, atoi(argv[2])) == SQLITE_OK ? 0 : 1;
}
#endif
