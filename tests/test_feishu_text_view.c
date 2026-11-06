#include "feishu_text_view.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t text[4096];size_t total=0;
    for(unsigned i=0;i<200;++i) {
        const char *value=(i%2) ? "abc\n" : "中文😀";
        size_t n=strlen(value);memcpy(text+total,value,n);total+=n;
    }
    uint8_t joined[4096];size_t count=0;
    for(uint32_t page=0;(uint64_t)page*FEISHU_TEXT_PAGE_STEP<total;++page) {
        uint64_t start=(uint64_t)page*FEISHU_TEXT_PAGE_STEP;
        uint64_t offset=start>3 ? start-3 : 0;
        size_t n=FEISHU_TEXT_PAGE_CAP;if(n>total-offset)n=total-(size_t)offset;
        char out[FEISHU_TEXT_PAGE_CAP];size_t used;
        assert(feishu_text_page(text+offset,n,offset,total,page,out,&used));
        assert(used && strlen(out)==used && ((uint8_t)out[0]&0xc0)!=0x80);
        memcpy(joined+count,out,used);count+=used;
    }
    assert(count==total && !memcmp(joined,text,total));
    char out[FEISHU_TEXT_PAGE_CAP];size_t used;
    assert(feishu_text_page(text,0,0,0,0,out,&used) && used==0);
    assert(!feishu_text_page(text,1,0,FEISHU_TEXT_PAGE_STEP*2,0,out,&used));
    assert(!feishu_text_page(text,FEISHU_TEXT_PAGE_STEP,0,FEISHU_TEXT_PAGE_STEP*2,1,out,&used));
    puts("text page boundaries: PASS");
    return 0;
}
