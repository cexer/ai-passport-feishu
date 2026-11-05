#include "feishu_messages_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint64_t lengths[FEISHU_MAX_MESSAGES];
    unsigned commits,aborts,writes;
    bool active[FEISHU_MAX_MESSAGES],fail_write;
    char captured[128];size_t captured_size;
} storage_t;
static bool begin(void *context,size_t slot)
{
    storage_t *s=context;assert(slot<FEISHU_MAX_MESSAGES);
    s->active[slot]=true;s->lengths[slot]=0;return true;
}
static bool write_block(void *context,size_t slot,const uint8_t *data,size_t size)
{
    storage_t *s=context;assert(s->active[slot] && size && size<=1024);
    if(s->fail_write)return false;
    ++s->writes;s->lengths[slot]+=size;
    if(size<=sizeof(s->captured)-s->captured_size) {memcpy(s->captured+s->captured_size,data,size);s->captured_size+=size;}
    return true;
}
static bool commit(void *context,size_t slot,const char *message,int64_t *id,uint64_t *length)
{
    storage_t *s=context;assert(s->active[slot] && *message);
    s->active[slot]=false;++s->commits;*id=(int64_t)slot+100;
    assert(*length==s->lengths[slot]);return true;
}
static void abort_slot(void *context,size_t slot)
{
    storage_t *s=context;
    if(s->active[slot]) {++s->aborts;s->active[slot]=false;}
}
static const feishu_stream_storage_t operations={.begin=begin,.write=write_block,.commit=commit,.abort=abort_slot};

int main(void)
{
    const char doc[]="{\"data\":{\"items\":[{\"body\":{\"content\":\"{\\\"text\\\":\\\"中文\\\\n末尾\\\"}\"},"
        "\"message_id\":\"new\",\"msg_type\":\"text\",\"create_time\":\"123\",\"sender\":{\"id\":\"ou_self\",\"sender_name\":\"测试\"}},"
        "{\"message_id\":\"old\",\"msg_type\":\"audio\",\"sender\":{},\"body\":{\"content\":\"{}\"}}]},\"code\":0}";
    feishu_messages_stream_t parser;
    feishu_message_t messages[FEISHU_MAX_MESSAGES];
    storage_t storage;
    for(size_t chunk=1;chunk<40;++chunk) {
        memset(&storage,0,sizeof(storage));memset(messages,0,sizeof(messages));
        feishu_messages_stream_init(&parser,messages,FEISHU_MAX_MESSAGES,"ou_self",&operations,&storage);
        for(size_t pos=0;pos<sizeof(doc)-1;) {
            size_t n=sizeof(doc)-1-pos;if(n>chunk)n=chunk;
            assert(feishu_messages_stream_feed(&parser,doc+pos,n));pos+=n;
        }
        size_t count=0;
        assert(feishu_messages_stream_finish(&parser,&count) && count==2);
        assert(!strcmp(messages[0].message_id,"old") && messages[0].type==FEISHU_MESSAGE_AUDIO);
        assert(!strcmp(messages[1].text,"中文\n末尾") && messages[1].mine);
        assert(messages[1].text_length==strlen("中文\n末尾") && messages[1].text_object_id==100);
        assert(storage.commits==1 && !storage.active[0]);
    }
    memset(&storage,0,sizeof(storage));
    feishu_messages_stream_init(&parser,messages,1,"ou_self",&operations,&storage);
    assert(!feishu_messages_stream_feed(&parser,doc,strlen(doc)));
    size_t overflow_count=0;assert(!feishu_messages_stream_finish(&parser,&overflow_count));
    assert(!storage.commits && !overflow_count && storage.aborts==1);
    /* An error code at the end must not publish any of the staged bodies. */
    char bad[sizeof(doc)];memcpy(bad,doc,sizeof(doc));bad[sizeof(doc)-3]='9';
    memset(&storage,0,sizeof(storage));
    feishu_messages_stream_init(&parser,messages,FEISHU_MAX_MESSAGES,"ou_self",&operations,&storage);
    assert(feishu_messages_stream_feed(&parser,bad,strlen(bad)));
    size_t count=0;assert(!feishu_messages_stream_finish(&parser,&count));
    assert(storage.commits==0 && storage.aborts==1 && !count);
    /* Storage failure degrades this body to a bounded preview, not a parser crash. */
    memset(&storage,0,sizeof(storage));storage.fail_write=true;
    feishu_messages_stream_init(&parser,messages,FEISHU_MAX_MESSAGES,"ou_self",&operations,&storage);
    assert(feishu_messages_stream_feed(&parser,doc,strlen(doc)));
    assert(feishu_messages_stream_finish(&parser,&count));
    assert(messages[1].text_unavailable && !messages[1].text_object_id && storage.aborts==1);
    /* Pagination metadata may follow the staged body. Invalid metadata must
     * abort it before any body publication. */
    const char *fields[]={"\"page_token\":\"cursor+8/=\",\"has_more\":true",
        "\"has_more\":true", "\"has_more\":\"false\"",
        "\"has_more\":false,\"has_more\":false", "\"page_token\":{},\"has_more\":false",
        "\"page_token\":null,\"has_more\":false", "\"page_token\":\"x\\u0000z\",\"has_more\":false"};
    char envelope[1024];
    for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
        snprintf(envelope,sizeof(envelope),"{\"code\":0,\"data\":{\"items\":[{\"message_id\":\"page\",\"msg_type\":\"text\",\"sender\":{},\"body\":{\"content\":\"{\\\"text\\\":\\\"body\\\"}\"}}],%s}}",fields[i]);
        memset(&storage,0,sizeof(storage));
        feishu_messages_stream_init(&parser,messages,FEISHU_MAX_MESSAGES,"ou_self",&operations,&storage);
        parser.pagination_required=true;
        bool fed=true;
        for(size_t at=0;at<strlen(envelope) && fed;++at)fed=feishu_messages_stream_feed(&parser,envelope+at,1);
        bool finished=feishu_messages_stream_finish(&parser,&count);
        if(!i)assert(fed && finished && count==1 && storage.commits==1 && parser.more && !strcmp(parser.next_page,"cursor+8/="));
        else assert(!finished && !count && !storage.commits && storage.aborts==1);
    }
    /* No large test string is allocated; chunks cross both JSON string layers. */
    const char prefix[]="{\"code\":0,\"data\":{\"items\":[{\"msg_type\":\"text\",\"message_id\":\"long\",\"sender\":{},\"body\":{\"content\":\"{\\\"text\\\":\\\"";
    const char suffix[]="\\\"}\"}}]}}";
    memset(&storage,0,sizeof(storage));
    feishu_messages_stream_init(&parser,messages,FEISHU_MAX_MESSAGES,"ou_self",&operations,&storage);
    assert(feishu_messages_stream_feed(&parser,prefix,strlen(prefix)));
    for(unsigned i=0;i<40000;++i)assert(feishu_messages_stream_feed(&parser,"长",strlen("长")));
    assert(feishu_messages_stream_feed(&parser,suffix,strlen(suffix)));
    assert(feishu_messages_stream_finish(&parser,&count) && count==1);
    assert(messages[0].text_length==120000 && messages[0].text_truncated);
    assert(strlen(messages[0].text)==255 && storage.lengths[0]==120000 && storage.writes==118);
    /* Actual Feishu post responses contain parallel content/content_v2.
     * Only canonical content contributes; unknown nested text cannot leak in. */
    const char *posts[]={"{\"code\":0,\"data\":{\"items\":[{\"msg_type\":\"post\",\"message_id\":\"post-canonical\",\"sender\":{},\"body\":{\"content\":\"{\\\"title\\\":\\\"\\\",\\\"content\\\":[[{\\\"tag\\\":\\\"text\\\",\\\"text\\\":\\\"\u4e2d\u6587\\\\\\\"\\\\\\\\\u3002\\\"}]],\\\"content_v2\\\":[[{\\\"tag\\\":\\\"text\\\",\\\"text\\\":\\\"\u91cd\u590d\\\"}]],\\\"metadata\\\":{\\\"text\\\":\\\"\u4e0d\u8981\\\",\\\"content\\\":[[{\\\"text\\\":\\\"\u4e0d\u80fd\u62fc\u63a5\\\"}]]}}\"}}]}}","{\"code\":0,\"data\":{\"items\":[{\"msg_type\":\"post\",\"message_id\":\"post-canonical\",\"sender\":{},\"body\":{\"content\":\"{\\\"title\\\":\\\"\\\",\\\"metadata\\\":{\\\"text\\\":\\\"\u4e0d\u8981\\\",\\\"content\\\":[[{\\\"text\\\":\\\"\u4e0d\u80fd\u62fc\u63a5\\\"}]]},\\\"content_v2\\\":[[{\\\"tag\\\":\\\"text\\\",\\\"text\\\":\\\"\u91cd\u590d\\\"}]],\\\"content\\\":[[{\\\"tag\\\":\\\"text\\\",\\\"text\\\":\\\"\u4e2d\u6587\\\\\\\"\\\\\\\\\u3002\\\"}]]}\"}}]}}"};
    for(size_t variant=0;variant<sizeof(posts)/sizeof(posts[0]);++variant) {
        for(size_t chunk=1;chunk<32;++chunk) {
            memset(&storage,0,sizeof(storage));memset(messages,0,sizeof(messages));
            feishu_messages_stream_init(&parser,messages,1,"ou_self",&operations,&storage);
            for(size_t at=0;at<strlen(posts[variant]);) {
                size_t n=strlen(posts[variant])-at;if(n>chunk)n=chunk;
                assert(feishu_messages_stream_feed(&parser,posts[variant]+at,n));at+=n;
            }
            size_t post_count=0;
            assert(feishu_messages_stream_finish(&parser,&post_count) && post_count==1);
            const char *expected="\n中文\"\\。\n";
            assert(!strcmp(messages[0].text,expected));
            assert(storage.captured_size==strlen(expected) && !memcmp(storage.captured,expected,strlen(expected)));
        }
    }
    printf("message stream: PASS; fixed context %zu bytes\n",sizeof(parser));
    return 0;
}
