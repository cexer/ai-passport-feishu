#include "feishu_feedback.h"
#include "feishu_assistant_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    feishu_sound_settings_t s={true,true,true};
    for(unsigned k=0;k<3;++k){
        assert(feishu_feedback_enabled(&s,(feishu_sound_t)k));
        unsigned count=feishu_feedback_samples((feishu_sound_t)k);assert(count && count<=16000/10);
        assert(!feishu_feedback_sample((feishu_sound_t)k,count));
        assert(feishu_feedback_fresh((feishu_sound_t)k,0xfffffff0u,20));
        assert(!feishu_feedback_fresh((feishu_sound_t)k,0,2000));
        assert(feishu_feedback_sample((feishu_sound_t)k,0)==0);
    }
    s.key=false;assert(!feishu_feedback_enabled(&s,FEISHU_SOUND_KEY));
    assert(feishu_feedback_enabled(&s,FEISHU_SOUND_SUCCESS));
    s.success=false;s.failure=false;assert(!feishu_feedback_enabled(&s,FEISHU_SOUND_FAILURE));
    assert(!feishu_feedback_samples((feishu_sound_t)4));
    assert(feishu_feedback_samples(FEISHU_SOUND_BOOT)==9200);
    assert(!feishu_feedback_enabled(&s,FEISHU_SOUND_BOOT));
    s.success=true;assert(feishu_feedback_enabled(&s,FEISHU_SOUND_BOOT));
    unsigned nonzero=0;
    for(unsigned i=0;i<9440;++i){
        int v=feishu_feedback_sample(FEISHU_SOUND_BOOT,i);
        assert(v>=-5000 && v<=5000);nonzero+=v!=0;
        if(i>=9200)assert(v==0);
    }
    assert(nonzero>7000 && nonzero<8500);
    const unsigned gaps[]={1360,2960,4560,8960};
    for(unsigned n=0;n<4;++n)for(unsigned j=0;j<240;++j)assert(!feishu_feedback_sample(FEISHU_SOUND_BOOT,gaps[n]+j));
    assert(!feishu_feedback_sample(FEISHU_SOUND_BOOT,0));
    /* Key tone itself spans at most two capture frames; after the short VAD
     * guard the six-frame preroll still retains a human command's beginning. */
    assert(feishu_feedback_samples(FEISHU_SOUND_KEY)<320);
    puts("feedback independent toggles/envelopes/bounds/expiry/rollover: PASS");
}
