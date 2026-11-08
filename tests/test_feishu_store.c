#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "feishu_store.h"
#include "nvs.h"
static char directory[128];
#define FEISHU_STORE_DIRECTORY directory
/* Reset the translation unit's RAM-only migration flag to model process reboot;
 * file operations, serialization and account-epoch checks are production code. */
#include "../main/feishu_store.c"

static unsigned lock_depth,imports,cleanups;
static uint64_t random_counter=10;
static feishu_credentials_t legacy;
static char legacy_access[FEISHU_TOKEN_MAX];
void esp_fill_random(void *data,size_t size)
{ assert(size==8);++random_counter;memcpy(data,&random_counter,8); }
esp_err_t feishu_cache_pool_lock(void) {++lock_depth;return ESP_OK;}
void feishu_cache_pool_unlock(void) {assert(lock_depth);--lock_depth;}
esp_err_t feishu_cache_release_space(void) {return ESP_FAIL;}
esp_err_t feishu_legacy_load_app_credentials(feishu_credentials_t *c)
{ ++imports;*c=legacy;c->refresh_token[0]=0;return ESP_OK; }
esp_err_t feishu_legacy_load_credentials(feishu_credentials_t *c) {*c=legacy;return ESP_OK;}
esp_err_t feishu_legacy_load_access_token(char *t,size_t n)
{ assert(n>strlen(legacy_access));strcpy(t,legacy_access);return ESP_OK; }
esp_err_t feishu_legacy_load_cache_owner(char *o,size_t n)
{ assert(n>strlen("ou_fake_old"));strcpy(o,"ou_fake_old");return ESP_OK; }
esp_err_t feishu_legacy_load_seen(feishu_seen_t *seen,size_t n,size_t *count)
{ assert(n);strcpy(seen[0].chat_id,"oc_fake");seen[0].last_seen_time_ms=12;*count=1;return ESP_OK; }
bool feishu_legacy_wifi_setup_required(void) {return true;}
/* Pretend cleanup failed: the migration marker must still prevent resurrection. */
esp_err_t feishu_legacy_cleanup(void) {++cleanups;return ESP_FAIL;}

int main(int argc,char **argv)
{
    assert(argc==2 && strlen(argv[1])<sizeof(directory));strcpy(directory,argv[1]);
    strcpy(legacy.app_id,"cli_fake_old");strcpy(legacy.app_secret,"fake-secret");
    memset(legacy.refresh_token,'R',7500);legacy.refresh_token[7500]=0;
    memset(legacy_access,'A',7500);legacy_access[7500]=0;
    feishu_credentials_t c;char token[FEISHU_TOKEN_MAX],owner[64];
    assert(!feishu_store_load_credentials(&c));assert(!memcmp(&c,&legacy,sizeof(c)));
    assert(!feishu_store_load_access_token(token,sizeof(token)) && !strcmp(token,legacy_access));
    assert(!feishu_store_load_cache_owner(owner,sizeof(owner)) && !strcmp(owner,"ou_fake_old"));
    assert(imports==1 && cleanups==1 && !lock_depth);
    assert(feishu_store_wifi_setup_required());
    unsigned auth_request=99;
    assert(!feishu_store_get_auth_request(&auth_request) && auth_request==FEISHU_AUTH_NONE);
    for(unsigned i=FEISHU_AUTH_REAUTHORIZE;i<=FEISHU_AUTH_REPAIR_APP;++i) {
        assert(!feishu_store_request_auth(i));migrated=false;
        assert(!feishu_store_get_auth_request(&auth_request) && auth_request==i);
        assert(!feishu_store_load_credentials(&c) && !memcmp(&c,&legacy,sizeof(c)));
    }
    assert(feishu_store_request_auth(99)==ESP_ERR_INVALID_ARG);
    assert(!feishu_store_request_auth(FEISHU_AUTH_NONE));
    unsigned char wifi[108],got_wifi[108];memset(wifi,0,sizeof(wifi));memcpy(wifi,"fake-ssid",9);
    assert(!begin());assert(!save_value(ROOT_WIFI,wifi,sizeof(wifi)));assert(!end(ESP_OK));
    assert(!feishu_store_save_wifi(wifi,sizeof(wifi)));
    migrated=false;
    assert(!feishu_store_load_credentials(&c));assert(!memcmp(&c,&legacy,sizeof(c)));
    assert(imports==1 && cleanups==1);
    assert(!feishu_store_load_wifi(got_wifi,sizeof(got_wifi)) && !memcmp(wifi,got_wifi,sizeof(wifi)));
    puts("store large legacy import, marker/reboot and shared Wi-Fi record: PASS");
    {
        char names[FEISHU_WIFI_SAVED_MAX][33];size_t count,active;
        assert(!feishu_store_list_wifi(names,&count,&active,sizeof(wifi)) && count==1 && active==0 && !strcmp(names[0],"fake-ssid"));
        for(unsigned i=1;i<10;++i){memset(wifi,0,sizeof(wifi));snprintf((char *)wifi,32,"test-network-%u",i);memset(wifi+32,'a'+i,64);assert(!feishu_store_save_wifi(wifi,sizeof(wifi)));}
        assert(!feishu_store_list_wifi(names,&count,&active,sizeof(wifi)) && count==10 && active==9);
        memset(wifi,0,sizeof(wifi));memcpy(wifi,"eleventh-network",16);
        assert(feishu_store_save_wifi(wifi,sizeof(wifi))==FEISHU_STORE_ERR_FULL);
        assert(!feishu_store_load_wifi(got_wifi,sizeof(got_wifi)) && !strcmp((char *)got_wifi,"test-network-9"));
        assert(!feishu_store_get_wifi(0,wifi,sizeof(wifi)) && !strcmp((char *)wifi,"fake-ssid"));
        assert(!feishu_store_save_wifi(wifi,sizeof(wifi)));
        migrated=false;
        assert(!feishu_store_list_wifi(names,&count,&active,sizeof(wifi)) && count==10 && active==0);
        assert(feishu_store_get_wifi(10,wifi,sizeof(wifi))==ESP_ERR_INVALID_ARG);
        assert(!feishu_store_get_wifi(0,wifi,sizeof(wifi)));
        unsigned char original[108];size_t total=0;
        assert(!begin());assert(!read_part(ROOT_WIFI,0,original,sizeof(original),&total));assert(!end(ESP_OK));
        assert(total==sizeof(original) && !memcmp(original,wifi,sizeof(wifi)));
        unsigned char book[FEISHU_WIFI_BOOK_MAX],corrupt[FEISHU_WIFI_BOOK_MAX];
        assert(!begin());assert(!read_part(ROOT_WIFI_BOOK,0,NULL,0,&total));assert(total<=sizeof(book));assert(!read_part(ROOT_WIFI_BOOK,0,book,total,&total));assert(!end(ESP_OK));
        memcpy(corrupt,book,total);((feishu_wifi_book_header_t *)corrupt)->count=11;
        assert(!begin());assert(!save_value(ROOT_WIFI_BOOK,corrupt,total));assert(!end(ESP_OK));
        assert(feishu_store_load_wifi(got_wifi,sizeof(got_wifi))==FEISHU_STORE_ERR_CORRUPT);
        assert(feishu_store_save_wifi(wifi,sizeof(wifi))==FEISHU_STORE_ERR_CORRUPT);
        assert(!begin());assert(!save_value(ROOT_WIFI_BOOK,book,total));assert(!end(ESP_OK));
        assert(!feishu_store_load_wifi(got_wifi,sizeof(got_wifi)) && !memcmp(wifi,got_wifi,sizeof(wifi)));
        assert(!feishu_store_get_wifi(9,got_wifi,sizeof(got_wifi)));
        memcpy(got_wifi+32,"changed-password",16);
        assert(!feishu_store_update_wifi(9,got_wifi,sizeof(got_wifi)));
        assert(!feishu_store_list_wifi(names,&count,&active,sizeof(wifi)) && active==0 && count==10);
        got_wifi[0]='X';assert(feishu_store_update_wifi(9,got_wifi,sizeof(got_wifi))==ESP_ERR_INVALID_ARG);
        assert(feishu_store_remove_wifi(10,sizeof(wifi))==ESP_ERR_INVALID_ARG);
        assert(!feishu_store_remove_wifi(0,sizeof(wifi)));
        assert(!feishu_store_list_wifi(names,&count,&active,sizeof(wifi)) && count==9 && active==0 && !strcmp(names[0],"test-network-1"));
        assert(!feishu_store_save_wifi(wifi,sizeof(wifi)));
        for(size_t remaining=10;remaining;--remaining) {
            assert(!feishu_store_remove_wifi(remaining-1,sizeof(wifi)));
            assert(!feishu_store_list_wifi(names,&count,&active,sizeof(wifi)) && count==remaining-1);
        }
        migrated=false;
        assert(feishu_store_load_wifi(got_wifi,sizeof(got_wifi))==FEISHU_STORE_ERR_WIFI_EMPTY);
        assert(!feishu_store_list_wifi(names,&count,&active,sizeof(wifi)) && !count);
        assert(!feishu_store_save_wifi(wifi,sizeof(wifi)));
        assert(!feishu_store_load_wifi(got_wifi,sizeof(got_wifi)) && !memcmp(wifi,got_wifi,sizeof(wifi)));
        puts("Wi-Fi edit preserves active; forget all is explicit and cannot resurrect the legacy network; re-add PASS");
        puts("Wi-Fi book: ten profiles, full preserves active, reselection/reopen and bounded index PASS");
    }
    char voice[FEISHU_VOICE_COOKIE_MAX];
    assert(feishu_store_load_voice_cookie(voice,sizeof(voice))==ESP_ERR_NVS_NOT_FOUND);
    assert(!feishu_store_save_voice_cookie("session=fake; session_list=fake-list"));
    migrated=false;
    assert(!feishu_store_load_voice_cookie(voice,sizeof(voice)) && !strcmp(voice,"session=fake; session_list=fake-list"));
    assert(feishu_store_save_voice_cookie("session=fake\r\nx-header: injected")==ESP_ERR_INVALID_ARG);
    assert(feishu_store_save_voice_cookie("unrelated=fake")==ESP_ERR_INVALID_ARG);
    memset(voice,'A',sizeof(voice)-1);voice[sizeof(voice)-1]=0;memcpy(voice,"session=",8);
    assert(!feishu_store_save_voice_cookie(voice));
    assert(!feishu_store_load_voice_cookie(token,sizeof(token)) && strlen(token)==4096);
    assert(feishu_store_load_voice_cookie(owner,sizeof(owner))==ESP_ERR_NVS_NOT_FOUND && !owner[0]);

    strcpy(c.app_id,"cli_fake_new");strcpy(c.app_secret,"new-secret");
    assert(!feishu_store_save_app_credentials(&c));
    assert(feishu_store_load_voice_cookie(voice,sizeof(voice))==ESP_ERR_NVS_NOT_FOUND && !voice[0]);
    assert(feishu_store_load_credentials(&c)==ESP_ERR_NVS_NOT_FOUND);
    assert(!strcmp(c.app_id,"cli_fake_new"));
    assert(feishu_store_load_access_token(token,sizeof(token))==ESP_ERR_NVS_NOT_FOUND && !token[0]);
    assert(feishu_store_load_cache_owner(owner,sizeof(owner))==ESP_ERR_NVS_NOT_FOUND);
    strcpy(c.refresh_token,"fake-new-refresh");assert(!feishu_store_save_credentials(&c));
    assert(!feishu_store_save_access_token("fake-new-access"));
    assert(!feishu_store_save_cache_owner("ou_fake_new"));
    memset(token,'T',8000);token[8000]=0;
    assert(!feishu_store_save_refresh_token(token));
    assert(!feishu_store_load_credentials(&c) && !strcmp(c.refresh_token,token));
    assert(!feishu_store_load_access_token(token,sizeof(token)) && !strcmp(token,"fake-new-access"));
    assert(!feishu_store_clear_user_tokens());migrated=false;
    assert(feishu_store_load_credentials(&c)==ESP_ERR_NVS_NOT_FOUND);
    assert(!strcmp(c.app_id,"cli_fake_new") && !strcmp(c.app_secret,"new-secret"));
    assert(feishu_store_load_access_token(token,sizeof(token))==ESP_ERR_NVS_NOT_FOUND);
    assert(feishu_store_load_cache_owner(owner,sizeof(owner))==ESP_ERR_NVS_NOT_FOUND);
    assert(!feishu_store_load_wifi(got_wifi,sizeof(got_wifi)) && !memcmp(wifi,got_wifi,sizeof(wifi)));
    puts("store binding changes isolate tokens; rotation and revocation preserve required settings: PASS");

    feishu_seen_t seen[FEISHU_MAX_CHATS];size_t count=0;
    assert(!feishu_store_mark_seen("oc_fake",30));assert(!feishu_store_mark_seen("oc_fake",20));
    assert(!feishu_store_load_seen(seen,FEISHU_MAX_CHATS,&count));
    assert(count==1 && seen[0].last_seen_time_ms==30);
    assert(!feishu_store_clear_credentials());migrated=false;
    assert(feishu_store_load_app_credentials(&c)==ESP_ERR_NVS_NOT_FOUND);
    assert(imports==1 && !lock_depth);
    /* Corrupting the disposable DB cannot block recovery configuration reads. */
    char path[160];snprintf(path,sizeof(path),"%s/objects.db",directory);
    FILE *f=fopen(path,"wb");assert(f);fputs("not-a-database",f);fclose(f);
    assert(!feishu_store_load_wifi(got_wifi,sizeof(got_wifi)) && !memcmp(wifi,got_wifi,sizeof(wifi)));
    assert(!feishu_store_require_wifi_setup(false));assert(!feishu_store_wifi_setup_required());
    puts("store seen monotonicity, no legacy resurrection and database-independent recovery: PASS");
    feishu_assistant_settings_t settings;
    assert(!feishu_store_load_assistant(&settings) && !settings.boot && settings.silence_seconds==2 && settings.volume==60 && settings.timeout_minutes==10);
    for(unsigned i=0;i<5;++i){
        settings=(feishu_assistant_settings_t){true,i ? i+1:0,i*25,i*15};
        assert(!feishu_store_save_assistant(&settings));migrated=false;
        feishu_assistant_settings_t restored;
        assert(!feishu_store_load_assistant(&restored) && restored.boot && restored.silence_seconds==settings.silence_seconds && restored.volume==settings.volume && restored.timeout_minutes==settings.timeout_minutes);
    }
    uint8_t legacy_assistant[]={1,1,4,0};
    assert(!save_value(ROOT_ASSISTANT,legacy_assistant,sizeof(legacy_assistant)));
    assert(!feishu_store_load_assistant(&settings) && settings.boot && settings.silence_seconds==4 && settings.volume==60 && settings.timeout_minutes==10);
    uint8_t v2_assistant[]={2,0,2,80};
    assert(!save_value(ROOT_ASSISTANT,v2_assistant,sizeof(v2_assistant)));
    assert(!feishu_store_load_assistant(&settings) && settings.volume==80 && settings.timeout_minutes==10);
    settings.timeout_minutes=61;assert(feishu_store_save_assistant(&settings)==ESP_ERR_INVALID_ARG);settings.timeout_minutes=10;
    settings.volume=101;assert(feishu_store_save_assistant(&settings)==ESP_ERR_INVALID_ARG);settings.volume=60;
    settings.silence_seconds=1;assert(feishu_store_save_assistant(&settings)==ESP_ERR_INVALID_ARG);
    settings.silence_seconds=6;assert(feishu_store_save_assistant(&settings)==ESP_ERR_INVALID_ARG);
    uint8_t bad_settings[]={1,1,9,0};
    assert(!save_value(ROOT_ASSISTANT,bad_settings,sizeof(bad_settings)));
    assert(feishu_store_load_assistant(&settings)==FEISHU_STORE_ERR_CORRUPT && !settings.boot && settings.silence_seconds==2 && settings.volume==60 && settings.timeout_minutes==10);
    puts("assistant settings: defaults, all choices, reopen, invalid and corrupt record: PASS");
    feishu_sound_settings_t sound;
    assert(!feishu_store_load_sounds(&sound) && sound.success && sound.failure && sound.key);
    for(unsigned mask=0;mask<8;++mask){
        sound=(feishu_sound_settings_t){(mask&1)!=0,(mask&2)!=0,(mask&4)!=0};
        assert(!feishu_store_save_sounds(&sound));migrated=false;
        feishu_sound_settings_t got;
        assert(!feishu_store_load_sounds(&got) && got.success==sound.success && got.failure==sound.failure && got.key==sound.key);
    }
    uint8_t bad_sound[]={1,8};assert(!save_value(ROOT_SOUNDS,bad_sound,sizeof(bad_sound)));
    assert(feishu_store_load_sounds(&sound)==FEISHU_STORE_ERR_CORRUPT && sound.success && sound.failure && sound.key);
    puts("sound settings: independent combinations, reboot and corrupt fallback PASS");
    feishu_power_settings_t power;
    assert(!feishu_store_load_power(&power) && power.screen_minutes==2 && power.cpu==2 && power.refresh==0 && power.wifi==2);
    for(unsigned m=0;m<3;++m){
        power=(feishu_power_settings_t){(uint8_t)(m+2),m,m,m};
        assert(!feishu_store_save_power(&power));migrated=false;
        feishu_power_settings_t got;assert(!feishu_store_load_power(&got));assert(!memcmp(&got,&power,sizeof(got)));
    }
    uint8_t bad_power[]={1,2,3,0,2};assert(!save_value(ROOT_POWER,bad_power,sizeof(bad_power)));migrated=false;
    assert(feishu_store_load_power(&power)==FEISHU_STORE_ERR_CORRUPT && power.screen_minutes==2 && power.cpu==2);
    power=feishu_power_defaults();assert(!feishu_store_save_power(&power));
    puts("power settings: defaults, persist/reopen, invalid and corrupt recovery PASS");
    return 0;
}
