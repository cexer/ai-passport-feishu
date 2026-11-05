/* Synthetic comparison profile; upstream FlashDB is kept unchanged. */
#ifndef FEISHU_PROBE_FDB_CFG_H
#define FEISHU_PROBE_FDB_CFG_H
#define FDB_USING_KVDB
#define FDB_USING_FILE_LIBC_MODE
#define FDB_USING_NATIVE_ASSERT
#define FDB_WRITE_GRAN 8
#define FDB_KV_CACHE_TABLE_SIZE 16
#define FDB_SECTOR_CACHE_TABLE_SIZE 4
#define FDB_USING_READ_CACHE 0
#endif
