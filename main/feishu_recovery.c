#include "feishu_recovery.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

enum {MARKER_KEY=0x7f,CLEAR=0,MOVING=1,HELD=2};
typedef struct {uint32_t generation,phase,journal;uint64_t floor;} marker_t;
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t value)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i));}
static bool path(const feishu_recovery_t *r,const char *name,char out[256])
{
    if(!r || !r->root.directory)return false;
    int n=snprintf(out,256,"%s/%s",r->root.directory,name);return n>0 && n<256;
}
static int exists(const char *name,bool *found)
{
    struct stat state;errno=0;
    if(!stat(name,&state)) {*found=true;return FEISHU_ROOT_OK;}
    *found=false;return errno==ENOENT ? FEISHU_ROOT_OK:FEISHU_ROOT_IO;
}
static int archives(const feishu_recovery_t *r,bool *found)
{
    *found=false;
#ifdef _WIN32
    char pattern[256];if(!path(r,"objects.q*",pattern))return FEISHU_ROOT_INVALID;
    struct _finddata_t entry;errno=0;intptr_t handle=_findfirst(pattern,&entry);
    if(handle==-1)return errno==ENOENT ? FEISHU_ROOT_OK:FEISHU_ROOT_IO;
    *found=true;return _findclose(handle) ? FEISHU_ROOT_IO:FEISHU_ROOT_OK;
#else
    DIR *directory=opendir(r->root.directory);if(!directory)return FEISHU_ROOT_IO;
    struct dirent *entry;unsigned count=0;errno=0;
    while((entry=readdir(directory))) {
        if(++count>4096) {closedir(directory);return FEISHU_ROOT_INVALID;}
        if(!strncmp(entry->d_name,"objects.q",9)) {*found=true;break;}
    }
    int rc=errno ? FEISHU_ROOT_IO:FEISHU_ROOT_OK;
    if(closedir(directory))rc=FEISHU_ROOT_IO;
    return rc;
#endif
}
static int write_marker(const feishu_recovery_t *r,const marker_t *m)
{
    uint8_t bytes[24]={0};memcpy(bytes,"FDQ1",4);
    put32(bytes+4,m->generation);put32(bytes+8,m->phase);put32(bytes+12,m->journal);
    put32(bytes+16,(uint32_t)m->floor);put32(bytes+20,(uint32_t)(m->floor>>32));
    feishu_root_part_t part={bytes,sizeof(bytes)};
    return feishu_root_write(&r->root,MARKER_KEY,&part,1);
}
static int read_marker(const feishu_recovery_t *r,marker_t *m)
{
    size_t total=0;int rc=feishu_root_read(&r->root,MARKER_KEY,0,NULL,0,&total);
    if(rc==FEISHU_ROOT_MISSING || rc==FEISHU_ROOT_CORRUPT) {
        bool found=false;int scan=archives(r,&found);if(scan)return scan;
        if(found)return FEISHU_ROOT_CORRUPT;
        /* A first marker can be partially written before any rename starts.
         * Only our reserved marker files may be reset, after proving there
         * are no archived databases. Other root records are never touched. */
        if(rc==FEISHU_ROOT_CORRUPT)for(unsigned i=0;i<2;++i) {
            char file[256];if(!path(r,i ? "cfg7fb":"cfg7fa",file))return FEISHU_ROOT_INVALID;
            errno=0;if(remove(file) && errno!=ENOENT)return FEISHU_ROOT_IO;
        }
        memset(m,0,sizeof(*m));return write_marker(r,m);
    }
    if(rc)return rc;
    if(total!=24)return FEISHU_ROOT_INVALID;
    uint8_t bytes[24];rc=feishu_root_read(&r->root,MARKER_KEY,0,bytes,sizeof(bytes),&total);if(rc)return rc;
    if(memcmp(bytes,"FDQ1",4))return FEISHU_ROOT_INVALID;
    m->generation=get32(bytes+4);m->phase=get32(bytes+8);m->journal=get32(bytes+12);
    m->floor=(uint64_t)get32(bytes+16)|((uint64_t)get32(bytes+20)<<32);
    if(m->phase>HELD || m->journal>1 || m->floor>INT64_MAX ||
       (!m->phase && (m->generation || m->journal || m->floor)) || (m->phase && !m->generation))return FEISHU_ROOT_CORRUPT;
    return FEISHU_ROOT_OK;
}
static bool saved_path(const feishu_recovery_t *r,uint32_t generation,bool journal,char out[256])
{
    char name[40];snprintf(name,sizeof(name),"objects.q%08x.db%s",(unsigned)generation,journal ? "-journal":"");
    return path(r,name,out);
}
static int move_one(const feishu_recovery_t *r,const char *active,const char *saved,bool required)
{
    bool a=false,b=false;int rc=exists(active,&a);if(rc)return rc;
    rc=exists(saved,&b);if(rc)return rc;
    if(!required)return !a && !b ? FEISHU_ROOT_OK:FEISHU_ROOT_CORRUPT;
    if(a && b)return FEISHU_ROOT_CORRUPT;
    if(b)return FEISHU_ROOT_OK;
    if(!a)return FEISHU_ROOT_CORRUPT;
    return (r->rename_file ? r->rename_file(active,saved):rename(active,saved)) ? FEISHU_ROOT_IO:FEISHU_ROOT_OK;
}
int feishu_recovery_resume(const feishu_recovery_t *r,feishu_recovery_info_t *info)
{
    if(!r || !info || !r->root.directory)return FEISHU_ROOT_INVALID;
    memset(info,0,sizeof(*info));marker_t m;int rc=read_marker(r,&m);if(rc)return rc;
    if(m.phase==CLEAR)return FEISHU_ROOT_OK;
    char active[256],saved[256];
    if(m.phase==MOVING) {
        if(!path(r,"objects.db-journal",active) || !saved_path(r,m.generation,true,saved))return FEISHU_ROOT_INVALID;
        rc=move_one(r,active,saved,m.journal!=0);if(rc)return rc;
        if(!path(r,"objects.db",active) || !saved_path(r,m.generation,false,saved))return FEISHU_ROOT_INVALID;
        rc=move_one(r,active,saved,true);if(rc)return rc;
        m.phase=HELD;rc=write_marker(r,&m);if(rc)return rc;
    } else {
        bool found=false;if(!saved_path(r,m.generation,false,saved))return FEISHU_ROOT_INVALID;
        rc=exists(saved,&found);if(rc || !found)return rc ? rc:FEISHU_ROOT_CORRUPT;
        if(m.journal) {
            if(!saved_path(r,m.generation,true,saved))return FEISHU_ROOT_INVALID;
            rc=exists(saved,&found);if(rc || !found)return rc ? rc:FEISHU_ROOT_CORRUPT;
        }
    }
    info->generation=m.generation;info->id_floor=m.floor;info->held=true;return FEISHU_ROOT_OK;
}
int feishu_recovery_isolate(const feishu_recovery_t *r,uint64_t floor,feishu_recovery_info_t *info)
{
    if(floor>INT64_MAX)return FEISHU_ROOT_INVALID;
    feishu_recovery_info_t previous;int rc=feishu_recovery_resume(r,&previous);if(rc)return rc;
    if(previous.generation==UINT32_MAX)return FEISHU_ROOT_FULL;
    marker_t m={.generation=previous.generation+1,.phase=MOVING,.floor=floor>previous.id_floor ? floor:previous.id_floor};
    char active[256],saved[256];bool found=false;
    for(unsigned i=0;i<2;++i) {
        if(!saved_path(r,m.generation,i!=0,saved))return FEISHU_ROOT_INVALID;
        rc=exists(saved,&found);if(rc || found)return rc ? rc:FEISHU_ROOT_CORRUPT;
    }
    if(!path(r,"objects.db",active))return FEISHU_ROOT_INVALID;
    rc=exists(active,&found);if(rc || !found)return rc ? rc:FEISHU_ROOT_MISSING;
    if(!path(r,"objects.db-journal",active))return FEISHU_ROOT_INVALID;
    rc=exists(active,&found);if(rc)return rc;m.journal=found;
    rc=write_marker(r,&m);return rc ? rc:feishu_recovery_resume(r,info);
}
