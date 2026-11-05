#include "sqlite3.h"
#include "feishu_data.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static sqlite3_vfs vfs;
static sqlite3_vfs *parent;
static int blocked, journals;
static int test_open(sqlite3_vfs *v,const char *name,sqlite3_file *file,int flags,int *out)
{
    (void)v;
    if(flags & SQLITE_OPEN_MAIN_JOURNAL) {
        ++journals;
        if(blocked)return SQLITE_CANTOPEN;
    }
    return parent->xOpen(parent,name,file,flags,out);
}
static int value(sqlite3 *db,const char *sql)
{
    sqlite3_stmt *q=NULL;
    assert(sqlite3_prepare_v2(db,sql,-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW);
    int n=sqlite3_column_int(q,0);
    assert(sqlite3_finalize(q)==SQLITE_OK);
    return n;
}
int main(int argc,char **argv)
{
    assert(argc==2);
    parent=sqlite3_vfs_find(NULL);assert(parent);vfs=*parent;
    vfs.zName="journal-pressure";vfs.pNext=NULL;vfs.xOpen=test_open;
    assert(sqlite3_vfs_register(&vfs,0)==SQLITE_OK);
    feishu_data_t data={0};
    assert(feishu_data_open(&data,argv[1],vfs.zName)==SQLITE_OK);
    assert(value(data.db,"PRAGMA auto_vacuum")==2);
    assert(sqlite3_exec(data.db,"INSERT INTO objects(k,kind,committed,protected,pin,raw,stored,touch) "
        "VALUES('protected-test',4,1,1,0,0,0,0)",NULL,NULL,NULL)==SQLITE_OK);
    assert(feishu_data_close(&data)==SQLITE_OK);

    /* Match the observed physical fault: no existing journal file. */
    sqlite3 *seed=NULL;
    assert(sqlite3_open(argv[1],&seed)==SQLITE_OK);
    assert(sqlite3_exec(seed,"PRAGMA journal_mode=DELETE;UPDATE objects SET touch=1",NULL,NULL,NULL)==SQLITE_OK);
    assert(sqlite3_close(seed)==SQLITE_OK);
    blocked=1;journals=0;
    assert(feishu_data_open(&data,argv[1],vfs.zName)==SQLITE_OK);
    assert(journals==0);
    assert(value(data.db,"SELECT count(*) FROM objects WHERE protected=1")==1);
    /* Reproduce the old startup statement with the actual SQLite pager. */
    assert(sqlite3_exec(data.db,"PRAGMA auto_vacuum=INCREMENTAL",NULL,NULL,NULL)==SQLITE_CANTOPEN);
    assert(journals==1);
    assert(value(data.db,"SELECT pin FROM objects WHERE k='protected-test'")==0);
    assert(feishu_data_close(&data)==SQLITE_OK);
    assert(feishu_data_open(&data,argv[1],vfs.zName)==SQLITE_OK);
    assert(sqlite3_exec(data.db,"UPDATE objects SET pin=1",NULL,NULL,NULL)==SQLITE_CANTOPEN);
    assert(value(data.db,"SELECT pin FROM objects WHERE k='protected-test'")==0);
    assert(feishu_data_close(&data)==SQLITE_OK);

    blocked=0;
    assert(feishu_data_open(&data,argv[1],vfs.zName)==SQLITE_OK);
    assert(sqlite3_exec(data.db,"UPDATE objects SET pin=1",NULL,NULL,NULL)==SQLITE_OK);
    assert(feishu_data_close(&data)==SQLITE_OK);
    blocked=1;
    /* Real stale leases still require durable cleanup; fail without deleting. */
    assert(feishu_data_open(&data,argv[1],vfs.zName)==SQLITE_CANTOPEN && !data.db);
    blocked=0;
    assert(feishu_data_open(&data,argv[1],vfs.zName)==SQLITE_OK);
    assert(value(data.db,"SELECT pin FROM objects WHERE k='protected-test'")==0);
    assert(value(data.db,"SELECT count(*) FROM objects WHERE protected=1")==1);
    sqlite3_db_release_memory(data.db);
    /* Host integrity audit is separate from the firmware's 32 KiB budget;
     * quick_check itself needs extra bytecode/scratch on the Windows VFS. */
    sqlite3_hard_heap_limit64(131072);
    sqlite3_stmt *q=NULL;
    assert(sqlite3_prepare_v2(data.db,"PRAGMA quick_check",-1,&q,NULL)==SQLITE_OK);
    assert(sqlite3_step(q)==SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(q,0),"ok"));
    sqlite3_finalize(q);
    assert(feishu_data_close(&data)==SQLITE_OK);
    assert(sqlite3_vfs_unregister(&vfs)==SQLITE_OK);
    puts("SQLite source/pager pressure: no-write reopen, journal failure, protected data and stale-pin recovery PASS");
    return 0;
}
