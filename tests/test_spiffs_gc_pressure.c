#include "spiffs.h"
#include "spiffs_nucleus.h"
#include <assert.h>
static unsigned char image[1048576],work[256],lu[256],cache[8192];
static unsigned writes,reads;
static int fail_read;
void spiffs_api_lock(spiffs *fs){(void)fs;}
void spiffs_api_unlock(spiffs *fs){(void)fs;}
static s32_t read_flash(spiffs *fs,u32_t addr,u32_t n,u8_t *dst){(void)fs;++reads;if(fail_read)return SPIFFS_ERR_INTERNAL;assert(addr+n<=sizeof(image));memcpy(dst,image+addr,n);return SPIFFS_OK;}
static s32_t write_flash(spiffs *fs,u32_t a,u32_t n,u8_t *s){(void)fs;(void)a;(void)n;(void)s;++writes;return SPIFFS_ERR_INTERNAL;}
static void make_block(unsigned block,unsigned deleted,unsigned groups)
{
    for(unsigned i=0;i<15;++i) {
        spiffs_obj_id oid=i<deleted ? SPIFFS_OBJ_ID_DELETED:1;
        memcpy(image+block*4096+i*2,&oid,2);
        spiffs_page_header h={0};h.obj_id=1;
        h.span_ix=103+124*((i-deleted)%groups);
        memcpy(image+(block*16+i+1)*256,&h,sizeof(h));
    }
}
int main(void)
{
    memset(image,255,sizeof(image));
    make_block(10,6,6); /* old score prefers six deletions, but six extra indices */
    make_block(20,5,3); /* five deletions, only three extra index rewrites */
    spiffs fs={0};fs.cfg.phys_size=sizeof(image);fs.cfg.phys_erase_block=4096;
    fs.cfg.log_block_size=4096;fs.cfg.log_page_size=256;
    fs.cfg.hal_read_f=read_flash;fs.cfg.hal_write_f=write_flash;
    fs.cache=cache;fs.cache_size=sizeof(cache);spiffs_cache_init(&fs);fs.block_count=256;fs.work=work;fs.lu_work=lu;fs.free_blocks=1;
    fs.stats_p_allocated=2735;fs.stats_p_deleted=1076;
    spiffs_block_ix *candidates=NULL;int count=0;
    assert(spiffs_gc_find_candidate(&fs,&candidates,&count,1)==SPIFFS_OK);
    assert(count>0 && candidates[0]==20 && writes==0);
    /* Non-pressure selection keeps the upstream wear-leveling heuristic. */
    fs.stats_p_allocated=100;fs.stats_p_deleted=100;fs.free_blocks=8;
    assert(spiffs_gc_find_candidate(&fs,&candidates,&count,0)==SPIFFS_OK);
    assert(candidates[0]==10 && writes==0);
    /* All useful candidates can have non-positive gain. Empty erased
     * blocks must not outrank them or inflate the free-block counter. */
    memset(image,255,sizeof(image));make_block(10,2,5);make_block(20,1,2);
    spiffs_cache_init(&fs);fs.free_blocks=1;fs.stats_p_allocated=2735;fs.stats_p_deleted=1076;
    assert(spiffs_gc_find_candidate(&fs,&candidates,&count,1)==SPIFFS_OK);
    assert(count==2 && candidates[0]==20 && writes==0);
    fail_read=1;
    assert(spiffs_gc_find_candidate(&fs,&candidates,&count,1)==SPIFFS_ERR_INTERNAL);
    assert(writes==0 && reads>0);
    puts("SPIFFS GC: positive net gain under pressure, original normal heuristic, read failure without writes PASS");
    return 0;
}
