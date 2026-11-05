#include "feishu_model.h"

#include <string.h>

uint32_t feishu_unread_count(const feishu_message_t *messages, size_t count,
                             uint64_t last_seen_time_ms)
{
    uint32_t unread = 0;

    if (messages == NULL) return 0;
    for (size_t i = 0; i < count; ++i) {
        const feishu_message_t *message = &messages[i];
        if (!message->deleted && !message->mine &&
            message->create_time_ms > last_seen_time_ms) {
            ++unread;
        }
    }
    return unread;
}

uint64_t feishu_latest_visible_time(const feishu_message_t *messages,
                                    size_t count)
{
    uint64_t latest = 0;

    if (messages == NULL) return 0;
    for (size_t i = 0; i < count; ++i) {
        if (!messages[i].deleted && messages[i].create_time_ms > latest) {
            latest = messages[i].create_time_ms;
        }
    }
    return latest;
}

size_t feishu_utf8_copy(char *dst, size_t dst_size, const char *src)
{
    size_t length;

    if (dst == NULL || dst_size == 0) return 0;
    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    length = strlen(src);
    if (length >= dst_size) length = dst_size - 1;
    while (length > 0 && ((unsigned char)src[length] & 0xC0U) == 0x80U) {
        --length;
    }
    memcpy(dst, src, length);
    dst[length] = '\0';
    return length;
}

static void move_selection(size_t *selection, size_t count, int direction)
{
    if (selection == NULL || count == 0) return;
    if (direction < 0) {
        if (*selection > 0) --*selection;
    } else if (*selection + 1 < count) {
        ++*selection;
    }
}

feishu_nav_action_t feishu_nav_handle(feishu_nav_t *nav,
                                      feishu_nav_event_t event)
{
    if (nav == NULL) return FEISHU_NAV_NONE;

    switch (nav->page) {
    case FEISHU_PAGE_CHATS:
        if (event == FEISHU_NAV_UP) move_selection(&nav->chat_index, nav->chat_count, -1);
        if (event == FEISHU_NAV_DOWN) move_selection(&nav->chat_index, nav->chat_count, 1);
        if (event == FEISHU_NAV_OK && nav->chat_count > 0) {
            nav->page = FEISHU_PAGE_MESSAGES;
            nav->message_index = 0;
            nav->composer_selected = true;
            return FEISHU_NAV_LOAD_MESSAGES;
        }
        if (event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_SETTINGS;
            nav->settings_index = 0;
        }
        break;
    case FEISHU_PAGE_MESSAGES:
        if (event == FEISHU_NAV_UP) {
            if (nav->composer_selected && nav->message_count > 0) {
                nav->composer_selected = false;
                nav->message_index = nav->message_count - 1;
            } else {
                move_selection(&nav->message_index, nav->message_count, -1);
            }
        }
        if (event == FEISHU_NAV_DOWN) {
            if (!nav->composer_selected && nav->message_count > 0 &&
                nav->message_index + 1 == nav->message_count) {
                nav->composer_selected = true;
            } else if (!nav->composer_selected) {
                move_selection(&nav->message_index, nav->message_count, 1);
            }
        }
        if (event == FEISHU_NAV_OK && nav->composer_selected) {
            nav->reply_to_message = false;
            nav->page = FEISHU_PAGE_REPLY_READY;
        } else if (event == FEISHU_NAV_OK && nav->message_count > 0) {
            nav->page = FEISHU_PAGE_MESSAGE_DETAIL;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_CHATS;
        break;
    case FEISHU_PAGE_MESSAGE_DETAIL:
        if (event == FEISHU_NAV_OK) {
            nav->reply_to_message = true;
            nav->page = FEISHU_PAGE_REPLY_READY;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGES;
        break;
    case FEISHU_PAGE_REPLY_READY:
        if (event == FEISHU_NAV_OK) {
            nav->page = FEISHU_PAGE_RECORDING;
            return FEISHU_NAV_START_RECORDING;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGES;
        break;
    case FEISHU_PAGE_RECORDING:
        if (event == FEISHU_NAV_OK) {
            nav->page = FEISHU_PAGE_TRANSCRIBING;
            return FEISHU_NAV_STOP_RECORDING;
        }
        if (event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_MESSAGES;
            return FEISHU_NAV_CANCEL_REPLY;
        }
        break;
    case FEISHU_PAGE_TRANSCRIBING:
        if (event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_MESSAGES;
            return FEISHU_NAV_CANCEL_REPLY;
        }
        break;
    case FEISHU_PAGE_REVIEW:
        if (event == FEISHU_NAV_OK) return FEISHU_NAV_SEND_REPLY;
        if (event == FEISHU_NAV_UP) {
            nav->page = FEISHU_PAGE_RECORDING;
            return FEISHU_NAV_RETRY_RECORDING;
        }
        if (event == FEISHU_NAV_DOWN || event == FEISHU_NAV_BACK) {
            nav->page = FEISHU_PAGE_MESSAGES;
            return FEISHU_NAV_CANCEL_REPLY;
        }
        break;
    case FEISHU_PAGE_DRAFT_ACTIONS:
        if(event==FEISHU_NAV_UP)move_selection(&nav->settings_index,3,-1);
        if(event==FEISHU_NAV_DOWN)move_selection(&nav->settings_index,3,1);
        if(event==FEISHU_NAV_BACK)nav->page=FEISHU_PAGE_REVIEW;
        if(event==FEISHU_NAV_OK) {
            if(nav->settings_index==0)return FEISHU_NAV_SEND_REPLY;
            if(nav->settings_index==1) {nav->page=FEISHU_PAGE_RECORDING;return FEISHU_NAV_RETRY_RECORDING;}
            nav->page=FEISHU_PAGE_MESSAGES;return FEISHU_NAV_CANCEL_REPLY;
        }
        break;
    case FEISHU_PAGE_STATUS:
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGES;
        break;
    case FEISHU_PAGE_IMAGE:
        if (event == FEISHU_NAV_OK) {
            nav->settings_index=0;nav->page=FEISHU_PAGE_IMAGE_ACTIONS;
        }
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_MESSAGE_DETAIL;
        break;
    case FEISHU_PAGE_IMAGE_ACTIONS:
        if(event==FEISHU_NAV_UP)move_selection(&nav->settings_index,6,-1);
        if(event==FEISHU_NAV_DOWN)move_selection(&nav->settings_index,6,1);
        if(event==FEISHU_NAV_BACK)nav->page=FEISHU_PAGE_IMAGE;
        if(event==FEISHU_NAV_OK) {
            if(nav->settings_index==0) {nav->reply_to_message=true;nav->page=FEISHU_PAGE_REPLY_READY;return FEISHU_NAV_NONE;}
            nav->page=FEISHU_PAGE_IMAGE;
            if(nav->settings_index==1)return FEISHU_NAV_IMAGE_ZOOM_IN;
            if(nav->settings_index==2)return FEISHU_NAV_IMAGE_ZOOM_OUT;
            if(nav->settings_index==3)return FEISHU_NAV_IMAGE_AXIS;
            if(nav->settings_index==4)return FEISHU_NAV_LOAD_IMAGE;
        }
        break;
    case FEISHU_PAGE_DRAFTS:
        if(event==FEISHU_NAV_UP)move_selection(&nav->draft_index,nav->draft_count,-1);
        if(event==FEISHU_NAV_DOWN)move_selection(&nav->draft_index,nav->draft_count,1);
        if(event==FEISHU_NAV_BACK) {nav->page=FEISHU_PAGE_CHATS;nav->draft_manager=false;}
        if(event==FEISHU_NAV_OK && nav->draft_count)return FEISHU_NAV_OPEN_DRAFT;
        break;
    case FEISHU_PAGE_SETTINGS:
        if (event == FEISHU_NAV_UP) move_selection(&nav->settings_index, 5, -1);
        if (event == FEISHU_NAV_DOWN) move_selection(&nav->settings_index, 5, 1);
        if (event == FEISHU_NAV_BACK) nav->page = FEISHU_PAGE_CHATS;
        if (event == FEISHU_NAV_OK) {
            if (nav->settings_index < 2) {
                nav->confirm_action = nav->settings_index + 1;
                nav->page = FEISHU_PAGE_CONFIRM;
            } else if(nav->settings_index==2) {
                nav->page=FEISHU_PAGE_DRAFTS;nav->draft_manager=true;nav->draft_index=nav->draft_count=0;
                return FEISHU_NAV_LIST_DRAFTS;
            } else if(nav->settings_index==3) {
                nav->confirm_action=3;nav->page=FEISHU_PAGE_CONFIRM;
            } else {
                nav->page = FEISHU_PAGE_CHATS;
            }
        }
        break;
    case FEISHU_PAGE_CONFIRM:
        if (event == FEISHU_NAV_OK) {
            return nav->confirm_action == 1 ? FEISHU_NAV_RESET_WIFI :
                   nav->confirm_action == 2 ? FEISHU_NAV_UNBIND : FEISHU_NAV_REAUTHORIZE;
        }
        if (event == FEISHU_NAV_UP || event == FEISHU_NAV_DOWN ||
            event == FEISHU_NAV_BACK) {
            nav->confirm_action = 0;
            nav->page = FEISHU_PAGE_SETTINGS;
        }
        break;
    default:
        break;
    }
    return FEISHU_NAV_NONE;
}

size_t feishu_text_preview(char *dst,size_t capacity,const char *src,bool truncated,unsigned cells,unsigned lines)
{
    if(!dst || !capacity)return 0;
    dst[0]=0;
    if(!src || !cells || !lines)return 0;
    size_t used=0;unsigned col=0,line=1;bool space=false;
    const unsigned char *p=(const unsigned char *)src;
    while(*p) {
        if(*p==' ' || *p=='\n' || *p=='\r' || *p=='\t') {space=used>0;++p;continue;}
        size_t n=*p<0x80 ? 1:*p<0xe0 ? 2:*p<0xf0 ? 3:4;
        unsigned width=n==1 ? 1:2;
        if(space && col) {
            if(col+1+width<=cells && used+1<capacity) {dst[used++]=' ';++col;}
            space=false;
        }
        /* Reserve three ASCII cells on the last line when more text follows. */
        unsigned limit=cells;
        if(line==lines && (p[n] || truncated))limit=cells>3 ? cells-3:0;
        if(col+width>limit) {
            if(line>=lines) {truncated=true;break;}
            if(used+1>=capacity) {truncated=true;break;}
            dst[used++]='\n';++line;col=0;
        }
        if(used+n+4>=capacity) {truncated=true;break;}
        memcpy(dst+used,p,n);used+=n;p+=n;col+=width;
    }
    if(truncated && used && used+3<capacity) {memcpy(dst+used,"...",3);used+=3;}
    dst[used]=0;return used;
}

void feishu_avatar_text(char output[5],const char *text,const char *fallback)
{
    const unsigned char *p=(const unsigned char *)(text ? text:"");
    while(*p) {
        if((*p>='A' && *p<='Z') || (*p>='a' && *p<='z') || (*p>='0' && *p<='9')) {
            unsigned n=0;
            while(n<2 && ((*p>='A' && *p<='Z') || (*p>='a' && *p<='z') || (*p>='0' && *p<='9'))) {
                output[n++]=(char)(*p>='a' && *p<='z' ? *p-'a'+'A':*p);++p;
            }
            output[n]=0;return;
        }
        size_t n=*p<0x80 ? 1:*p<0xe0 ? 2:*p<0xf0 ? 3:4;
        uint32_t code=*p & (n==1 ? 0x7f:n==2 ? 0x1f:n==3 ? 0x0f:7);
        for(size_t i=1;i<n;++i) {
            if((p[i]&0xc0)!=0x80) {feishu_utf8_copy(output,5,fallback);return;}
            code=(code<<6)|(p[i]&63);
        }
        if(code>=0x4e00 && code<=0x9fff) {memcpy(output,p,n);output[n]=0;return;}
        p+=n;
    }
    feishu_utf8_copy(output,5,fallback);
}
