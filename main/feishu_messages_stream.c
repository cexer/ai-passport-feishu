#include "feishu_messages_stream.h"
#include <limits.h>
#include <string.h>

enum {R_NONE,R_ROOT,R_DATA,R_ITEMS,R_ITEM,R_SENDER,R_BODY,R_MENTIONS,R_MENTION,R_INNER,R_IGNORE,R_POST_LINES,R_POST_ROW};
enum {K_NONE,K_CODE,K_DATA,K_ITEMS,K_ID,K_TYPE,K_SENDER,K_BODY,K_CONTENT,K_TIME,K_DELETED,
      K_SENDER_ID,K_SENDER_NAME,K_MENTIONS,K_KEY,K_NAME,K_TEXT,K_TITLE,K_IMAGE,K_FILE,K_TAG,K_HREF,K_LANG_CN,K_LANG,K_MORE,K_PAGE_TOKEN};
enum {S_NONE,S_ID,S_TYPE,S_SENDER_ID,S_NAME,S_TIME,S_BODY,S_MENTION_KEY,S_MENTION_NAME,S_TEXT,S_TITLE,S_IMAGE,S_TAG,S_HREF,S_POST_NAME,S_PAGE_TOKEN};

static bool equals(const uint8_t *data,size_t size,const char *word)
{ return data && size==strlen(word) && !memcmp(data,word,size); }
static unsigned key_type(const uint8_t *data,size_t size)
{
    static const struct {const char *name;unsigned key;} names[]={
        {"code",K_CODE},{"data",K_DATA},{"items",K_ITEMS},{"message_id",K_ID},{"msg_type",K_TYPE},
        {"sender",K_SENDER},{"body",K_BODY},{"content",K_CONTENT},{"create_time",K_TIME},
        {"deleted",K_DELETED},{"id",K_SENDER_ID},{"sender_name",K_SENDER_NAME},{"mentions",K_MENTIONS},
        {"key",K_KEY},{"name",K_NAME},{"text",K_TEXT},{"title",K_TITLE},{"image_key",K_IMAGE},
        {"has_more",K_MORE},{"page_token",K_PAGE_TOKEN},{"file_key",K_FILE},{"tag",K_TAG},{"href",K_HREF},{"user_name",K_NAME},{"zh_cn",K_LANG_CN}};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i)if(equals(data,size,names[i].name))return names[i].key;
    if(data && size==5 && data[2]=='_' && data[0]>='a' && data[0]<='z' && data[3]>='a' && data[3]<='z')return K_LANG;
    return K_NONE;
}
static bool number(const uint8_t *data,size_t size,uint64_t *value)
{
    if(!data || !size || size>20)return false;
    uint64_t n=0;
    for(size_t i=0;i<size;++i) {
        if(data[i]<'0' || data[i]>'9' || n>(UINT64_MAX-(data[i]-'0'))/10)return false;
        n=n*10+data[i]-'0';
    }
    *value=n;return true;
}
static void small(feishu_messages_stream_t *p,const uint8_t *data,size_t size)
{
    if(p->small_overflow)return;
    if(size>=sizeof(p->small)-p->small_size || memchr(data,0,size)) {p->small_overflow=true;return;}
    memcpy(p->small+p->small_size,data,size);p->small_size+=size;p->small[p->small_size]=0;
}
static void abort_slot(feishu_messages_stream_t *p)
{
    if(p->storage.abort && p->count<p->capacity)p->storage.abort(p->storage_context,p->count);
}
static void reset_text(feishu_messages_stream_t *p)
{
    if(p->storage.reset)p->storage.reset(p->storage_context,p->count);else abort_slot(p);
    p->text_length=0;p->text_used=p->preview_size=0;
    p->current.text[0]=0;p->have_text=p->text_started=p->text_failed=p->preview_full=false;
    p->current.resource_key[0]=0;
}
static void start_text(feishu_messages_stream_t *p)
{
    p->have_text=true;
    if(p->text_started)return;
    p->text_started=true;
    p->text_failed=!p->storage.begin || !p->storage.begin(p->storage_context,p->count);
}
static void flush_text(feishu_messages_stream_t *p)
{
    if(p->text_used && !p->text_failed && (!p->storage.write ||
       !p->storage.write(p->storage_context,p->count,p->text_block,p->text_used)))p->text_failed=true;
    p->text_used=0;
}
static void text(feishu_messages_stream_t *p,const uint8_t *data,size_t size)
{
    start_text(p);
    static const uint8_t replacement[]={0xef,0xbf,0xbd};
    if(size==1 && data[0]==0) {data=replacement;size=sizeof(replacement);}
    if(!p->preview_full) {
        if(size>=sizeof(p->current.text)-p->preview_size)p->preview_full=true;
        else {memcpy(p->current.text+p->preview_size,data,size);p->preview_size+=size;p->current.text[p->preview_size]=0;}
    }
    p->text_length+=size;
    if(p->text_failed)return;
    while(size) {
        size_t n=sizeof(p->text_block)-p->text_used;if(n>size)n=size;
        memcpy(p->text_block+p->text_used,data,n);p->text_used+=n;data+=n;size-=n;
        if(p->text_used==sizeof(p->text_block))flush_text(p);
    }
}
static void literal(feishu_messages_stream_t *p,const char *value)
{
    while(*value) {
        uint8_t lead=(uint8_t)*value;
        size_t n=lead<0x80 ? 1 : lead<0xe0 ? 2 : lead<0xf0 ? 3 : 4;
        text(p,(const uint8_t *)value,n);value+=n;
    }
}

static bool inner_event(void *context,feishu_json_event_t event,unsigned depth,const uint8_t *data,size_t size)
{
    feishu_messages_stream_t *p=context;
    if(depth>=FEISHU_JSON_DEPTH)return false;
    unsigned parent=depth ? p->inner_role[depth-1] : R_NONE;
    unsigned key=depth ? p->inner_key[depth-1] : K_NONE;
    if(event==FEISHU_JSON_KEY) {p->inner_key[depth-1]=(uint8_t)key_type(data,size);return true;}
    if(event==FEISHU_JSON_OBJECT_BEGIN || event==FEISHU_JSON_ARRAY_BEGIN) {
        /* Unknown fields can contain a second representation of the whole
         * post (content_v2), not additional text. Traverse only supported
         * content/row/element paths and selected locale objects. */
        p->inner_role[depth]=R_IGNORE;p->inner_key[depth]=K_NONE;
        if(depth==1 && event==FEISHU_JSON_OBJECT_BEGIN)p->inner_role[depth]=R_INNER;
        else if(parent==R_POST_ROW && event==FEISHU_JSON_OBJECT_BEGIN)p->inner_role[depth]=R_INNER;
        else if(parent==R_INNER && key==K_CONTENT && event==FEISHU_JSON_ARRAY_BEGIN)
            p->inner_role[depth]=R_POST_LINES;
        else if(parent==R_POST_LINES && event==FEISHU_JSON_ARRAY_BEGIN)
            p->inner_role[depth]=R_POST_ROW;
        if(parent==R_INNER && depth==2 && event==FEISHU_JSON_OBJECT_BEGIN &&
           (key==K_LANG_CN || key==K_LANG)) {
            unsigned choice=key==K_LANG_CN ? 2 : 1;
            if(!p->locale || choice>p->locale) {
                if(p->have_text)reset_text(p);
                p->locale=choice;p->inner_role[depth]=R_INNER;
            }
        }
        return true;
    }
    if(event==FEISHU_JSON_ARRAY_END && p->inner_role[depth]==R_POST_ROW && p->have_text)literal(p,"\n");
    if(event==FEISHU_JSON_STRING_BEGIN) {
        p->inner_route=S_NONE;p->small_size=0;p->small_overflow=false;p->small[0]=0;
        if(parent==R_IGNORE)return true;
        if(key==K_TEXT) {p->inner_route=S_TEXT;start_text(p);}
        else if(key==K_TITLE) {p->inner_route=S_TITLE;start_text(p);}
        else if(key==K_IMAGE || key==K_FILE)p->inner_route=S_IMAGE;
        else if(key==K_TAG)p->inner_route=S_TAG;
        else if(key==K_NAME)p->inner_route=S_POST_NAME;
        else if(key==K_HREF) {p->inner_route=S_HREF;literal(p," [");}
    } else if(event==FEISHU_JSON_STRING_PART) {
        if(p->inner_route==S_TEXT || p->inner_route==S_TITLE || p->inner_route==S_HREF)text(p,data,size);
        else if(p->inner_route!=S_NONE)small(p,data,size);
    } else if(event==FEISHU_JSON_STRING_END) {
        if(p->inner_route==S_TITLE)literal(p,"\n");
        else if(p->inner_route==S_HREF)literal(p,"] ");
        else if(p->inner_route==S_IMAGE && !p->small_overflow)
            feishu_utf8_copy(p->current.resource_key,sizeof(p->current.resource_key),p->small);
        else if(p->inner_route==S_TAG && !strcmp(p->small,"img"))literal(p,"[图片]");
        else if(p->inner_route==S_TAG && !strcmp(p->small,"emotion"))literal(p,"[表情]");
        else if(p->inner_route==S_POST_NAME && !p->small_overflow) {literal(p,"@");literal(p,p->small);}
        p->inner_route=S_NONE;
    }
    return true;
}

static feishu_message_type_t type(const char *name)
{
    if(!strcmp(name,"text") || !strcmp(name,"post"))return FEISHU_MESSAGE_TEXT;
    if(!strcmp(name,"image"))return FEISHU_MESSAGE_IMAGE;
    if(!strcmp(name,"audio"))return FEISHU_MESSAGE_AUDIO;
    if(!strcmp(name,"file") || !strcmp(name,"media"))return FEISHU_MESSAGE_FILE;
    return FEISHU_MESSAGE_OTHER;
}
static void end_item(feishu_messages_stream_t *p)
{
    if(!p->item_active)return;
    p->item_active=false;
    if(p->item_invalid || !p->current.message_id[0] || !p->sender_seen || !p->type_seen) {abort_slot(p);return;}
    p->current.type=type(p->type);
    p->current.mine=p->current.sender_id[0] && !strcmp(p->current.sender_id,p->self);
    if(!p->current.sender_name[0])feishu_utf8_copy(p->current.sender_name,sizeof(p->current.sender_name),"未知用户");
    const char *placeholder=NULL;
    if(p->current.deleted)placeholder="[消息已撤回]";
    else if(p->current.type==FEISHU_MESSAGE_AUDIO)placeholder="[语音消息]";
    else if(p->current.type==FEISHU_MESSAGE_IMAGE)placeholder="[图片]";
    else if(p->current.type==FEISHU_MESSAGE_FILE)placeholder="[文件]";
    else if(p->current.type==FEISHU_MESSAGE_OTHER)placeholder="[暂不支持的消息]";
    else if(p->body_invalid)placeholder="[消息内容无法解析]";
    else if(!p->have_text)placeholder="[富文本消息]";
    if(placeholder) {
        feishu_utf8_copy(p->current.text,sizeof(p->current.text),placeholder);abort_slot(p);
        p->current.text_length=0;p->current.text_unavailable=false;
    } else {
        flush_text(p);
        p->current.text_length=p->text_length;
        if((p->mention_count || p->mapping_failed) && !p->text_failed && p->storage.prepare &&
           !p->storage.prepare(p->storage_context,p->count,!p->mapping_failed,&p->current.text_length,&p->current.text_mapped,
               p->current.text,sizeof(p->current.text),p->text_block,sizeof(p->text_block)))p->text_failed=true;
        p->current.text_unavailable=p->text_failed;
        p->current.text_truncated=p->current.text_length>strlen(p->current.text);
        p->text_ready[p->count]=!p->text_failed;
        if(p->text_failed)abort_slot(p);
    }
    p->messages[p->count++]=p->current;
}

static bool outer_event(void *context,feishu_json_event_t event,unsigned depth,const uint8_t *data,size_t size)
{
    feishu_messages_stream_t *p=context;
    if(depth>=FEISHU_JSON_DEPTH)return false;
    unsigned role=depth ? p->outer_role[depth-1] : R_NONE;
    unsigned key=depth ? p->outer_key[depth-1] : K_NONE;
    if(event==FEISHU_JSON_KEY) {p->outer_key[depth-1]=(uint8_t)key_type(data,size);return true;}
    if(event==FEISHU_JSON_OBJECT_BEGIN || event==FEISHU_JSON_ARRAY_BEGIN) {
        if(role==R_DATA && (key==K_MORE || key==K_PAGE_TOKEN))return false;
        unsigned next=R_NONE;
        if(depth==1 && event==FEISHU_JSON_OBJECT_BEGIN)next=R_ROOT;
        else if(role==R_ROOT && key==K_DATA && event==FEISHU_JSON_OBJECT_BEGIN) {
            if(p->data_seen)return false;
            p->data_seen=true;next=R_DATA;
        } else if(role==R_DATA && key==K_ITEMS && event==FEISHU_JSON_ARRAY_BEGIN) {
            if(p->items_seen)return false;
            p->items_seen=true;next=R_ITEMS;
        } else if(role==R_ITEMS && event==FEISHU_JSON_OBJECT_BEGIN) {
            if(++p->item_count>p->capacity)return false;
            next=R_ITEM;p->item_active=true;
            memset(&p->current,0,sizeof(p->current));p->type[0]=0;
            p->item_invalid=p->sender_seen=p->type_seen=p->body_seen=p->body_invalid=false;
            p->mention_count=0;p->mapping_failed=p->mentions_seen=false;p->locale=0;reset_text(p);
        } else if(role==R_ITEM && key==K_SENDER && event==FEISHU_JSON_OBJECT_BEGIN) {next=R_SENDER;p->sender_seen=true;}
        else if(role==R_ITEM && key==K_BODY && event==FEISHU_JSON_OBJECT_BEGIN)next=R_BODY;
        else if(role==R_ITEM && key==K_MENTIONS && event==FEISHU_JSON_ARRAY_BEGIN) {
            if(p->mentions_seen)p->mapping_failed=true;
            p->mentions_seen=true;next=R_MENTIONS;
        }
        else if(role==R_MENTIONS && event==FEISHU_JSON_OBJECT_BEGIN) {next=R_MENTION;p->mention_invalid=false;memset(&p->mention,0,sizeof(p->mention));}
        p->outer_role[depth]=(uint8_t)next;p->outer_key[depth]=K_NONE;
        return true;
    }
    if(event==FEISHU_JSON_OBJECT_END) {
        if(p->outer_role[depth]==R_ITEM)end_item(p);
        else if(p->outer_role[depth]==R_MENTION) {
            if(p->mention_count<SIZE_MAX)++p->mention_count;else p->mapping_failed=true;
            if(p->mention_invalid || !p->mention.key[0] || !p->mention.name[0])p->mapping_failed=true;
            if(!p->mapping_failed && (!p->storage.mention ||
               !p->storage.mention(p->storage_context,p->count,&p->mention)))p->mapping_failed=true;
        }
        return true;
    }
    if(event==FEISHU_JSON_ATOM) {
        uint64_t n;
        if(role==R_ROOT && key==K_CODE) {
            if(p->code_seen || !number(data,size,&n) || n>INT_MAX)return false;
            p->code_seen=true;p->api_code=(int)n;
        } else if(role==R_DATA && key==K_MORE) {
            if(p->more_seen || (!equals(data,size,"true") && !equals(data,size,"false")))return false;
            p->more_seen=true;p->more=equals(data,size,"true");
        } else if(role==R_DATA && key==K_PAGE_TOKEN)return false;
        else if(role==R_ITEM && key==K_DELETED)p->current.deleted=equals(data,size,"true");
        else if(role==R_ITEM && key==K_TIME && number(data,size,&n))p->current.create_time_ms=n;
        return true;
    }
    if(event==FEISHU_JSON_STRING_BEGIN) {
        p->outer_route=S_NONE;p->small_size=0;p->small_overflow=false;p->small[0]=0;
        if(role==R_DATA && key==K_MORE)return false;
        if(role==R_DATA && key==K_PAGE_TOKEN) {
            if(p->next_seen)return false;
            p->next_seen=true;p->next_page_size=0;p->next_page[0]=0;p->outer_route=S_PAGE_TOKEN;
        } else if(role==R_ITEM) {
            if(key==K_ID)p->outer_route=S_ID;else if(key==K_TYPE)p->outer_route=S_TYPE;else if(key==K_TIME)p->outer_route=S_TIME;
        } else if(role==R_SENDER) {
            if(key==K_SENDER_ID)p->outer_route=S_SENDER_ID;else if(key==K_SENDER_NAME)p->outer_route=S_NAME;
        } else if(role==R_MENTION) {
            if(key==K_KEY)p->outer_route=S_MENTION_KEY;else if(key==K_NAME)p->outer_route=S_MENTION_NAME;
        } else if(role==R_BODY && key==K_CONTENT) {
            if(p->body_seen)return false;
            p->body_seen=true;p->outer_route=S_BODY;
            feishu_json_reset(&p->inner);memset(p->inner_role,0,sizeof(p->inner_role));memset(p->inner_key,0,sizeof(p->inner_key));
        }
    } else if(event==FEISHU_JSON_STRING_PART) {
        if(p->outer_route==S_PAGE_TOKEN) {
            if(size>FEISHU_PAGE_TOKEN_MAX-p->next_page_size)return false;
            for(size_t i=0;i<size;++i)if(data[i]<33 || data[i]>126)return false;
            memcpy(p->next_page+p->next_page_size,data,size);p->next_page_size+=size;p->next_page[p->next_page_size]=0;
        } else if(p->outer_route==S_BODY) {
            if(!p->body_invalid && !feishu_json_feed(&p->inner,data,size))p->body_invalid=true;
        } else if(p->outer_route!=S_NONE)small(p,data,size);
    } else if(event==FEISHU_JSON_STRING_END) {
        switch(p->outer_route) {
        case S_BODY:if(!p->body_invalid && !feishu_json_finish(&p->inner))p->body_invalid=true;break;
        case S_ID:
            if(p->small_overflow || p->small_size>=sizeof(p->current.message_id))p->item_invalid=true;
            else feishu_utf8_copy(p->current.message_id,sizeof(p->current.message_id),p->small);
            break;
        case S_TYPE:p->type_seen=true;feishu_utf8_copy(p->type,sizeof(p->type),p->small);break;
        case S_SENDER_ID:feishu_utf8_copy(p->current.sender_id,sizeof(p->current.sender_id),p->small);break;
        case S_NAME:feishu_utf8_copy(p->current.sender_name,sizeof(p->current.sender_name),p->small);break;
        case S_TIME:{uint64_t n;if(number((const uint8_t *)p->small,p->small_size,&n))p->current.create_time_ms=n;break;}
        case S_MENTION_KEY:
            if(p->small_overflow || p->small_size>=sizeof(p->mention.key))p->mention_invalid=true;
            else feishu_utf8_copy(p->mention.key,sizeof(p->mention.key),p->small);
            break;
        case S_MENTION_NAME:
            if(p->small_overflow || p->small_size>=sizeof(p->mention.name))p->mention_invalid=true;
            else feishu_utf8_copy(p->mention.name,sizeof(p->mention.name),p->small);
            break;
        default:break;
        }
        p->outer_route=S_NONE;
    }
    return true;
}

void feishu_messages_stream_init(feishu_messages_stream_t *p,feishu_message_t *messages,
                                  size_t capacity,const char *self,const feishu_stream_storage_t *storage,void *context)
{
    memset(p,0,sizeof(*p));p->messages=messages;p->capacity=capacity<=FEISHU_MAX_MESSAGES ? capacity : 0;p->storage_context=context;
    if(!messages || !capacity || capacity>FEISHU_MAX_MESSAGES || !self)p->failed=true;
    else feishu_utf8_copy(p->self,sizeof(p->self),self);
    if(storage)p->storage=*storage;
    feishu_json_init(&p->outer,outer_event,p);feishu_json_init(&p->inner,inner_event,p);
}
bool feishu_messages_stream_feed(feishu_messages_stream_t *p,const void *data,size_t size)
{
    if(p->failed || p->finished)return false;
    if(!feishu_json_feed(&p->outer,data,size))p->failed=true;
    return !p->failed;
}
void feishu_messages_stream_abort(feishu_messages_stream_t *p)
{
    if(p->storage.abort)for(size_t i=0;i<p->capacity;++i)p->storage.abort(p->storage_context,i);
}
bool feishu_messages_stream_finish(feishu_messages_stream_t *p,size_t *count)
{
    if(!count)return false;
    *count=0;
    if(p->failed || p->finished || !feishu_json_finish(&p->outer) || !p->code_seen || !p->data_seen || !p->items_seen || p->api_code ||
       (p->pagination_required && (!p->more_seen || (p->more && (!p->next_seen || !p->next_page_size))))) {
        feishu_messages_stream_abort(p);return false;
    }
    for(size_t i=0;i<p->count;++i) {
        if(p->text_ready[i]) {
            if(!p->storage.commit || !p->storage.commit(p->storage_context,i,p->messages[i].message_id,
                &p->messages[i].text_object_id,&p->messages[i].text_length)) {
                p->messages[i].text_object_id=0;p->messages[i].text_unavailable=true;
                if(p->storage.abort)p->storage.abort(p->storage_context,i);
            }
        }
    }
    for(size_t i=0;i<p->count/2;++i) {
        feishu_message_t swap=p->messages[i];p->messages[i]=p->messages[p->count-1-i];p->messages[p->count-1-i]=swap;
    }
    p->finished=true;*count=p->count;return true;
}
