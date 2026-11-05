#include "feishu_asr_batch.h"
#include <string.h>
bool feishu_asr_batch(unsigned blocks,size_t bytes,void *zero_scratch,
    feishu_asr_next_t next,feishu_asr_release_t release,void *source,
    feishu_asr_batch_write_t write,void *writer,bool *done)
{
    if(!blocks || blocks>8 || !bytes || bytes>6400 || blocks*bytes>25600 || !zero_scratch || !next || !release || !write || !done)return false;
    *done=false;
    for(unsigned block=0;block<blocks;++block) {
        const void *data=NULL;int slot=next(source,&data);
        if(slot==-1) {
            *done=true;memset(zero_scratch,0,bytes);
            for(;block<blocks;++block)if(!write(writer,zero_scratch,bytes))return false;
            return true;
        }
        if(slot<0)return false;
        bool ok=data && write(writer,data,bytes);
        /* Socket write consumed the bytes, even on its failed/partial path.
         * Release before waiting for another capture block or HTTP response. */
        release(source,slot);
        if(!ok)return false;
    }
    return true;
}
