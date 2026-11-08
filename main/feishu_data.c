#include "feishu_data.h"
#include "sqlite3.h"
#ifdef ESP_PLATFORM
#include "feishu_vfs_write.h"
#endif
#include <limits.h>
#include <string.h>
#include <stdio.h>

static const uint32_t quotas[FEISHU_DATA_KINDS] = {128*1024, 300*1024, 300*1024, 300*1024, 0};
static feishu_data_t *owner;
static sqlite3 *broken_handle;
static int observed(sqlite3 *handle,int rc)
{if((rc&255)==SQLITE_CORRUPT || (rc&255)==SQLITE_NOTADB)broken_handle=handle;return rc;}
bool feishu_data_broken(feishu_data_t *s) {return s && s->db && broken_handle==(sqlite3 *)s->db;}
static int auxiliary_remove(feishu_data_t *s,int64_t id,size_t *bytes);
static int auxiliary_orphans(feishu_data_t *s,size_t *bytes);
static int auxiliary_usage(feishu_data_t *s,uint64_t *bytes);
static sqlite3 *db(feishu_data_t *s) { return (sqlite3 *)s->db; }
bool feishu_data_is_open(const feishu_data_t *s) {return s && s->db;}
bool feishu_data_needs_reopen(const feishu_data_t *s) {(void)s;return false;}
feishu_data_diagnostics_t feishu_data_diagnostics(const feishu_data_t *s)
{
    return s && s->db ? (feishu_data_diagnostics_t){
        sqlite3_extended_errcode((sqlite3 *)s->db),sqlite3_system_errno((sqlite3 *)s->db)}:
        (feishu_data_diagnostics_t){0,0};
}
const char *feishu_data_backend_name(void) {return "sqlite";}
void feishu_data_io_begin(void)
{
#ifdef ESP_PLATFORM
    feishu_vfs_recovery_begin();
#endif
}
void feishu_data_io_end(void)
{
#ifdef ESP_PLATFORM
    feishu_vfs_recovery_end();
#endif
}
static int exec(feishu_data_t *s, const char *sql)
{ return observed(db(s),sqlite3_exec(db(s),sql,NULL,NULL,NULL)); }
/* Every public operation owns its entire transaction. After a transient IO
 * failure, retry rollback before exposing or writing any unfinished state. */
static int restore_transaction(feishu_data_t *s)
{
    if(sqlite3_get_autocommit(db(s)))return SQLITE_OK;
    sqlite3_db_release_memory(db(s));return exec(s,"ROLLBACK");
}
#define RESTORE_TX(s) do {int restored=restore_transaction(s);if(restored!=SQLITE_OK)return restored;} while(0)

static int prepare(feishu_data_t *s, const char *sql, sqlite3_stmt **q)
{
    int rc=observed(db(s),sqlite3_prepare_v2(db(s),sql,-1,q,NULL));
#ifdef FEISHU_DATA_TRACE
    if(rc!=SQLITE_OK)fprintf(stderr,"prepare rc=%d %s\n",rc,sql);
#endif
    return rc;
}
static int data_step(sqlite3_stmt *q)
{
    int rc=observed(sqlite3_db_handle(q),sqlite3_step(q));
#ifdef FEISHU_DATA_TRACE
    if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)fprintf(stderr,"step rc=%d %s\n",rc,sqlite3_sql(q));
#endif
    return rc;
}
static int step_done(sqlite3_stmt *q)
{
    int rc = data_step(q);
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

/* A step never deletes an entire large object while a reader waits on the
 * adapter mutex. Tombstoned versions cannot be pinned or rediscovered. */
static int remove_chunk(feishu_data_t *s,int64_t id,size_t *bytes,bool metadata_first)
{
    if(metadata_first) {
        int aux=auxiliary_remove(s,id,bytes);
        if(aux!=SQLITE_NOTFOUND)return aux;
    }
    sqlite3_stmt *q=NULL;
    int rc=prepare(s,"SELECT rowid,length(data) FROM chunks WHERE object=? LIMIT 1",&q);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int64(q,1,id);
    rc=data_step(q);
    bool found=rc==SQLITE_ROW;
    int64_t row=found ? sqlite3_column_int64(q,0) : 0;
    *bytes=found ? (size_t)sqlite3_column_int(q,1) : 0;
    sqlite3_finalize(q);
    if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)return rc;
    if(!found && !metadata_first) {
        int aux=auxiliary_remove(s,id,bytes);
        if(aux!=SQLITE_NOTFOUND)return aux;
    }
    rc=prepare(s,found ? "DELETE FROM chunks WHERE rowid=?" : "DELETE FROM objects WHERE id=?",&q);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(q,1,found ? row : id);
    return step_done(q);
}
static int page_budget(feishu_data_t *s)
{
    sqlite3_stmt *query=NULL;int rc=prepare(s,"PRAGMA page_size",&query),page=0;
    if(rc!=SQLITE_OK)return rc;
    rc=data_step(query);
    if(rc==SQLITE_ROW && sqlite3_column_type(query,0)==SQLITE_INTEGER) {
        page=sqlite3_column_int(query,0);
        rc=page>=512 && page<=65536 && !(page&(page-1)) ? SQLITE_OK:SQLITE_CORRUPT;
    }
    sqlite3_finalize(query);if(rc!=SQLITE_OK)return rc;
    int limit=640*1024/page;char sql[64];snprintf(sql,sizeof(sql),"PRAGMA max_page_count=%d",limit);
    rc=prepare(s,sql,&query);if(rc!=SQLITE_OK)return rc;
    rc=data_step(query);
    if(rc==SQLITE_ROW)rc=sqlite3_column_int(query,0)<=limit ? SQLITE_OK:SQLITE_FULL;
    sqlite3_finalize(query);return rc;
}

static int configure_vacuum(feishu_data_t *s)
{
    sqlite3_stmt *q=NULL;
    int rc=prepare(s,"PRAGMA auto_vacuum",&q),mode=-1;
    if(rc!=SQLITE_OK)return rc;
    rc=data_step(q);
    if(rc==SQLITE_ROW) {mode=sqlite3_column_int(q,0);rc=SQLITE_OK;}
    sqlite3_finalize(q);
    /* SQLite's setter emits OP_Transaction/OP_SetCookie even if the mode
     * already equals INCREMENTAL, requiring a journal just to reopen. */
    if(rc==SQLITE_OK && mode!=2)rc=exec(s,"PRAGMA auto_vacuum=INCREMENTAL");
    return rc;
}

static int clear_stale_pins(feishu_data_t *s)
{
    sqlite3_stmt *q=NULL;
    int rc=prepare(s,"SELECT 1 FROM objects WHERE pin<>0 LIMIT 1",&q);
    if(rc!=SQLITE_OK)return rc;
    rc=data_step(q);
    bool found=rc==SQLITE_ROW;
    sqlite3_finalize(q);
    if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)return rc;
    return found ? exec(s,"UPDATE objects SET pin=0 WHERE pin<>0"):SQLITE_OK;
}

int feishu_data_open(feishu_data_t *s, const char *path, const char *vfs)
{
    if (!s || !path || s->db) return SQLITE_MISUSE;
    if (owner) return SQLITE_BUSY;
    sqlite3_config(SQLITE_CONFIG_LOOKASIDE, 0, 0);
    sqlite3_hard_heap_limit64(32768);
    sqlite3 *handle = NULL;
    int rc = sqlite3_open_v2(path, &handle, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, vfs);
    s->db = handle;
    if(rc==SQLITE_OK)rc=sqlite3_db_config(handle,SQLITE_DBCONFIG_LOOKASIDE,NULL,0,0);
    if(rc==SQLITE_OK)rc=exec(s,"PRAGMA page_size=512");
    if(rc==SQLITE_OK)rc=configure_vacuum(s);
    if (rc == SQLITE_OK) rc = exec(s,
        "PRAGMA cache_size=8;"
        "PRAGMA locking_mode=EXCLUSIVE;PRAGMA journal_mode=PERSIST;PRAGMA journal_size_limit=32768;"
        "PRAGMA synchronous=FULL;PRAGMA temp_store=MEMORY;PRAGMA automatic_index=OFF;");
    if(rc==SQLITE_OK)rc=page_budget(s);
    if(rc==SQLITE_OK)rc=exec(s,
        "CREATE TABLE IF NOT EXISTS objects(id INTEGER PRIMARY KEY AUTOINCREMENT,k TEXT NOT NULL,kind INT,"
        "committed INT,protected INT,pin INT,raw INT,stored INT,touch INT);"
        "CREATE UNIQUE INDEX IF NOT EXISTS live_key ON objects(k) WHERE committed=1;"
        "CREATE INDEX IF NOT EXISTS cache_lru ON objects(touch);"
        "CREATE TABLE IF NOT EXISTS chunks(object INT,seq INT,codec INT,raw INT,crc INT,data BLOB,PRIMARY KEY(object,seq));"
        /* View nonces are integers; UTF-8 names are bound as opaque BLOBs so
         * numeric display names retain every byte under this column affinity. */
        "CREATE TABLE IF NOT EXISTS text_aux(owner INT,kind INT,k,a,b,v INTEGER,crc INT,PRIMARY KEY(owner,kind,k));"
        "CREATE UNIQUE INDEX IF NOT EXISTS text_aux_map ON text_aux(v) WHERE kind=1 AND k=0;");
    if(rc==SQLITE_OK)rc=clear_stale_pins(s);
    if (rc != SQLITE_OK) {
#ifdef ESP_PLATFORM
        static unsigned failures;
        if(failures++<8)fprintf(stderr,"FEISHU_DB_OPEN rc=%d ext=%d sys=%d heap=%ld\n",rc,
            handle ? sqlite3_extended_errcode(handle):rc,handle ? sqlite3_system_errno(handle):0,(long)sqlite3_memory_used());
#endif
        if(handle) {sqlite3_close(handle);if(broken_handle==handle)broken_handle=NULL;}
        s->db = NULL;
    }
    if (rc == SQLITE_OK) owner = s;
    return rc;
}

int feishu_data_close(feishu_data_t *s)
{
    if (!s || !s->db) return SQLITE_OK;
    int rc = sqlite3_close(db(s));
    if(rc==SQLITE_OK) {if(broken_handle==db(s))broken_handle=NULL;s->db=NULL;owner=NULL;}
    return rc;
}

int feishu_data_id_floor(feishu_data_t *s,uint64_t *floor)
{
    if(!s || !s->db || !floor || *floor>INT64_MAX)return SQLITE_MISUSE;
    RESTORE_TX(s);sqlite3_stmt *q=NULL;
    int rc=prepare(s,"SELECT coalesce(max(id),0),coalesce((SELECT max(seq) FROM sqlite_sequence WHERE name='objects'),0) FROM objects",&q);
    if(rc!=SQLITE_OK)return rc;
    rc=data_step(q);uint64_t current=0;
    if(rc==SQLITE_ROW) {
        for(int i=0;i<2;++i) {
            if(sqlite3_column_type(q,i)!=SQLITE_INTEGER || sqlite3_column_int64(q,i)<0) {rc=SQLITE_CORRUPT;break;}
            uint64_t value=(uint64_t)sqlite3_column_int64(q,i);if(value>current)current=value;
        }
        if(rc==SQLITE_ROW)rc=SQLITE_OK;
    }
    sqlite3_finalize(q);if(rc!=SQLITE_OK)return rc;
    if(*floor<=current) {*floor=current;return SQLITE_OK;}
    rc=prepare(s,"UPDATE sqlite_sequence SET seq=? WHERE name='objects'",&q);
    if(rc==SQLITE_OK) {sqlite3_bind_int64(q,1,(sqlite3_int64)*floor);rc=step_done(q);}
    if(rc==SQLITE_OK && !sqlite3_changes(db(s))) {
        rc=prepare(s,"INSERT INTO sqlite_sequence(name,seq) VALUES('objects',?)",&q);
        if(rc==SQLITE_OK) {sqlite3_bind_int64(q,1,(sqlite3_int64)*floor);rc=step_done(q);}
    }
    return rc;
}
void feishu_data_trim(feishu_data_t *s)
{ if (s && s->db) sqlite3_db_release_memory(db(s)); }

int feishu_data_shrink(feishu_data_t *s)
{
    if(!s || !s->db)return SQLITE_MISUSE;
    RESTORE_TX(s);return exec(s,"PRAGMA incremental_vacuum(8)");
}

static int reclaim_match(feishu_data_t *s,int kind,size_t *bytes)
{
    if (!s || !s->db || !bytes) return SQLITE_MISUSE;
    RESTORE_TX(s);
    /* Reclamation must fit the same hard heap limit even if a preceding
     * streaming reader filled the bounded cache pages. */
    feishu_data_trim(s);
    *bytes = 0;
    if(kind<0 || kind==FEISHU_DATA_METADATA) {
        int aux=auxiliary_orphans(s,bytes);
        if(aux!=SQLITE_NOTFOUND)return aux;
        feishu_data_trim(s);
    }
    sqlite3_stmt *q = NULL;
    int rc=SQLITE_DONE;int64_t id=0;
    if(kind>=0) {
        rc=prepare(s,"SELECT id FROM objects INDEXED BY cache_lru WHERE pin=0 AND (committed<>1 OR protected=0) AND "
            "(kind=? OR (?=0 AND EXISTS(SELECT 1 FROM text_aux WHERE owner=objects.id AND kind IN (1,2)))) "
            "ORDER BY touch,id LIMIT 1",&q);
        if(rc!=SQLITE_OK)return rc;
        sqlite3_bind_int(q,1,kind);sqlite3_bind_int(q,2,kind);rc=data_step(q);
        if(rc==SQLITE_ROW)id=sqlite3_column_int64(q,0);
        sqlite3_finalize(q);
    } else {
        /* A CASE sort creates a temporary B-tree. Separate indexed priority
         * passes keep reclamation inside the unchanged SQLite heap budget. */
        static const char *const candidates[]={
            "SELECT id FROM objects INDEXED BY cache_lru WHERE pin=0 AND committed=3 ORDER BY touch,id LIMIT 1",
            "SELECT id FROM objects INDEXED BY cache_lru WHERE pin=0 AND committed<>1 ORDER BY touch,id LIMIT 1",
            "SELECT id FROM objects INDEXED BY cache_lru WHERE pin=0 AND protected=0 AND committed=1 ORDER BY touch,id LIMIT 1"};
        for(unsigned pass=0;pass<3;++pass) {
            rc=prepare(s,candidates[pass],&q);if(rc!=SQLITE_OK)return rc;
            rc=data_step(q);if(rc==SQLITE_ROW)id=sqlite3_column_int64(q,0);
            sqlite3_finalize(q);if(rc!=SQLITE_DONE)break;
        }
    }
    if (rc != SQLITE_ROW) return rc == SQLITE_DONE ? SQLITE_FULL : rc;
    rc=prepare(s,"UPDATE objects SET committed=3 WHERE id=? AND pin=0",&q);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(q,1,id);rc=step_done(q);
    if(rc!=SQLITE_OK)return rc;
    return remove_chunk(s,id,bytes,kind==FEISHU_DATA_METADATA);
}
int feishu_data_reclaim(feishu_data_t *s,size_t *bytes) {return reclaim_match(s,-1,bytes);}
int feishu_data_reclaim_category(feishu_data_t *s,feishu_data_kind_t kind,size_t *bytes)
{ return kind<0 || kind>=FEISHU_DATA_KINDS ? SQLITE_MISUSE:reclaim_match(s,kind,bytes); }

int feishu_data_begin(feishu_data_t *s, const char *key, feishu_data_kind_t kind,
                       bool protected_data, feishu_data_writer_t *writer)
{
    if (!s || !s->db || !key || !*key || strlen(key)>255 || !writer || kind<0 || kind>=FEISHU_DATA_KINDS) return SQLITE_MISUSE;
    RESTORE_TX(s);
    sqlite3_stmt *q = NULL;
    int rc = prepare(s, "INSERT INTO objects(k,kind,committed,protected,pin,raw,stored,touch) VALUES(?,?,0,?,1,0,0,0)", &q);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int(q, 2, kind);
    sqlite3_bind_int(q, 3, protected_data);
    rc = step_done(q);
    if (rc == SQLITE_OK) {
        memset(writer, 0, sizeof(*writer));
        writer->id = sqlite3_last_insert_rowid(db(s));
        writer->kind = kind;
    }
    return rc;
}

static int usage(feishu_data_t *s, feishu_data_kind_t kind, uint64_t *used)
{
    sqlite3_stmt *q = NULL;
    int rc = prepare(s, "SELECT coalesce(sum(stored),0) FROM objects WHERE kind=? AND (committed=1 OR pin>0)", &q);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int(q, 1, kind);
    rc = data_step(q);
    if(rc==SQLITE_ROW) {
        sqlite3_int64 total=sqlite3_column_int64(q,0);
        if(total<0)rc=SQLITE_CORRUPT;else *used=(uint64_t)total;
    }
    sqlite3_finalize(q);
    if(rc==SQLITE_ROW && kind==FEISHU_DATA_METADATA) {
        uint64_t auxiliary=0;rc=auxiliary_usage(s,&auxiliary);
        if(rc==SQLITE_OK)*used+=auxiliary;
        return rc;
    }
    return rc == SQLITE_ROW ? SQLITE_OK : rc;
}

/* The caller owns the transaction; its temporary cursor is published only
 * after the entire bounded span commits. Packing still uses one 1 KiB block. */
static int append_chunk(feishu_data_t *s,feishu_data_writer_t *w,
                        const uint8_t *data,size_t size,bool compress)
{
    uint8_t packed[FEISHU_BLOCK_BYTES];
    feishu_block_info_t info;
    if (!feishu_block_pack(data, size, compress, packed, sizeof(packed), &info)) return SQLITE_MISUSE;
    if (quotas[w->kind]) {
        uint64_t used=0;
        int rc = usage(s, w->kind, &used);
        if (rc != SQLITE_OK) return rc;
        if (used+info.stored_size > quotas[w->kind]) {w->quota_limited=true;return SQLITE_FULL;}
    }
    sqlite3_stmt *q = NULL;
    int rc=SQLITE_OK;
    rc=prepare(s, "INSERT INTO chunks VALUES(?,?,?,?,?,?)", &q);
    if(rc==SQLITE_OK) {
        sqlite3_bind_int64(q, 1, w->id);
        sqlite3_bind_int(q, 2, (int)w->sequence);
        sqlite3_bind_int(q, 3, info.codec);
        sqlite3_bind_int(q, 4, info.raw_size);
        sqlite3_bind_int64(q, 5, info.crc32);
        sqlite3_bind_blob(q, 6, packed, info.stored_size, SQLITE_STATIC);
        rc=step_done(q);
    }
    if(rc==SQLITE_OK)rc=prepare(s,"UPDATE objects SET raw=?,stored=? WHERE id=? AND committed=0 AND pin=1",&q);
    if(rc==SQLITE_OK) {
        sqlite3_bind_int64(q,1,(sqlite3_int64)(w->raw+size));
        sqlite3_bind_int64(q,2,(sqlite3_int64)(w->stored+info.stored_size));sqlite3_bind_int64(q,3,w->id);
        rc=step_done(q);
        if(rc==SQLITE_OK && sqlite3_changes(db(s))!=1)rc=SQLITE_MISUSE;
    }
    if(rc==SQLITE_OK) {w->raw+=size;w->stored+=info.stored_size;++w->sequence;}
    return rc;
}

int feishu_data_append_span(feishu_data_t *s,feishu_data_writer_t *w,
                             const uint8_t *data,size_t size,bool compress)
{
    if(!s || !s->db || !w || !data || !size || size>8u*FEISHU_BLOCK_BYTES || w->id<=0 ||
       w->kind<0 || w->kind>=FEISHU_DATA_KINDS || w->sequence>INT_MAX-8u ||
       w->raw>INT64_MAX-size)return SQLITE_MISUSE;
    w->quota_limited=false;RESTORE_TX(s);
    feishu_data_writer_t next=*w;
    int rc=exec(s,"BEGIN");if(rc!=SQLITE_OK)return rc;
    for(size_t at=0;at<size && rc==SQLITE_OK;) {
        size_t n=size-at;if(n>FEISHU_BLOCK_BYTES)n=FEISHU_BLOCK_BYTES;
        rc=append_chunk(s,&next,data+at,n,compress);at+=n;
    }
    if(rc==SQLITE_OK)rc=exec(s,"COMMIT");
    if(rc!=SQLITE_OK) {exec(s,"ROLLBACK");w->quota_limited=next.quota_limited;}
    else *w=next;
    return rc;
}
int feishu_data_append(feishu_data_t *s,feishu_data_writer_t *w,
                        const uint8_t *data,size_t size,bool compress)
{
    if(size>FEISHU_BLOCK_BYTES)return SQLITE_MISUSE;
    return feishu_data_append_span(s,w,data,size,compress);
}

static int publish(feishu_data_t *s,feishu_data_writer_t *w,const char *key)
{
    sqlite3_stmt *q = NULL;
    int rc=prepare(s,"UPDATE objects SET k=coalesce(?,k) WHERE id=? AND committed=0 AND pin=1",&q);
    if(rc==SQLITE_OK) {
        if(key)sqlite3_bind_text(q,1,key,-1,SQLITE_STATIC);else sqlite3_bind_null(q,1);
        sqlite3_bind_int64(q,2,w->id);rc=step_done(q);
        if(rc==SQLITE_OK && sqlite3_changes(db(s))!=1)rc=SQLITE_MISUSE;
    }
    if(rc==SQLITE_OK)rc = prepare(s, "UPDATE objects SET committed=2 WHERE committed=1 AND k=(SELECT k FROM objects WHERE id=?)", &q);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, w->id);
        rc = step_done(q);
    }
    if (rc == SQLITE_OK) rc = prepare(s, "UPDATE objects SET committed=1,pin=0,"
        "touch=(SELECT coalesce(max(touch),0)+1 FROM objects) WHERE id=?", &q);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, w->id);
        rc = step_done(q);
    }
    return rc;
}

int feishu_data_commit_pair(feishu_data_t *s,feishu_data_writer_t *first,const char *first_key,
                             feishu_data_writer_t *second,const char *second_key,int64_t release)
{
    if(!s || !s->db || !first || first->id<=0 || release<0 ||
       (first_key && (!*first_key || strlen(first_key)>255)) ||
       (second && (second->id<=0 || second->id==first->id ||
        (second_key && (!*second_key || strlen(second_key)>255)))) ||
       (second && first_key && second_key && !strcmp(first_key,second_key)) ||
       (release && (release==first->id || (second && release==second->id))))return SQLITE_MISUSE;
    RESTORE_TX(s);
    int rc=exec(s,"BEGIN");if(rc!=SQLITE_OK)return rc;
    rc=publish(s,first,first_key);
    if(rc==SQLITE_OK && second)rc=publish(s,second,second_key);
    if(rc==SQLITE_OK && release) {
        sqlite3_stmt *q=NULL;
        rc=prepare(s,"UPDATE objects SET protected=0 WHERE id=? AND committed IN (1,2)",&q);
        if(rc==SQLITE_OK) {sqlite3_bind_int64(q,1,release);rc=step_done(q);}
    }
    if(rc==SQLITE_OK)rc=exec(s,"COMMIT");
    if(rc!=SQLITE_OK)exec(s,"ROLLBACK");
    else {first->id=0;if(second)second->id=0;}
    return rc;
}
int feishu_data_commit_as(feishu_data_t *s,feishu_data_writer_t *w,const char *key)
{ return feishu_data_commit_pair(s,w,key,NULL,NULL,0); }

int feishu_data_commit(feishu_data_t *s,feishu_data_writer_t *w)
{ return feishu_data_commit_as(s,w,NULL); }

int feishu_data_discard_pending(feishu_data_t *s)
{
    if(!s || !s->db)return SQLITE_MISUSE;
    RESTORE_TX(s);
    return exec(s,"UPDATE objects SET pin=0,committed=3 WHERE committed=0");
}
int feishu_data_release_orphan(feishu_data_t *s,const int64_t *active,size_t count,int64_t *released)
{
    if(!s || !s->db || !released || count>64 || (count && !active))return SQLITE_MISUSE;
    RESTORE_TX(s);
    *released=0;sqlite3_stmt *q=NULL;
    int rc=prepare(s,"SELECT id FROM objects WHERE committed=0 AND pin=1",&q);
    if(rc!=SQLITE_OK)return rc;
    int64_t orphan=0;
    while((rc=data_step(q))==SQLITE_ROW) {
        int64_t id=sqlite3_column_int64(q,0);bool owned=false;
        for(size_t i=0;i<count;++i)if(active[i]==id) {owned=true;break;}
        if(!owned) {orphan=id;break;}
    }
    sqlite3_finalize(q);
    if(rc!=SQLITE_ROW && rc!=SQLITE_DONE)return rc;
    if(!orphan)return SQLITE_OK;
    rc=prepare(s,"UPDATE objects SET pin=0,committed=3 WHERE id=? AND committed=0 AND pin=1",&q);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(q,1,orphan);rc=step_done(q);
    if(rc==SQLITE_OK)*released=orphan;
    return rc;
}

int feishu_data_abort(feishu_data_t *s, feishu_data_writer_t *w)
{
    if (!s || !s->db || !w || w->id<=0) return SQLITE_MISUSE;
    RESTORE_TX(s);
    sqlite3_stmt *q=NULL;
    int rc=prepare(s,"UPDATE objects SET pin=0,committed=3 WHERE id=? AND committed=0",&q);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(q,1,w->id);rc=step_done(q);
    if (rc == SQLITE_OK) w->id = 0;
    return rc;
}

int feishu_data_find(feishu_data_t *s, const char *key, feishu_data_object_t *obj)
{
    if (!s || !s->db || !key || !obj) return SQLITE_MISUSE;
    RESTORE_TX(s);
    sqlite3_stmt *q = NULL;
    int rc = prepare(s, "SELECT id,raw,stored,protected FROM objects WHERE k=? AND committed=1", &q);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_text(q, 1, key, -1, SQLITE_STATIC);
    rc = data_step(q);
    if (rc == SQLITE_ROW) {
        obj->id=sqlite3_column_int64(q,0); obj->raw=(uint64_t)sqlite3_column_int64(q,1);
        obj->stored=(uint64_t)sqlite3_column_int64(q,2); obj->protected_data=sqlite3_column_int(q,3)!=0;
    }
    sqlite3_finalize(q);
    return rc == SQLITE_ROW ? SQLITE_OK : rc == SQLITE_DONE ? SQLITE_NOTFOUND : rc;
}

int feishu_data_stat(feishu_data_t *s,int64_t id,const char *key,feishu_data_object_t *obj)
{
    if(!s || !s->db || !key || !obj || id<=0)return SQLITE_MISUSE;
    RESTORE_TX(s);
    sqlite3_stmt *q=NULL;
    int rc=prepare(s,"SELECT raw,stored,protected FROM objects WHERE id=? AND k=? AND committed IN(1,2)",&q);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(q,1,id);sqlite3_bind_text(q,2,key,-1,SQLITE_STATIC);
    rc=data_step(q);
    if(rc==SQLITE_ROW) {
        sqlite3_int64 raw=sqlite3_column_int64(q,0),stored=sqlite3_column_int64(q,1);
        if(raw<0 || stored<0)rc=SQLITE_CORRUPT;
        else {obj->id=id;obj->raw=(uint64_t)raw;obj->stored=(uint64_t)stored;obj->protected_data=sqlite3_column_int(q,2)!=0;rc=SQLITE_OK;}
    } else if(rc==SQLITE_DONE)rc=SQLITE_NOTFOUND;
    sqlite3_finalize(q);
    return rc;
}

int feishu_data_set_protected(feishu_data_t *s,int64_t id,bool protect)
{
    if(!s || !s->db || id<=0)return FEISHU_DATA_MISUSE;
    RESTORE_TX(s);
    sqlite3_stmt *q=NULL;int rc=prepare(s,"UPDATE objects SET protected=? WHERE id=? AND committed IN(1,2)",&q);
    if(rc==SQLITE_OK){
        sqlite3_bind_int(q,1,protect);sqlite3_bind_int64(q,2,id);rc=data_step(q);
        if(rc==SQLITE_DONE)rc=sqlite3_changes(s->db) ? SQLITE_OK:SQLITE_NOTFOUND;
    }
    sqlite3_finalize(q);return rc;
}

int feishu_data_read(feishu_data_t *s, int64_t id, unsigned sequence, uint8_t *data, size_t capacity, size_t *size)
{
    if (!s || !s->db || id<=0 || !data || !size || sequence>INT_MAX) return SQLITE_MISUSE;
    RESTORE_TX(s);
    *size = 0;
    sqlite3_stmt *q = NULL;
    int rc = prepare(s, "SELECT codec,raw,crc,data FROM chunks WHERE object=? AND seq=?", &q);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int64(q, 1, id); sqlite3_bind_int(q, 2, (int)sequence);
    rc = data_step(q);
    if (rc == SQLITE_ROW) {
        int raw=sqlite3_column_int(q,1), stored=sqlite3_column_bytes(q,3), codec=sqlite3_column_int(q,0);
        if (raw<=0 || raw>FEISHU_BLOCK_BYTES || stored<=0 || stored>FEISHU_BLOCK_BYTES || codec<0 || codec>1) rc=SQLITE_CORRUPT;
        else {
            feishu_block_info_t info = {.raw_size=(uint16_t)raw,.stored_size=(uint16_t)stored,
                                       .crc32=(uint32_t)sqlite3_column_int64(q,2),.codec=(uint8_t)codec};
            if (!feishu_block_unpack(sqlite3_column_blob(q,3), stored, &info, data, capacity)) rc=SQLITE_CORRUPT;
            else { *size=(size_t)raw; rc=SQLITE_OK; }
        }
    } else if (rc == SQLITE_DONE) rc=SQLITE_NOTFOUND;
    sqlite3_finalize(q);
    return rc;
}

int feishu_data_pin(feishu_data_t *s, int64_t id, bool pin)
{
    if (!s || !s->db || id<=0) return SQLITE_MISUSE;
    RESTORE_TX(s);
    sqlite3_stmt *q = NULL;
    int rc = prepare(s,"UPDATE objects SET pin=?,touch=(SELECT coalesce(max(touch),0)+1 FROM objects) WHERE id=? AND committed IN (1,2)",&q);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int(q,1,pin ? 1:0);sqlite3_bind_int64(q,2,id);rc=step_done(q);
    return rc==SQLITE_OK && sqlite3_changes(db(s))!=1 ? SQLITE_NOTFOUND:rc;
}

/* Canonical auxiliary records protect offsets and names, not just body chunks.
 * A map's AUTOINCREMENT staging id remains its nonce after publication. */
static void map_u64(uint8_t *out,uint64_t value)
{ for(unsigned i=0;i<8;++i)out[i]=(uint8_t)(value>>(i*8)); }
static uint32_t mention_crc(int64_t map,const char *key,const void *value,size_t size)
{
    uint8_t bytes[4+8+2+31+128];size_t key_size=strlen(key);
    memcpy(bytes,value ? "TMN1":"TML1",4);map_u64(bytes+4,(uint64_t)map);
    bytes[12]=(uint8_t)key_size;bytes[13]=(uint8_t)size;
    memcpy(bytes+14,key,key_size);if(size)memcpy(bytes+14+key_size,value,size);
    return feishu_block_crc32(bytes,14+key_size+size);
}
static uint32_t point_crc(int64_t object,int64_t map,uint64_t raw,uint64_t logical)
{
    uint8_t bytes[36];memcpy(bytes,"TSK1",4);map_u64(bytes+4,(uint64_t)object);
    map_u64(bytes+12,(uint64_t)map);map_u64(bytes+20,raw);map_u64(bytes+28,logical);
    return feishu_block_crc32(bytes,sizeof(bytes));
}
static uint32_t view_crc(int64_t object,const feishu_data_text_view_t *view)
{
    uint8_t bytes[40];memcpy(bytes,"TVW2",4);map_u64(bytes+4,(uint64_t)object);
    map_u64(bytes+12,(uint64_t)view->map);map_u64(bytes+20,view->raw);map_u64(bytes+28,view->logical);
    for(unsigned i=0;i<4;++i)bytes[36+i]=(uint8_t)(view->metadata>>(i*8));
    return feishu_block_crc32(bytes,sizeof(bytes));
}
static bool integer(sqlite3_stmt *query,int column,uint64_t *value)
{
    if(sqlite3_column_type(query,column)!=SQLITE_INTEGER || sqlite3_column_int64(query,column)<0)return false;
    *value=(uint64_t)sqlite3_column_int64(query,column);return true;
}
static int staging(feishu_data_t *s,const feishu_data_writer_t *writer,bool map)
{
    if(!writer || writer->id<=0 || writer->kind!=(map ? FEISHU_DATA_METADATA:FEISHU_DATA_TEXT))return SQLITE_MISUSE;
    if(map && (writer->raw || writer->sequence || (writer->stored && (writer->stored<40 || writer->stored>quotas[FEISHU_DATA_METADATA]))))return SQLITE_MISUSE;
    sqlite3_stmt *query=NULL;
    int rc=prepare(s,"SELECT kind,committed,pin,protected,raw,stored FROM objects WHERE id=?",&query);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(query,1,writer->id);rc=data_step(query);
    if(rc==SQLITE_ROW) {
        uint64_t raw,stored;
        rc=sqlite3_column_int(query,0)==(int)writer->kind && sqlite3_column_int(query,1)==0 &&
            sqlite3_column_int(query,2)==1 && sqlite3_column_int(query,3)==0 &&
            integer(query,4,&raw) && integer(query,5,&stored) && raw==writer->raw && stored==writer->stored ? SQLITE_OK:SQLITE_MISUSE;
    } else if(rc==SQLITE_DONE)rc=SQLITE_NOTFOUND;
    sqlite3_finalize(query);return rc;
}
static int auxiliary_usage(feishu_data_t *s,uint64_t *bytes)
{
    sqlite3_stmt *query=NULL;
    int rc=prepare(s,"SELECT coalesce(sum(CASE WHEN typeof(v.crc)='integer' AND v.crc>=0 AND "
        "(v.crc>>32)>=40 AND (v.crc>>32)<=131072 THEN v.crc>>32 ELSE 131072 END),0) "
        "FROM text_aux v JOIN objects o ON o.id=v.owner WHERE v.kind=1 AND v.k=0 AND "
        "o.protected=0 AND (o.committed=1 OR o.pin>0)",&query);
    if(rc!=SQLITE_OK)return rc;
    rc=data_step(query);
    if(rc==SQLITE_ROW)rc=integer(query,0,bytes) ? SQLITE_OK:SQLITE_CORRUPT;
    sqlite3_finalize(query);return rc;
}
int feishu_data_mention_get(feishu_data_t *s,int64_t map,const char *key,void *value,size_t capacity,size_t *size)
{
    if(!s || !s->db || map<=0 || !key || strlen(key)<2 || strlen(key)>31 || !size || (capacity && !value))return SQLITE_MISUSE;
    *size=0;RESTORE_TX(s);sqlite3_stmt *query=NULL;
    int rc=prepare(s,"SELECT v,crc FROM text_aux WHERE owner=? AND kind=0 AND k=?",&query);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(query,1,map);sqlite3_bind_text(query,2,key,-1,SQLITE_STATIC);rc=data_step(query);
    if(rc==SQLITE_ROW) {
        int type=sqlite3_column_type(query,0),length=sqlite3_column_bytes(query,0);uint64_t crc=0;
        const void *bytes=type==SQLITE_NULL ? NULL:sqlite3_column_blob(query,0);
        if(!integer(query,1,&crc) || crc>UINT32_MAX ||
           (type!=SQLITE_NULL && type!=SQLITE_BLOB) || length<0 || length>128 ||
           (type==SQLITE_BLOB && (!length || !bytes)) || crc!=mention_crc(map,key,bytes,(size_t)length))rc=SQLITE_CORRUPT;
        else if(type==SQLITE_NULL)rc=SQLITE_DONE;
        else if((size_t)length>capacity)rc=SQLITE_TOOBIG;
        else {memcpy(value,bytes,(size_t)length);*size=(size_t)length;rc=SQLITE_OK;}
    } else if(rc==SQLITE_DONE)rc=SQLITE_NOTFOUND;
    sqlite3_finalize(query);return rc;
}
int feishu_data_mention_put(feishu_data_t *s,feishu_data_writer_t *map,const char *key,const char *value)
{
    if(!s || !s->db || !map || !key || strlen(key)<2 || strlen(key)>31 ||
       (value && (!*value || strlen(value)>128)))return SQLITE_MISUSE;
    map->quota_limited=false;RESTORE_TX(s);
    int rc=staging(s,map,true);if(rc!=SQLITE_OK)return rc;
    uint8_t previous[128];size_t previous_size=0,size=value ? strlen(value):0;
    rc=feishu_data_mention_get(s,map->id,key,previous,sizeof(previous),&previous_size);
    if(rc==SQLITE_OK || rc==SQLITE_DONE) {
        bool same=value ? rc==SQLITE_OK && size==previous_size && !memcmp(previous,value,size):rc==SQLITE_DONE;
        return same ? SQLITE_OK:SQLITE_CONSTRAINT;
    }
    if(rc!=SQLITE_NOTFOUND)return rc;
    uint64_t used=0;rc=usage(s,FEISHU_DATA_METADATA,&used);if(rc!=SQLITE_OK)return rc;
    size_t reserve=map->stored ? 0:40;
    uint64_t next_stored=map->stored+reserve+14+strlen(key)+size;
    if(used+reserve+14+strlen(key)+size>quotas[FEISHU_DATA_METADATA]) {map->quota_limited=true;return SQLITE_FULL;}
    rc=exec(s,"BEGIN");if(rc!=SQLITE_OK)return rc;
    sqlite3_stmt *query=NULL;
    rc=prepare(s,"INSERT INTO text_aux(owner,kind,k,v,crc) VALUES(?,0,?,?,?)",&query);
    if(rc==SQLITE_OK) {
        sqlite3_bind_int64(query,1,map->id);sqlite3_bind_text(query,2,key,-1,SQLITE_STATIC);
        if(value)sqlite3_bind_blob(query,3,value,(int)size,SQLITE_STATIC);else sqlite3_bind_null(query,3);
        sqlite3_bind_int64(query,4,mention_crc(map->id,key,value,size));rc=step_done(query);
    }
    if(rc==SQLITE_OK) {
        rc=prepare(s,"UPDATE objects SET stored=? WHERE id=? AND committed=0 AND pin=1 AND stored=?",&query);
        if(rc==SQLITE_OK) {
            sqlite3_bind_int64(query,1,(sqlite3_int64)next_stored);sqlite3_bind_int64(query,2,map->id);
            sqlite3_bind_int64(query,3,(sqlite3_int64)map->stored);rc=step_done(query);
            if(rc==SQLITE_OK && sqlite3_changes(db(s))!=1)rc=SQLITE_MISUSE;
        }
    }
    if(rc==SQLITE_OK)rc=exec(s,"COMMIT");
    if(rc!=SQLITE_OK)exec(s,"ROLLBACK");else map->stored=next_stored;
    return rc;
}
int feishu_data_text_index(feishu_data_t *s,const feishu_data_writer_t *body,feishu_data_writer_t *map,
                            const feishu_data_text_point_t *points,size_t count)
{
    if(!s || !s->db || !map || map->id<=0 || !body || body->id==map->id || !points || !count || count>8)return SQLITE_MISUSE;
    map->quota_limited=false;RESTORE_TX(s);int rc=staging(s,body,false);
    if(rc==SQLITE_OK)rc=staging(s,map,true);
    if(rc!=SQLITE_OK)return rc;
    for(size_t i=0;i<count;++i)if(!points[i].raw || !points[i].logical || points[i].raw>body->raw ||
        points[i].logical>INT64_MAX || (i && (points[i].raw<=points[i-1].raw || points[i].logical<=points[i-1].logical)))return SQLITE_MISUSE;
    uint64_t used=0;rc=usage(s,FEISHU_DATA_METADATA,&used);if(rc!=SQLITE_OK)return rc;
    if(used+count*28>quotas[FEISHU_DATA_METADATA]) {map->quota_limited=true;return SQLITE_FULL;}
    rc=exec(s,"BEGIN");if(rc!=SQLITE_OK)return rc;
    for(size_t i=0;i<count && rc==SQLITE_OK;++i) {
        sqlite3_stmt *query=NULL;
        rc=prepare(s,"INSERT INTO text_aux(owner,kind,k,a,crc) VALUES(?,2,?,?,?)",&query);
        if(rc==SQLITE_OK) {
            sqlite3_bind_int64(query,1,body->id);sqlite3_bind_int64(query,2,(sqlite3_int64)points[i].logical);
            sqlite3_bind_int64(query,3,(sqlite3_int64)points[i].raw);
            sqlite3_bind_int64(query,4,point_crc(body->id,map->id,points[i].raw,points[i].logical));rc=step_done(query);
        }
    }
    if(rc==SQLITE_OK) {
        sqlite3_stmt *query=NULL;
        rc=prepare(s,"UPDATE objects SET stored=? WHERE id=? AND committed=0 AND pin=1 AND stored=?",&query);
        if(rc==SQLITE_OK) {
            sqlite3_bind_int64(query,1,(sqlite3_int64)(map->stored+count*28));sqlite3_bind_int64(query,2,map->id);
            sqlite3_bind_int64(query,3,(sqlite3_int64)map->stored);rc=step_done(query);
            if(rc==SQLITE_OK && sqlite3_changes(db(s))!=1)rc=SQLITE_MISUSE;
        }
    }
    if(rc==SQLITE_OK)rc=exec(s,"COMMIT");
    if(rc!=SQLITE_OK)exec(s,"ROLLBACK");else map->stored+=count*28;
    return rc;
}
int feishu_data_text_index_clear(feishu_data_t *s,int64_t object)
{
    if(!s || !s->db || object<=0)return SQLITE_MISUSE;
    RESTORE_TX(s);sqlite3_stmt *query=NULL;
    int rc=prepare(s,"DELETE FROM text_aux WHERE owner=? AND kind=2",&query);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(query,1,object);return step_done(query);
}
int feishu_data_text_info(feishu_data_t *s,int64_t object,uint64_t raw,feishu_data_text_view_t *view)
{
    if(!s || !s->db || object<=0 || !view || raw>INT64_MAX)return SQLITE_MISUSE;
    RESTORE_TX(s);sqlite3_stmt *query=NULL;
    int rc=prepare(s,"SELECT v,a,b,crc FROM text_aux WHERE owner=? AND kind=1 AND k=0",&query);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(query,1,object);rc=data_step(query);
    if(rc==SQLITE_ROW) {
        uint64_t map=0,stored_raw=0,logical=0,crc=0;
        if(!integer(query,0,&map) || !map || map>INT64_MAX || !integer(query,1,&stored_raw) ||
           stored_raw!=raw || !integer(query,2,&logical) || !integer(query,3,&crc) || (crc>>32)<40 || (crc>>32)>quotas[FEISHU_DATA_METADATA])rc=SQLITE_CORRUPT;
        else {
            feishu_data_text_view_t found={(int64_t)map,raw,logical,(uint32_t)(crc>>32)};
            if((uint32_t)crc!=view_crc(object,&found))rc=SQLITE_CORRUPT;
            else {*view=found;rc=SQLITE_OK;}
        }
    } else if(rc==SQLITE_DONE)rc=SQLITE_NOTFOUND;
    sqlite3_finalize(query);return rc;
}
static bool point_from_query(sqlite3_stmt *query,int64_t object,const feishu_data_text_view_t *view,
                              feishu_data_text_point_t *point)
{
    uint64_t raw=0,logical=0,crc=0;
    if(!integer(query,0,&raw) || !integer(query,1,&logical) || !integer(query,2,&crc) || crc>UINT32_MAX ||
       !raw || !logical || raw>view->raw || logical>view->logical || crc!=point_crc(object,view->map,raw,logical))return false;
    *point=(feishu_data_text_point_t){raw,logical};return true;
}
int feishu_data_text_seek(feishu_data_t *s,int64_t object,const feishu_data_text_view_t *view,
                           uint64_t offset,feishu_data_text_point_t *point)
{
    if(!s || !s->db || object<=0 || !view || view->map<=0 || offset>view->logical || !point)return SQLITE_MISUSE;
    RESTORE_TX(s);*point=(feishu_data_text_point_t){0,0};sqlite3_stmt *query=NULL;
    int rc=prepare(s,"SELECT a,k,crc FROM text_aux WHERE owner=? AND kind=2 AND k<=? ORDER BY k DESC LIMIT 1",&query);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(query,1,object);sqlite3_bind_int64(query,2,(sqlite3_int64)offset);rc=data_step(query);
    if(rc==SQLITE_ROW)rc=point_from_query(query,object,view,point) && point->logical<=offset ? SQLITE_OK:SQLITE_CORRUPT;
    else if(rc==SQLITE_DONE)rc=SQLITE_OK;
    sqlite3_finalize(query);if(rc!=SQLITE_OK)return rc;
    feishu_data_text_point_t end={view->raw,view->logical};
    rc=prepare(s,"SELECT a,k,crc FROM text_aux WHERE owner=? AND kind=2 AND k>? ORDER BY k LIMIT 1",&query);
    if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(query,1,object);sqlite3_bind_int64(query,2,(sqlite3_int64)offset);rc=data_step(query);
    if(rc==SQLITE_ROW)rc=point_from_query(query,object,view,&end) ? SQLITE_OK:SQLITE_CORRUPT;
    else if(rc==SQLITE_DONE)rc=SQLITE_OK;
    sqlite3_finalize(query);
    if(rc==SQLITE_OK && (end.raw<point->raw || end.logical<point->logical ||
       end.raw-point->raw>1024+31 || end.logical-point->logical>1024+129))rc=SQLITE_CORRUPT;
    return rc;
}
int feishu_data_text_publish(feishu_data_t *s,feishu_data_writer_t *body,feishu_data_writer_t *map,
                              uint64_t logical,const char *key)
{
    if(!s || !s->db || !body || !map || body->id==map->id || !key || !*key || strlen(key)>255 || logical>INT64_MAX)return SQLITE_MISUSE;
    RESTORE_TX(s);int rc=staging(s,body,false);if(rc==SQLITE_OK)rc=staging(s,map,true);
    if(rc!=SQLITE_OK || map->stored<40)return rc==SQLITE_OK ? SQLITE_MISUSE:rc;
    feishu_data_text_view_t view={map->id,body->raw,logical,(uint32_t)map->stored};
    rc=exec(s,"BEGIN");if(rc!=SQLITE_OK)return rc;
    rc=publish(s,body,key);sqlite3_stmt *query=NULL;
    if(rc==SQLITE_OK)rc=prepare(s,"INSERT INTO text_aux(owner,kind,k,v,a,b,crc) VALUES(?,1,0,?,?,?,?)",&query);
    if(rc==SQLITE_OK) {
        sqlite3_bind_int64(query,1,body->id);sqlite3_bind_int64(query,2,map->id);
        sqlite3_bind_int64(query,3,(sqlite3_int64)body->raw);sqlite3_bind_int64(query,4,(sqlite3_int64)logical);
        sqlite3_bind_int64(query,5,(sqlite3_int64)(((uint64_t)view.metadata<<32)|view_crc(body->id,&view)));rc=step_done(query);
    }
    if(rc==SQLITE_OK)rc=prepare(s,"DELETE FROM objects WHERE id=? AND committed=0 AND pin=1",&query);
    if(rc==SQLITE_OK) {
        sqlite3_bind_int64(query,1,map->id);rc=step_done(query);
        if(rc==SQLITE_OK && sqlite3_changes(db(s))!=1)rc=SQLITE_MISUSE;
    }
    if(rc==SQLITE_OK)rc=exec(s,"COMMIT");
    if(rc!=SQLITE_OK)exec(s,"ROLLBACK");else body->id=map->id=0;
    return rc;
}
/* Select and finalize before opening the delete transaction. Keeping a large
 * correlated DELETE VM and its dirty pager alive together exceeds the fixed
 * 32 KiB SQLite heap on this target. At most eight row ids live on the stack. */
static int auxiliary_delete(feishu_data_t *s,const char *select_sql,const char *delete_sql,
                              int64_t id,bool bind,size_t charge,size_t *bytes)
{
    int64_t ids[8];size_t count=0;sqlite3_stmt *query=NULL;
    int rc=prepare(s,select_sql,&query);if(rc!=SQLITE_OK)return rc;
    if(bind)sqlite3_bind_int64(query,1,id);
    while((rc=data_step(query))==SQLITE_ROW) {
        if(count==8) {rc=SQLITE_CORRUPT;break;}
        ids[count++]=sqlite3_column_int64(query,0);
    }
    sqlite3_finalize(query);
    if(rc!=SQLITE_DONE) {
#ifdef FEISHU_DATA_TRACE
        fprintf(stderr,"aux select rc=%d %s\n",rc,select_sql);
#endif
        return rc;
    }
    if(!count)return SQLITE_NOTFOUND;
    feishu_data_trim(s);rc=exec(s,"BEGIN");if(rc!=SQLITE_OK)return rc;
    rc=prepare(s,delete_sql,&query);size_t removed=0;
    for(size_t i=0;rc==SQLITE_OK && i<count;++i) {
        sqlite3_bind_int64(query,1,ids[i]);rc=data_step(query);
        if(rc==SQLITE_DONE) {removed+=(size_t)sqlite3_changes(db(s));rc=sqlite3_reset(query);}
    }
    if(query)sqlite3_finalize(query);
    if(rc==SQLITE_OK)rc=exec(s,"COMMIT");
    if(rc!=SQLITE_OK) {exec(s,"ROLLBACK");return rc;}
    *bytes=removed*charge;return removed ? SQLITE_OK:SQLITE_NOTFOUND;
}
static int auxiliary_orphans(feishu_data_t *s,size_t *bytes)
{
    int rc=auxiliary_delete(s,"SELECT rowid FROM text_aux WHERE kind IS NULL OR kind NOT IN (0,1,2) OR "
        "(kind=0 AND typeof(k)<>'text') OR (kind=1 AND (typeof(k)<>'integer' OR k<>0)) OR "
        "(kind=2 AND typeof(k)<>'integer') LIMIT 8","DELETE FROM text_aux WHERE rowid=?",0,false,1,bytes);
    if(rc!=SQLITE_NOTFOUND)return rc;
    feishu_data_trim(s);
    rc=auxiliary_delete(s,"SELECT m.rowid FROM text_aux m "
        "LEFT JOIN text_aux v ON v.kind=1 AND v.k=0 AND v.v=m.owner "
        "LEFT JOIN objects o ON o.id=m.owner AND o.committed=0 AND o.pin>0 "
        "WHERE m.kind=0 AND v.owner IS NULL AND o.id IS NULL LIMIT 8",
        "DELETE FROM text_aux WHERE rowid=?",0,false,14,bytes);
    if(rc!=SQLITE_NOTFOUND)return rc;
    feishu_data_trim(s);
    rc=auxiliary_delete(s,"SELECT t.rowid FROM text_aux t "
        "LEFT JOIN text_aux v ON v.owner=t.owner AND v.kind=1 AND v.k=0 "
        "LEFT JOIN objects o ON o.id=t.owner AND o.committed=0 AND o.pin>0 "
        "WHERE t.kind=2 AND v.owner IS NULL AND o.id IS NULL LIMIT 8",
        "DELETE FROM text_aux WHERE rowid=?",0,false,28,bytes);
    if(rc!=SQLITE_NOTFOUND)return rc;
    feishu_data_trim(s);
    return auxiliary_delete(s,"SELECT v.rowid FROM text_aux v LEFT JOIN objects o ON o.id=v.owner "
        "WHERE v.kind=1 AND v.k=0 AND (o.id IS NULL OR o.protected<>0) LIMIT 8",
        "DELETE FROM text_aux WHERE rowid=?",0,false,40,bytes);
}
static int auxiliary_remove(feishu_data_t *s,int64_t id,size_t *bytes)
{
    sqlite3_stmt *query=NULL;uint64_t raw=0;
    int rc=prepare(s,"SELECT raw FROM objects WHERE id=?",&query);if(rc!=SQLITE_OK)return rc;
    sqlite3_bind_int64(query,1,id);rc=data_step(query);
    if(rc==SQLITE_ROW)rc=integer(query,0,&raw) ? SQLITE_OK:SQLITE_CORRUPT;
    else if(rc==SQLITE_DONE)rc=SQLITE_NOTFOUND;
    sqlite3_finalize(query);if(rc!=SQLITE_OK)return rc;
    feishu_data_text_view_t view={0};rc=feishu_data_text_info(s,id,raw,&view);
    if(rc==SQLITE_CORRUPT && sqlite3_errcode(db(s))!=SQLITE_CORRUPT && sqlite3_errcode(db(s))!=SQLITE_NOTADB) {
        /* A bad nonce must never be followed into another body's names. */
        return auxiliary_delete(s,"SELECT rowid FROM text_aux WHERE owner=? AND kind=1 AND k=0",
            "DELETE FROM text_aux WHERE rowid=?",id,true,40,bytes);
    }
    if(rc!=SQLITE_OK && rc!=SQLITE_NOTFOUND)return rc;
    int64_t map=rc==SQLITE_OK ? view.map:id;
    rc=auxiliary_delete(s,"SELECT rowid FROM text_aux WHERE owner=? AND kind=0 LIMIT 8",
        "DELETE FROM text_aux WHERE rowid=?",map,true,14,bytes);
    if(rc!=SQLITE_NOTFOUND)return rc;
    rc=auxiliary_delete(s,"SELECT rowid FROM text_aux WHERE owner=? AND kind=2 LIMIT 8",
        "DELETE FROM text_aux WHERE rowid=?",id,true,28,bytes);
    if(rc!=SQLITE_NOTFOUND)return rc;
    return auxiliary_delete(s,"SELECT rowid FROM text_aux WHERE owner=? AND kind=1 AND k=0",
        "DELETE FROM text_aux WHERE rowid=?",id,true,40,bytes);
}
