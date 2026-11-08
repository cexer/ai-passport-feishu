#ifndef FEISHU_FDB_CFG_H
#define FEISHU_FDB_CFG_H
#define FDB_USING_KVDB
#define FDB_USING_FAL_MODE
#define FDB_USING_NATIVE_ASSERT
#define FDB_WRITE_GRAN 8
#define FDB_KV_CACHE_TABLE_SIZE 128
#define FDB_SECTOR_CACHE_TABLE_SIZE 4
#define FDB_USING_READ_CACHE 1
#define FDB_READ_CACHE_WINDOW 1024
/* Host/reference default; firmware enables the fixed filter through CMake. */
#ifndef FDB_USING_NAME_FILTER
#define FDB_USING_NAME_FILTER 0
#endif
/* Never log keys, which may contain user metadata. */
/* Cooperative scheduling belongs above the optional read cache. */
void feishu_fdb_cooperate(void);
#define FDB_COOPERATE() feishu_fdb_cooperate()
#define FDB_PRINT(...) ((void)0)
#endif
