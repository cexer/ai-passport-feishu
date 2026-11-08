#pragma once
#include "feishu_fdb.h"
#include "esp_err.h"
esp_err_t feishu_flash_port_open(feishu_fdb_io_t *io);
void feishu_flash_port_close(void);
#ifdef FEISHU_STORAGE_AUTOTEST
void feishu_flash_test_faults(int read_after,int write_after,bool partial);
unsigned feishu_flash_test_injected(void);
unsigned feishu_flash_test_after_fault(void);
#endif

#ifdef FEISHU_SEND_PERF_AUTOTEST
typedef struct {uint32_t reads,writes,erases,read_us,write_us,erase_us,mapped_reads,driver_reads,verify_reads;} feishu_flash_profile_t;
feishu_flash_profile_t feishu_flash_profile(void);
#endif

#ifdef FEISHU_ASSISTANT_AUTOTEST
typedef struct {uint32_t calls,yields,idles;} feishu_flash_scheduler_probe_t;
feishu_flash_scheduler_probe_t feishu_flash_scheduler_probe(void);
#endif
