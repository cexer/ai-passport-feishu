#pragma once
#include "feishu_fdb.h"
const feishu_fdb_io_t *feishu_fdb_host_reset(void);
void feishu_fdb_host_checkpoint(void);
void feishu_fdb_host_restore(void);
void feishu_fdb_host_fault(int read_after,int write_after,bool partial);
bool feishu_fdb_host_tripped(void);
unsigned feishu_fdb_host_reads(void);
unsigned feishu_fdb_host_writes(void);
