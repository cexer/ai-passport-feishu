#include "feishu_model.h"
#include "feishu_pairing_policy.h"
#include "feishu_wifi_keyboard.h"

#include <assert.h>
#include <string.h>

static void test_unread(void)
{
    feishu_message_t messages[] = {
        { .create_time_ms = 300, .mine = false },
        { .create_time_ms = 250, .mine = true },
        { .create_time_ms = 200, .mine = false, .deleted = true },
        { .create_time_ms = 100, .mine = false },
    };

    assert(feishu_unread_count(messages, 4, 150) == 1);
    assert(feishu_unread_count(messages, 4, 0) == 2);
    assert(feishu_latest_visible_time(messages, 4) == 300);
}

static void test_utf8_copy(void)
{
    char output[8];

    assert(feishu_utf8_copy(output, sizeof(output), "你好世界") == 6);
    assert(strcmp(output, "你好") == 0);

    // Long server/ASR text is bounded in bytes and must end on a UTF-8 edge.
    char long_text[12001];
    for (size_t at = 0; at < 12000; at += 3) {
        memcpy(long_text + at, "长", 3);
    }
    long_text[12000] = '\0';
    char received[FEISHU_TEXT_MAX];
    char reply[FEISHU_REPLY_MAX];
    assert(feishu_utf8_copy(received, sizeof(received), long_text) == 255);
    assert(strlen(received) == 255);
    assert(feishu_utf8_copy(reply, sizeof(reply), long_text) == 510);
    assert(strlen(reply) == 510);
    for (size_t at = 0; at < 510; at += 3) {
        assert(memcmp(reply + at, "长", 3) == 0);
    }
    char emoji[8];
    assert(feishu_utf8_copy(emoji, sizeof(emoji), "😀😀") == 4);
    assert(strcmp(emoji, "😀") == 0);
    char exact[7];
    assert(feishu_utf8_copy(exact, sizeof(exact), "你好") == 6);
    assert(strcmp(exact, "你好") == 0);
}

static void test_navigation(void)
{
    feishu_nav_t nav = {
        .page = FEISHU_PAGE_CHATS,
        .chat_count = 2,
        .message_count = 3,
    };

    assert(feishu_nav_handle(&nav, FEISHU_NAV_DOWN) == FEISHU_NAV_NONE);
    assert(nav.chat_index == 1);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_DOWN) == FEISHU_NAV_NONE);
    assert(nav.chat_index == 1);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_LOAD_MESSAGES);
    assert(nav.page == FEISHU_PAGE_MESSAGES);
    assert(nav.composer_selected);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_START_RECORDING);
    assert(!nav.reply_to_message);
    assert(nav.page == FEISHU_PAGE_RECORDING);

    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_STOP_RECORDING);
    nav.page = FEISHU_PAGE_MESSAGES;
    nav.composer_selected = true;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_UP) == FEISHU_NAV_NONE);
    assert(!nav.composer_selected && nav.message_index == 2);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_MESSAGE_DETAIL);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_START_RECORDING);
    assert(nav.page == FEISHU_PAGE_RECORDING && nav.reply_to_message);
    nav.page = FEISHU_PAGE_IMAGE;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page==FEISHU_PAGE_IMAGE_ACTIONS);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK)==FEISHU_NAV_START_RECORDING);
    assert(nav.page == FEISHU_PAGE_RECORDING);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_BACK) == FEISHU_NAV_CANCEL_REPLY);
    assert(nav.page == FEISHU_PAGE_MESSAGES);
    nav.page = FEISHU_PAGE_REVIEW;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_SEND_REPLY);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_UP) == FEISHU_NAV_RETRY_RECORDING);
    assert(nav.page == FEISHU_PAGE_RECORDING);

    nav.page = FEISHU_PAGE_CHATS;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_BACK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_SETTINGS);
    nav.settings_index=FEISHU_SETTING_NETWORK;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_LIST_NETWORK);
    assert(nav.page == FEISHU_PAGE_NETWORK);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_BACK) == FEISHU_NAV_NONE);
    assert(nav.page==FEISHU_PAGE_SETTINGS && nav.settings_index==FEISHU_SETTING_NETWORK);
    nav.page = FEISHU_PAGE_SETTINGS;
    nav.settings_index = FEISHU_SETTING_ACCOUNT;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page==FEISHU_PAGE_ACCOUNT);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_REAUTHORIZE);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_DOWN) == FEISHU_NAV_NONE);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_CONFIRM);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_UNBIND);
}

static void test_compose_shortcut(void)
{
    for(size_t count=0;count<=FEISHU_MAX_MESSAGES;++count)for(size_t i=0;i<(count ? count:1);++i) {
        feishu_nav_t focus={.page=FEISHU_PAGE_MESSAGES,.message_count=count,.message_index=i};
        assert(feishu_nav_handle(&focus,FEISHU_NAV_COMPOSE)==FEISHU_NAV_NONE && focus.composer_selected);
        assert(feishu_nav_handle(&focus,FEISHU_NAV_BROWSE)==FEISHU_NAV_NONE);
        assert(focus.composer_selected==(count==0) && focus.message_index==i && focus.page==FEISHU_PAGE_MESSAGES);
        feishu_nav_t nav={.page=FEISHU_PAGE_MESSAGES,.message_count=count,.message_index=i};
        assert(feishu_nav_handle(&nav,FEISHU_NAV_COMPOSE)==FEISHU_NAV_NONE);
        assert(nav.page==FEISHU_PAGE_MESSAGES && nav.composer_selected && nav.message_index==i);
        assert(feishu_nav_handle(&nav,FEISHU_NAV_COMPOSE)==FEISHU_NAV_NONE);
        assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_START_RECORDING);
    }
    feishu_nav_t nav={.page=FEISHU_PAGE_SETTINGS,.settings_index=FEISHU_SETTING_XIAOZHI};
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_CHECK_XIAOZHI && nav.page==FEISHU_PAGE_XIAOZHI);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_COMPOSE)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_XIAOZHI);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_CHECK_XIAOZHI);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_BACK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_SETTINGS);
    nav.settings_index=FEISHU_SETTING_RETURN;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_CHATS);
}

static void test_draft_actions(void)
{
    feishu_nav_t nav={.page=FEISHU_PAGE_DRAFT_ACTIONS,.settings_index=0};
    assert(feishu_nav_handle(&nav,FEISHU_NAV_DOWN)==FEISHU_NAV_NONE);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_RETRY_RECORDING);
    assert(nav.page==FEISHU_PAGE_RECORDING);
    nav.page=FEISHU_PAGE_DRAFT_ACTIONS;nav.settings_index=2;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_CANCEL_REPLY && nav.page==FEISHU_PAGE_MESSAGES);
    nav.page=FEISHU_PAGE_DRAFT_ACTIONS;nav.settings_index=0;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_BACK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_REVIEW);
}
static void test_image_actions(void)
{
    feishu_nav_t nav={.page=FEISHU_PAGE_IMAGE_ACTIONS,.settings_index=1};
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_IMAGE_ZOOM_IN && nav.page==FEISHU_PAGE_IMAGE);
    nav.page=FEISHU_PAGE_IMAGE_ACTIONS;nav.settings_index=2;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_IMAGE_ZOOM_OUT);
    nav.page=FEISHU_PAGE_IMAGE_ACTIONS;nav.settings_index=3;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_IMAGE_AXIS);
    nav.page=FEISHU_PAGE_IMAGE_ACTIONS;nav.settings_index=4;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_LOAD_IMAGE);
    nav.page=FEISHU_PAGE_IMAGE_ACTIONS;nav.settings_index=5;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_IMAGE);
}

static void test_global_drafts(void)
{
    feishu_nav_t nav={.page=FEISHU_PAGE_SETTINGS,.settings_index=FEISHU_SETTING_DRAFTS};
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_LIST_DRAFTS);
    assert(nav.page==FEISHU_PAGE_DRAFTS && nav.draft_manager && nav.draft_count==0);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE);
    nav.draft_count=4;
    for(unsigned i=0;i<8;++i)feishu_nav_handle(&nav,FEISHU_NAV_DOWN);
    assert(nav.draft_index==3);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_OPEN_DRAFT);
    assert(nav.page==FEISHU_PAGE_DRAFTS); /* Worker reads durable state before review. */
    for(unsigned i=0;i<8;++i)feishu_nav_handle(&nav,FEISHU_NAV_UP);
    assert(nav.draft_index==0);
    feishu_nav_handle(&nav,FEISHU_NAV_BACK);
    assert(nav.page==FEISHU_PAGE_CHATS && !nav.draft_manager);
    nav.page=FEISHU_PAGE_SETTINGS;nav.settings_index=FEISHU_SETTING_RETURN;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_CHATS);
    nav.page=FEISHU_PAGE_ACCOUNT;nav.settings_index=1;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_CONFIRM);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_UP)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_SETTINGS);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_ACCOUNT);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_REAUTHORIZE);
}
int main(void)
{
    assert(feishu_focus_step(9,12,1,false)==10);
    assert(feishu_focus_step(10,12,-1,false)==9);
    assert(feishu_focus_step(11,12,-1,false)==10);
    assert(feishu_focus_step(0,12,-1,false)==0 && feishu_focus_step(11,12,1,false)==11);
    assert(feishu_focus_step(0,31,-1,true)==30 && feishu_focus_step(30,31,1,true)==0);
    assert(feishu_focus_step(0,0,-1,false)==0);
    {
        bool characters[128]={0};char password[65]={0};
        for(unsigned p=0;p<4;++p)for(size_t k=0;k<feishu_wifi_key_count(p);++k) {
            const char *key=feishu_wifi_keys[p][k];if(strlen(key)!=1)continue;
            unsigned page=p;size_t selection=k;password[0]=0;
            assert(feishu_wifi_key_apply(&page,&selection,password)==0 && password[0]==key[0] && !password[1]);
            characters[(unsigned char)key[0]]=true;
        }
        for(unsigned c=32;c<=126;++c)assert(characters[c]);
        password[0]=0;unsigned page=0;size_t key=0;
        for(unsigned i=0;i<64;++i)assert(feishu_wifi_key_apply(&page,&key,password)==0);
        assert(strlen(password)==64 && feishu_wifi_key_apply(&page,&key,password)==-1);
        key=29;assert(!feishu_wifi_key_apply(&page,&key,password) && strlen(password)==63);
        key=30;assert(feishu_wifi_key_apply(&page,&key,password)==1 && strlen(password)==63);
        key=27;assert(!feishu_wifi_key_apply(&page,&key,password) && page==1 && key==0);
        memset(password,0,sizeof(password));
        assert(feishu_wifi_password_valid("") && !feishu_wifi_password_valid("short") && feishu_wifi_password_valid("eight123"));
        memset(password,'a',64);assert(feishu_wifi_password_valid(password));password[1]='z';assert(!feishu_wifi_password_valid(password));
    }
    assert(feishu_pairing_auto_due(100,true,false,false,false,0,0));
    assert(!feishu_pairing_auto_due(100,false,false,false,false,0,0));
    assert(!feishu_pairing_auto_due(100,true,true,false,false,0,0));
    assert(!feishu_pairing_auto_due(100,true,false,true,false,0,0));
    assert(!feishu_pairing_auto_due(100,true,false,false,true,200,0));
    assert(feishu_pairing_auto_due(200,true,false,false,true,200,0));
    assert(!feishu_pairing_auto_due(29999,true,false,false,false,0,30000));
    assert(feishu_pairing_auto_due(30000,true,false,false,false,0,30000));
    assert(!feishu_pairing_auto_due(UINT32_MAX-10,true,false,false,false,0,5));
    assert(feishu_pairing_auto_due(6,true,false,false,false,0,5));
    for(uint32_t t=100;t<30100;++t)assert(!feishu_pairing_auto_due(t,true,false,false,false,0,30100));
    {
        uint16_t short_rows[]={28,28,28,28,28,28,28,28};size_t first;
        assert(feishu_message_window(short_rows,8,8,190,5,&first)==5 && first==3);
        assert(feishu_message_window(short_rows,8,0,190,5,&first)==5 && first==0);
        uint16_t mixed[]={44,44,44,44,28,28,28,28};
        for(size_t selected=0;selected<=8;++selected) {
            size_t n=feishu_message_window(mixed,8,selected,190,5,&first);
            size_t anchor=selected<8?selected:7;assert(first<=anchor && first+n>anchor);
            unsigned used=0;for(size_t i=0;i<n;++i)used+=mixed[first+i]+(i?4:0);
            assert(n<=5 && used<=190);
        }
        assert(feishu_message_window(mixed,0,0,190,5,&first)==0);
        assert(feishu_message_window(mixed,8,0,20,5,&first)==0);
        /* Every short/two-line mixture keeps focus visible without overlapping
         * the composer, including a selected index beyond the last message. */
        for(unsigned pattern=0;pattern<256;++pattern) {
            uint16_t heights[8];
            for(unsigned i=0;i<8;++i)heights[i]=(pattern&(1u<<i))?44:28;
            for(size_t selected=0;selected<=9;++selected) {
                size_t n=feishu_message_window(heights,8,selected,190,5,&first);
                size_t anchor=selected<8?selected:7;
                assert(n && n<=5 && first+n<=8 && first<=anchor && first+n>anchor);
                unsigned used=0;
                for(size_t i=0;i<n;++i)used+=heights[first+i]+(i?4:0);
                assert(used<=190);
            }
        }
    }

    char avatar[5];
    feishu_avatar_text(avatar,"【AI工卡】飞书","群");assert(!strcmp(avatar,"AI"));
    feishu_avatar_text(avatar," [botmux] codex","群");assert(!strcmp(avatar,"BO"));
    feishu_avatar_text(avatar,"【飞书】测试","群");assert(!strcmp(avatar,"飞"));
    feishu_avatar_text(avatar,"😀【 】","群");assert(!strcmp(avatar,"群"));
    feishu_avatar_text(avatar,NULL,"他");assert(!strcmp(avatar,"他"));
    char preview[91];
    feishu_text_preview(preview,sizeof(preview),"\n\n  前部文字预览，后面的超长内容需要省略显示，再来一段文字",false,22,2);
    assert(!strncmp(preview,"前部文字预览",strlen("前部文字预览")));
    assert(strstr(preview,"...") && strchr(preview,'\n'));
    feishu_text_preview(preview,sizeof(preview),"  短消息\n测试 ",false,22,2);
    assert(!strcmp(preview,"短消息 测试"));
    feishu_text_preview(preview,sizeof(preview),"已有前缀",true,22,2);
    assert(!strcmp(preview,"已有前缀..."));
    feishu_text_preview(preview,1,"中文",true,22,2);assert(!preview[0]);
    feishu_text_preview(preview,sizeof(preview),"正常恢复",false,22,2);assert(!strcmp(preview,"正常恢复"));
    test_unread();
    test_utf8_copy();
    test_navigation();
    test_compose_shortcut();
    test_draft_actions();
    test_image_actions();
    test_global_drafts();
    return 0;
}
