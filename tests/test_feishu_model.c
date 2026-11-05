#include "feishu_model.h"

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
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_REPLY_READY);
    assert(!nav.reply_to_message);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_START_RECORDING);
    assert(nav.page == FEISHU_PAGE_RECORDING);

    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_STOP_RECORDING);
    nav.page = FEISHU_PAGE_MESSAGES;
    nav.composer_selected = true;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_UP) == FEISHU_NAV_NONE);
    assert(!nav.composer_selected && nav.message_index == 2);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_MESSAGE_DETAIL);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_REPLY_READY && nav.reply_to_message);
    nav.page = FEISHU_PAGE_IMAGE;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page==FEISHU_PAGE_IMAGE_ACTIONS);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK)==FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_REPLY_READY);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_BACK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_MESSAGES);
    nav.page = FEISHU_PAGE_REVIEW;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_SEND_REPLY);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_UP) == FEISHU_NAV_RETRY_RECORDING);
    assert(nav.page == FEISHU_PAGE_RECORDING);

    nav.page = FEISHU_PAGE_CHATS;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_BACK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_SETTINGS);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(nav.page == FEISHU_PAGE_CONFIRM);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_RESET_WIFI);
    nav.page = FEISHU_PAGE_SETTINGS;
    nav.settings_index = 1;
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_NONE);
    assert(feishu_nav_handle(&nav, FEISHU_NAV_OK) == FEISHU_NAV_UNBIND);
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
    feishu_nav_t nav={.page=FEISHU_PAGE_SETTINGS,.settings_index=2};
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
    nav.page=FEISHU_PAGE_SETTINGS;nav.settings_index=4;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_CHATS);
    nav.page=FEISHU_PAGE_SETTINGS;nav.settings_index=3;
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_CONFIRM);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_UP)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_SETTINGS);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_NONE && nav.page==FEISHU_PAGE_CONFIRM);
    assert(feishu_nav_handle(&nav,FEISHU_NAV_OK)==FEISHU_NAV_REAUTHORIZE);
}
int main(void)
{
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
    test_draft_actions();
    test_image_actions();
    test_global_drafts();
    return 0;
}
