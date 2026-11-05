#include "feishu_cache.h"
#include "sqlite3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void run(void *unused)
{
 (void)unused;int rc=SQLITE_ERROR;sqlite3_file *f=NULL;const char *path="/feishu/vfs_contract";
 if(feishu_cache_init()!=ESP_OK || feishu_cache_mount()!=ESP_OK)goto done;
 sqlite3_initialize();sqlite3_vfs *v=sqlite3_vfs_find("feishu");if(!v)goto done;
 unlink(path);f=sqlite3_malloc(v->szOsFile);if(!f){rc=SQLITE_NOMEM;goto done;}
 memset(f,0,v->szOsFile);rc=v->xOpen(v,path,f,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_MAIN_JOURNAL,NULL);if(rc)goto done;
 unsigned char block[1024];memset(block,'C',sizeof(block));
 rc=f->pMethods->xWrite(f,block,sizeof(block),0);
 if(!rc)rc=f->pMethods->xWrite(f,block,sizeof(block),1024);
 if(!rc)rc=f->pMethods->xSync(f,SQLITE_SYNC_FULL);
 if(!rc)rc=f->pMethods->xWrite(f,block,512,2048);
 if(!rc)rc=f->pMethods->xTruncate(f,1024);
 int close_rc=f->pMethods->xClose(f);f->pMethods=NULL;if(!rc)rc=close_rc;
 if(rc)goto done;
 rc=v->xOpen(v,path,f,SQLITE_OPEN_READWRITE|SQLITE_OPEN_MAIN_DB,NULL);if(rc)goto done;
 sqlite3_int64 size=0;rc=f->pMethods->xFileSize(f,&size);
 printf("VFS_CONTRACT truncate_size=%ld expected=1024\n",(long)size);
 if(!rc && size!=1024)rc=SQLITE_MISMATCH;
 if(!rc){unsigned char readback[1024];rc=f->pMethods->xRead(f,readback,sizeof(readback),0);if(!rc && memcmp(readback,block,sizeof(block)))rc=SQLITE_CORRUPT;}
 if(rc)goto done;
 f->pMethods->xClose(f);f->pMethods=NULL;
 /* An unwritable journal must surface the flush error before truncation. */
 rc=v->xOpen(v,path,f,SQLITE_OPEN_READONLY|SQLITE_OPEN_MAIN_JOURNAL,NULL);if(rc)goto done;
 rc=f->pMethods->xWrite(f,block,512,0);if(rc)goto done;
 int failed=f->pMethods->xTruncate(f,512);
 printf("VFS_CONTRACT flush_failure=%d expected=%d\n",failed,SQLITE_IOERR_WRITE);
 f->pMethods->xClose(f);f->pMethods=NULL;
 if(failed!=SQLITE_IOERR_WRITE){rc=SQLITE_MISMATCH;goto done;}
 rc=v->xOpen(v,path,f,SQLITE_OPEN_READWRITE|SQLITE_OPEN_MAIN_DB,NULL);if(rc)goto done;
 rc=f->pMethods->xFileSize(f,&size);
 if(!rc && size!=1024)rc=SQLITE_MISMATCH;
 if(!rc)rc=f->pMethods->xWrite(f,block,sizeof(block),0);
 if(!rc)rc=f->pMethods->xSync(f,SQLITE_SYNC_FULL);
 printf("VFS_CONTRACT after_failure_write=%d size=%ld\n",rc,(long)size);
done:
 if(f){if(f->pMethods)f->pMethods->xClose(f);sqlite3_free(f);}unlink(path);
 printf("VFS_CONTRACT DONE result=%d\n",rc);vTaskDelete(NULL);
}
void app_main(void){xTaskCreate(run,"vfs_contract",16384,NULL,4,NULL);}
