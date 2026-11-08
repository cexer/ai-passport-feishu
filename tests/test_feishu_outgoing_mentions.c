#include "feishu_outgoing_mentions.h"
#include "feishu_member_search.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool page(feishu_member_search_t *s,const char *json)
{
    feishu_member_search_page(s);
    for(size_t i=0;i<strlen(json);++i)if(!feishu_member_search_feed(s,json+i,1))return false;
    return feishu_member_search_finish(s);
}
int main(void)
{
    feishu_member_search_t *s=calloc(1,sizeof(*s));assert(s);
    feishu_mention_t found={0};
    assert(feishu_member_search_init(s,"Neo"));
    assert(page(s,"{\"code\":0,\"data\":{\"items\":[{\"member_id\":\"ou_human\",\"name\":\"Neo fan\"}],\"has_more\":true,\"page_token\":\"next\"}}"));
    assert(s->more && !strcmp(s->token,"next"));
    assert(page(s,"{\"code\":0,\"data\":{\"items\":[{\"bot_id\":\"ou_robot\",\"bot_name\":\"neo\"}],\"has_more\":false}}"));
    assert(feishu_member_search_result(s,&found) && !strcmp(found.id,"ou_robot"));
    /* Duplicate IDs across pages don't become ambiguity; a second person does. */
    assert(page(s,"{\"code\":0,\"data\":{\"items\":[{\"member_id\":\"ou_robot\",\"name\":\"Neo\"}]}}"));
    assert(feishu_member_search_result(s,&found));
    assert(page(s,"{\"code\":0,\"data\":{\"items\":[{\"member_id\":\"ou_other\",\"name\":\"Neo\"}]}}"));
    assert(!feishu_member_search_result(s,&found));
    const char *bad[]={"{\"code\":2147483648,\"data\":{\"items\":[]}}",
        "{\"code\":0,\"data\":{\"items\":[],\"has_more\":true}}",
        "{\"code\":0,\"data\":{\"items\":[],\"has_more\":\"false\"}}",
        "{\"code\":0,\"data\":{\"items\":[{\"member_id\":\"ou_a\",\"member_id\":\"ou_b\",\"name\":\"Neo\"}]}}",
        "{\"code\":0,\"data\":{\"items\":[]},\"data\":{\"items\":[]}}",
        "{\"code\":999,\"data\":{\"items\":[]}}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i){assert(feishu_member_search_init(s,"Neo"));assert(!page(s,bad[i]));}
    char out[512],display[512];feishu_mention_t m={"ou_real","Neo & <friend>"};
    assert(feishu_mentions_compose("Please reply",&m,1,out,sizeof(out)));
    assert(strstr(out,"<at user_id=\"ou_real\">Neo &amp; &lt;friend&gt;</at>"));
    feishu_mention_t parsed[FEISHU_MENTION_MAX];size_t count=0;const char *body=NULL;
    assert(feishu_mentions_split(out,parsed,&count,&body) && count==1 && !strcmp(body,"Please reply"));
    assert(!strcmp(parsed[0].id,m.id) && !strcmp(parsed[0].name,m.name));
    feishu_mentions_display(out,display,sizeof(display));assert(!strcmp(display,"@Neo & <friend> Please reply"));
    assert(!feishu_mentions_compose("@Neo reply",&m,1,out,sizeof(out)));
    assert(!feishu_mentions_compose("<at user_id=\"ou_fake\">Fake</at>",&m,1,out,sizeof(out)));
    assert(feishu_mentions_compose("mail user@example.com",NULL,0,out,sizeof(out)));
    assert(!feishu_mentions_compose("reply",&m,1,out,8));
    /* A Unicode display cut must retain complete scalar values. */
    feishu_mentions_display("\xe4\xb8\xad\xe6\x96\x87",display,5);assert(!strcmp(display,"\xe4\xb8\xad"));
    free(s);puts("mentions stream/pages/user+bot/ambiguity/overflow/markup/UTF-8: PASS");
}
