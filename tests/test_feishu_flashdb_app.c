#include "feishu_host/cache_flashdb_fixture.h"
#include "feishu_store.h"
#define FEISHU_STORE_FRESH_POOL 1
#define FEISHU_STORE_DIRECTORY directory
#include "../main/feishu_store.c"
static unsigned cleanups;
void esp_fill_random(void *data,size_t size)
{static uint64_t n=100;assert(size==8);++n;memcpy(data,&n,8);}
esp_err_t feishu_legacy_cleanup(void) {++cleanups;return ESP_OK;}
/* Fresh-pool builds must never consult legacy credentials. */
esp_err_t feishu_legacy_load_app_credentials(feishu_credentials_t *c) {(void)c;assert(0);return ESP_FAIL;}
esp_err_t feishu_legacy_load_credentials(feishu_credentials_t *c) {(void)c;assert(0);return ESP_FAIL;}
esp_err_t feishu_legacy_load_access_token(char *p,size_t n) {(void)p;(void)n;assert(0);return ESP_FAIL;}
esp_err_t feishu_legacy_load_cache_owner(char *p,size_t n) {(void)p;(void)n;assert(0);return ESP_FAIL;}
esp_err_t feishu_legacy_load_seen(feishu_seen_t *p,size_t n,size_t *c) {(void)p;(void)n;(void)c;assert(0);return ESP_FAIL;}
bool feishu_legacy_wifi_setup_required(void) {assert(0);return false;}
static void draft(feishu_outbox_t *p)
{
    feishu_data_writer_t w={0};assert(feishu_cache_draft_begin(&w)==ESP_OK);
    assert(feishu_cache_text_append(&w,"durable draft",13)==ESP_OK);
    assert(feishu_outbox_create(p,w.id,w.raw,"draft_self","oc_test","","stable-uuid"));
    assert(feishu_cache_pending_write(0,p,&w)==ESP_OK && !w.id);
}
int main(void)
{
    assert(feishu_cache_init()==ESP_OK);
    feishu_credentials_t credentials={0};
    assert(feishu_store_load_app_credentials(&credentials)==ESP_ERR_NVS_NOT_FOUND);
    assert(feishu_store_wifi_setup_required() && cleanups==1);
    strcpy(credentials.app_id,"cli_fresh");strcpy(credentials.app_secret,"fake-secret");
    assert(feishu_store_save_app_credentials(&credentials)==ESP_OK);
    uint8_t wifi[128];memset(wifi,0x51,sizeof(wifi));assert(feishu_store_save_wifi(wifi,sizeof(wifi))==ESP_OK);
    assert(feishu_store_require_wifi_setup(false)==ESP_OK);
    /* A short configuration must load into a larger destination after reopen;
     * the mirrored-root API requires exact slice sizes, unlike fread. */
    const char xiao[]="{\"uuid\":\"fixture\",\"url\":\"wss://example.invalid/\",\"token\":\"fake-token\"}";
    char config[FEISHU_XIAOZHI_CONFIG_MAX],small[8];
    assert(feishu_store_load_xiaozhi(config,sizeof(config))==ESP_ERR_NVS_NOT_FOUND);
    assert(feishu_store_save_xiaozhi(xiao)==ESP_OK);
    memset(config,0x5a,sizeof(config));
    assert(feishu_store_load_xiaozhi(config,sizeof(config))==ESP_OK && !strcmp(config,xiao));
    assert(config[sizeof(config)-1]==0x5a);
    assert(feishu_store_load_xiaozhi(small,sizeof(small))==ESP_ERR_INVALID_SIZE && !small[0]);
    feishu_cache_set_scope("cli_fresh","ou_self");
    feishu_outbox_t p,loaded;draft(&p);
    char first_uuid[41],retry_uuid[41];assert(feishu_outbox_uuid(&p,first_uuid));
    assert(feishu_outbox_attempt(&p,false));assert(feishu_cache_pending_write(0,&p,NULL)==ESP_OK);
    feishu_cache_unmount();migrated=false;
    assert(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.state==FEISHU_OUTBOX_SENDING);
    assert(!feishu_outbox_attempt(&loaded,false));
    assert(feishu_outbox_attempt(&loaded,true) && feishu_outbox_uuid(&loaded,retry_uuid) && !strcmp(first_uuid,retry_uuid));
    feishu_outbox_failed(&loaded,false);assert(feishu_cache_pending_write(0,&loaded,NULL)==ESP_OK);
    /* A read exception cannot masquerade as an absent draft; the next action
     * reopens the raw engine and restores the normal path. */
    feishu_fdb_host_fault(0,-1,false);
    assert(feishu_cache_pending_read(0,&p)!=ESP_OK && !feishu_cache_pending_empty());
    feishu_fdb_host_fault(-1,-1,false);
    assert(feishu_cache_pending_read(0,&p)==ESP_OK && p.state==FEISHU_OUTBOX_UNCERTAIN);
    assert(!feishu_outbox_attempt(&p,false));
    assert(feishu_store_load_app_credentials(&credentials)==ESP_OK && !strcmp(credentials.app_id,"cli_fresh"));
    uint8_t check[128];assert(feishu_store_load_wifi(check,sizeof(check))==ESP_OK && !memcmp(check,wifi,sizeof(wifi)));
    /* Repeated cache versions must give room back without evicting a draft. */
    uint8_t block[1024];for(unsigned i=0;i<sizeof(block);++i)block[i]=(uint8_t)((i*17+i/7)%251);
    for(unsigned i=0;i<170;++i) {
        char key[32];snprintf(key,sizeof(key),"om_%u",i);
        feishu_data_writer_t w={0};int64_t id=0;
        assert(feishu_cache_text_begin(&w)==ESP_OK);
        assert(feishu_cache_text_append(&w,block,sizeof(block))==ESP_OK);
        assert(feishu_cache_text_commit(&w,key,&id)==ESP_OK);
    }
    assert(feishu_cache_pending_read(0,&loaded)==ESP_OK && loaded.object_id==p.object_id);
    assert(feishu_outbox_attempt(&loaded,true));assert(feishu_outbox_ack(&loaded,loaded.length));
    assert(feishu_cache_pending_write(0,&loaded,NULL)==ESP_OK);
    assert(feishu_cache_pending_read(0,&p)==ESP_ERR_NOT_FOUND);
    /* Four protected slots must survive reopening independently. Cancelling a
     * staged re-record must not replace an existing saved draft. */
    feishu_outbox_t slots[FEISHU_OUTBOX_SLOTS];
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
        feishu_data_writer_t w={0};char name[32],uuid[32],text[32];
        snprintf(name,sizeof(name),"draft_slot_%u",i);
        snprintf(uuid,sizeof(uuid),"stable_slot_%u",i);
        snprintf(text,sizeof(text),"saved slot %u",i);
        assert(feishu_cache_draft_begin(&w)==ESP_OK);
        assert(feishu_cache_text_append(&w,text,strlen(text))==ESP_OK);
        assert(feishu_outbox_create(&slots[i],w.id,w.raw,name,"oc_test","",uuid));
        assert(feishu_cache_pending_write(i,&slots[i],&w)==ESP_OK);
    }
    feishu_data_writer_t cancelled={0};
    assert(feishu_cache_draft_begin(&cancelled)==ESP_OK);
    assert(feishu_cache_text_append(&cancelled,"cancelled replacement",21)==ESP_OK);
    feishu_cache_text_abort(&cancelled);
    feishu_cache_unmount();
    for(unsigned i=0;i<FEISHU_OUTBOX_SLOTS;++i) {
        char text[32],out[32];size_t n=0;
        snprintf(text,sizeof(text),"saved slot %u",i);
        assert(feishu_cache_pending_read(i,&loaded)==ESP_OK && loaded.state==FEISHU_OUTBOX_READY &&
            loaded.object_id==slots[i].object_id && !strcmp(loaded.uuid,slots[i].uuid));
        assert(feishu_cache_text_read_stream(loaded.body_name,loaded.object_id,false,0,out,sizeof(out),&n)==ESP_OK &&
            n==strlen(text) && !memcmp(out,text,n));
    }
    assert(feishu_store_load_xiaozhi(config,sizeof(config))==ESP_OK && !strcmp(config,xiao));
    feishu_cache_unmount();assert(!lock_depth);
    puts("FlashDB app: fresh setup, no legacy import, shared configuration/cache, durable draft, uncertain send/retry UUID, IO reopen and cache pressure PASS");
    return 0;
}
