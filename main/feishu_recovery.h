#pragma once
#include "feishu_root.h"
#include <stdbool.h>

/* Preserve failed databases and journals in the same shared filesystem.
 * This module never deletes archives, configuration, or application data. */
typedef struct {
    feishu_root_t root;
    int (*rename_file)(const char *source,const char *destination);
} feishu_recovery_t;
typedef struct {uint32_t generation;uint64_t id_floor;bool held;} feishu_recovery_info_t;

/* Resume interrupted renames before SQLite can open the active DB path.
 * Return FEISHU_ROOT_* codes. The caller holds the shared-pool lock. */
int feishu_recovery_resume(const feishu_recovery_t *recovery,feishu_recovery_info_t *info);
/* Call only for a confirmed SQLite CORRUPT/NOTADB error, with the DB closed.
 * Ordinary FULL/NOMEM/IO errors must not be classified as corruption. */
int feishu_recovery_isolate(const feishu_recovery_t *recovery,uint64_t id_floor,feishu_recovery_info_t *info);
