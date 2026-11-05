idf_component_register(SRCS "feishu_fdb.c" "vendor/fdb.c" "vendor/fdb_kvdb.c"
    "vendor/fdb_utils.c" "vendor/fdb_read_cache.c"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "vendor")

option(FEISHU_FDB_NAME_FILTER "Use the qualified fixed 1 KiB name filter" ON)
if(FEISHU_FDB_NAME_FILTER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FDB_USING_NAME_FILTER=1)
endif()
