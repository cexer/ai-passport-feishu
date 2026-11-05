#pragma once
#include "feishu_fdb.h"
/* The pool owner configures the raw object adapter while its store is closed.
 * Root configuration can use the shared engine before the catalog is opened. */
int feishu_data_flashdb_configure(const feishu_fdb_io_t *io);
/* Explicit preparation only; rejects any KV record already present. */
int feishu_data_flashdb_initialize(void);
