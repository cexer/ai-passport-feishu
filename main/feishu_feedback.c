#include "feishu_feedback.h"
#include <stddef.h>
#include "feishu_feedback_pcm.inc"
static const struct {const int16_t *pcm;unsigned count;} tones[]={
    {kNav,sizeof(kNav)/sizeof(*kNav)},
    {kConfirm,sizeof(kConfirm)/sizeof(*kConfirm)},
    {kBack,sizeof(kBack)/sizeof(*kBack)}
};
unsigned feishu_feedback_samples(feishu_sound_t k)
{return k==FEISHU_SOUND_BOOT ? 9200u:(unsigned)k<3 ? tones[k].count:0;}
/* Short E5/G5/B5/E6 electronic arpeggio, following play438's note rhythm.
 * Generate only the next sample into the existing 128-sample audio block;
 * no permanent PCM array or extra task/heap buffer is needed. */
static int16_t boot_sample(unsigned i)
{
    static const struct {unsigned hz,ms;} notes[]={{659,85},{784,85},{988,85},{1319,260}};
    for(unsigned n=0;n<4;++n){
        unsigned count=notes[n].ms*16u,span=count+240u;
        if(i>=span){i-=span;continue;}
        if(i>=count)return 0;
        unsigned attack=i<32u ? i:32u;
        unsigned tail=count-1u-i,release=tail<192u ? tail:192u;
        int amplitude=(int)(5000u*(count*10u-6u*i)/(count*10u));
        amplitude=amplitude*(int)attack/32*(int)release/192;
        return ((i*notes[n].hz)%16000u)<8000u ? (int16_t)amplitude:(int16_t)-amplitude;
    }
    return 0;
}
int16_t feishu_feedback_sample(feishu_sound_t k,unsigned i)
{
    if(i>=feishu_feedback_samples(k))return 0;
    return k==FEISHU_SOUND_BOOT ? boot_sample(i):tones[k].pcm[i];
}
bool feishu_feedback_enabled(const feishu_sound_settings_t *s,feishu_sound_t k)
{return s && ((k==FEISHU_SOUND_KEY && s->key) || ((k==FEISHU_SOUND_SUCCESS || k==FEISHU_SOUND_BOOT) && s->success) || (k==FEISHU_SOUND_FAILURE && s->failure));}
bool feishu_feedback_fresh(feishu_sound_t k,uint32_t requested,uint32_t now)
{return (unsigned)k<4 && (uint32_t)(now-requested)<(k==FEISHU_SOUND_KEY ? 180u:2000u);}
