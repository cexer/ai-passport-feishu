#pragma once
#include "feishu_root.h"
/* One obsolete raw configuration chunk per serialized recovery step.
 * Live mirrored generations and unrecognized records are never removed. */
int feishu_root_flashdb_collect(size_t *bytes);
