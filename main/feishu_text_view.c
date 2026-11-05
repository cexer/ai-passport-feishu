#include "feishu_text_view.h"
#include <string.h>

bool feishu_text_page(const uint8_t *raw,size_t size,uint64_t offset,uint64_t total,
                       uint32_t page,char output[FEISHU_TEXT_PAGE_CAP],size_t *length)
{
    if(!raw || !output || !length)return false;
    *length=0;output[0]=0;
    uint64_t start=(uint64_t)page*FEISHU_TEXT_PAGE_STEP;
    if(start>=total)return start==0 && total==0;
    uint64_t end=start+FEISHU_TEXT_PAGE_STEP;if(end>total)end=total;
    if(offset>start || start-offset>3 || end<offset || end-offset>size)return false;
    size_t begin=(size_t)(start-offset),last=(size_t)(end-offset);
    if(begin>=size || (end<total && last>=size))return false;
    while(begin>0 && (raw[begin]&0xc0)==0x80)--begin;
    if(end<total)while(last>begin && (raw[last]&0xc0)==0x80)--last;
    if(last<begin || last-begin>=FEISHU_TEXT_PAGE_CAP)return false;
    *length=last-begin;memcpy(output,raw+begin,*length);output[*length]=0;
    return true;
}
