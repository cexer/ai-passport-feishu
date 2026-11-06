/* Opt-in synthetic LAN diagnostic. Never loads tokens or sends IM messages.
 * Credentials are read by the ordinary Wi-Fi driver path, never printed. */
#include "network_probe_config.h"
#include "feishu_network.h"
#include "feishu_theme.h"
#include "ui_pixel.h"
#include "bsp_display.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void link_info(void)
{
    wifi_ap_record_t ap={0};wifi_config_t config={0};wifi_ps_type_t ps=WIFI_PS_NONE;
    int8_t power=0;uint8_t protocol=0;wifi_bandwidth_t bandwidth=WIFI_BW_HT20;
    int rc=esp_wifi_sta_get_ap_info(&ap);
    esp_wifi_get_config(WIFI_IF_STA,&config);esp_wifi_get_ps(&ps);
    esp_wifi_get_max_tx_power(&power);esp_wifi_get_protocol(WIFI_IF_STA,&protocol);
    esp_wifi_get_bandwidth(WIFI_IF_STA,&bandwidth);
    printf("NETPROBE_LINK rc=%d expected_ssid=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d channel=%u pinned=%u hint_channel=%u scan=%u sort=%u ps=%u tx_quarter_dbm=%d protocol=%u bandwidth=%u\n",
        rc,(unsigned)!strcmp((char *)ap.ssid,NETWORK_PROBE_SSID),ap.bssid[0],ap.bssid[1],ap.bssid[2],ap.bssid[3],ap.bssid[4],ap.bssid[5],
        ap.rssi,ap.primary,(unsigned)config.sta.bssid_set,config.sta.channel,config.sta.scan_method,config.sta.sort_method,ps,power,protocol,bandwidth);
    memset(&config,0,sizeof(config));
    esp_netif_ip_info_t ip={0};esp_netif_t *netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif && esp_netif_get_ip_info(netif,&ip)==ESP_OK)printf("NETPROBE_IP ip=" IPSTR " gateway=" IPSTR "\n",IP2STR(&ip.ip),IP2STR(&ip.gw));
    esp_netif_dns_info_t dns={0};
    if(netif && esp_netif_get_dns_info(netif,ESP_NETIF_DNS_MAIN,&dns)==ESP_OK && dns.ip.type==ESP_IPADDR_TYPE_V4)
        printf("NETPROBE_DNS server=" IPSTR "\n",IP2STR(&dns.ip.u_addr.ip4));
}
static void scan(void)
{
    wifi_scan_config_t config={.ssid=(uint8_t *)NETWORK_PROBE_SSID,.show_hidden=false};
    int rc=esp_wifi_scan_start(&config,true);uint16_t count=8;
    wifi_ap_record_t *ap=calloc(count,sizeof(*ap));
    if(!ap){printf("NETPROBE_SCAN rc=nomem\n");return;}
    if(rc==ESP_OK)rc=esp_wifi_scan_get_ap_records(&count,ap);
    printf("NETPROBE_SCAN rc=%d matches=%u\n",rc,(unsigned)(rc==ESP_OK?count:0));
    if(rc==ESP_OK)for(unsigned i=0;i<count;++i)printf("NETPROBE_AP bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d channel=%u\n",
        ap[i].bssid[0],ap[i].bssid[1],ap[i].bssid[2],ap[i].bssid[3],ap[i].bssid[4],ap[i].bssid[5],ap[i].rssi,ap[i].primary);
    free(ap);vTaskDelay(pdMS_TO_TICKS(300));
}
static bool transfer(bool tls,unsigned total,unsigned chunk,unsigned pace,const char *phase)
{
    esp_transport_handle_t t=tls?esp_transport_ssl_init():esp_transport_tcp_init();
    if(!t)return false;
    if(tls)esp_transport_ssl_set_cert_data(t,NETWORK_PROBE_CA,(int)strlen(NETWORK_PROBE_CA)+1);
    int64_t began=esp_timer_get_time();errno=0;
    int rc=esp_transport_connect(t,NETWORK_PROBE_HOST,tls?NETWORK_PROBE_TLS_PORT:NETWORK_PROBE_TCP_PORT,5000);
    unsigned connect_ms=(unsigned)((esp_timer_get_time()-began)/1000),done=0,slow=0,partial=0;
    int failure=0,err=errno;unsigned char *buffer=NULL;unsigned send_ms=0,ack_ms=0;bool pass=false;
    if(rc!=0){failure=1;goto finish;}
    int one=1;setsockopt(esp_transport_get_socket(t),IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    uint32_t header=htonl(total);
    if(esp_transport_write(t,(char *)&header,4,1000)!=4){failure=2;goto finish;}
    buffer=malloc(chunk);if(!buffer){failure=3;goto finish;}
    int64_t sending=esp_timer_get_time();
    while(done<total) {
        unsigned n=total-done;if(n>chunk)n=chunk;
        for(unsigned i=0;i<n;++i)buffer[i]=(uint8_t)(done+i);
        unsigned written=0;int64_t deadline=esp_timer_get_time()+1000000;
        while(written<n) {
            int64_t left=deadline-esp_timer_get_time();if(left<=0){failure=4;goto finish;}
            int64_t call=esp_timer_get_time();errno=0;
            int sent=esp_transport_write(t,(char *)buffer+written,(int)(n-written),(int)((left+999)/1000));err=errno;
            unsigned took=(unsigned)((esp_timer_get_time()-call)/1000);if(took>slow)slow=took;
            if(sent<=0){printf("NETPROBE_WRITE tls=%u offset=%u requested=%u returned=%d errno=%d ms=%u\n",(unsigned)tls,done+written,n-written,sent,err,took);failure=5;goto finish;}
            if((unsigned)sent<n-written)++partial;
            written+=(unsigned)sent;
        }
        done+=n;if(pace)vTaskDelay(pdMS_TO_TICKS(pace));
    }
    send_ms=(unsigned)((esp_timer_get_time()-sending)/1000);
    uint8_t ack[8];unsigned received=0;int64_t waiting=esp_timer_get_time();
    while(received<sizeof(ack)) {
        int got=esp_transport_read(t,(char *)ack+received,(int)(sizeof(ack)-received),5000);
        if(got<=0){failure=6;goto finish;}received+=(unsigned)got;
    }
    ack_ms=(unsigned)((esp_timer_get_time()-waiting)/1000);
    uint32_t size;memcpy(&size,ack+4,4);pass=!memcmp(ack,"OKAY",4) && ntohl(size)==total;
    if(!pass)failure=7;
finish:
    printf("NETPROBE_TRANSFER phase=%s tls=%u total=%u chunk=%u pace_ms=%u sent=%u pass=%u failure=%d connect_ms=%u send_ms=%u ack_ms=%u slow_write_ms=%u partial=%u errno=%d heap=%u min_heap=%u\n",
        phase,(unsigned)tls,total,chunk,pace,done,(unsigned)pass,failure,connect_ms,send_ms,ack_ms,slow,partial,err,(unsigned)esp_get_free_heap_size(),(unsigned)esp_get_minimum_free_heap_size());
    free(buffer);esp_transport_close(t);esp_transport_destroy(t);return pass;
}
static void suite(const char *phase)
{
    for(unsigned tls=0;tls<2;++tls) {
        transfer(tls,55,55,0,phase);
        transfer(tls,16384,4096,0,phase);
        transfer(tls,16000,320,20,phase);
        transfer(tls,16384,8192,0,phase);
    }
}
static void cloud(const char *host,const char *address)
{
    esp_transport_handle_t t=esp_transport_ssl_init();if(!t)return;
    esp_transport_ssl_crt_bundle_attach(t,esp_crt_bundle_attach);
    if(address)esp_transport_ssl_set_common_name(t,host);
    int64_t start=esp_timer_get_time();
    int connected=esp_transport_connect(t,address?address:host,443,5000);
    unsigned connect_ms=(unsigned)((esp_timer_get_time()-start)/1000);int written=-1,got=-1,status=0;unsigned read_ms=0;
    if(connected==0) {
        char request[256],response[256];
        int n=snprintf(request,sizeof(request),"GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: Passport-Network-Probe/1\r\n\r\n",host);
        written=esp_transport_write(t,request,n,1000);start=esp_timer_get_time();
        if(written==n)got=esp_transport_read(t,response,sizeof(response)-1,5000);
        read_ms=(unsigned)((esp_timer_get_time()-start)/1000);
        if(got>12){response[got]=0;if(!strncmp(response,"HTTP/1.",7))status=atoi(response+9);}
    }
    printf("NETPROBE_WAN host=%s direct=%u connected=%d connect_ms=%u written=%d read_bytes=%d read_ms=%u http=%d\n",host,(unsigned)(address!=NULL),connected,connect_ms,written,got,read_ms,status);
    esp_transport_close(t);esp_transport_destroy(t);
}
static void internet(void)
{
    const char *hosts[]={"open.feishu.cn","api.tenclass.net"};
    for(unsigned h=0;h<2;++h) {
        struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*result=NULL;
        int64_t began=esp_timer_get_time();int rc=getaddrinfo(hosts[h],NULL,&hints,&result);char first[INET_ADDRSTRLEN]={0};
        printf("NETPROBE_RESOLVE host=%s result=%d ms=%u\n",hosts[h],rc,(unsigned)((esp_timer_get_time()-began)/1000));
        if(!rc) {
            unsigned count=0;
            for(struct addrinfo *p=result;p && count<4;p=p->ai_next) {
                char ip[INET_ADDRSTRLEN];struct sockaddr_in *a=(struct sockaddr_in *)p->ai_addr;
                if(inet_ntop(AF_INET,&a->sin_addr,ip,sizeof(ip))) {
                    printf("NETPROBE_ADDRESS host=%s ip=%s\n",hosts[h],ip);
                    if(!count)snprintf(first,sizeof(first),"%s",ip);
                }
                ++count;
            }
            freeaddrinfo(result);
        }
        cloud(hosts[h],NULL);if(first[0])cloud(hosts[h],first);
    }
}
static void worker(void *argument)
{
    (void)argument;
    printf("NETPROBE_BEGIN heap=%u\n",(unsigned)esp_get_free_heap_size());
    int rc=feishu_network_start(15000);printf("NETPROBE_CONNECT result=%d\n",rc);
    if(rc==ESP_OK) {
        link_info();scan();link_info();suite("roomy");
        void *ballast[48]={0};unsigned used=0;
        while(used<48 && esp_get_free_heap_size()>100000) {
            ballast[used]=malloc(4096);if(!ballast[used])break;++used;
        }
    printf("NETPROBE_BALLAST bytes=%u heap=%u largest=%u\n",used*4096,(unsigned)esp_get_free_heap_size(),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        suite("app_heap");internet();for(unsigned i=0;i<used;++i)free(ballast[i]);link_info();
    }
    printf("NETPROBE_DONE result=%d heap=%u\n",rc,(unsigned)esp_get_free_heap_size());
    vTaskDelete(NULL);
}
void feishu_network_probe_start(void)
{
    lv_obj_t *screen=lv_obj_create(NULL);lv_obj_set_style_bg_color(screen,lv_color_hex(FEISHU_BG),0);
    lv_obj_t *label=lv_label_create(screen);lv_obj_set_style_text_font(label,&lv_font_ai_passport_14,0);
    lv_obj_set_style_text_color(label,lv_color_hex(FEISHU_TEXT),0);
    lv_label_set_text(label,"网络诊断中\n请勿操作设备");lv_obj_center(label);lv_screen_load(screen);
    if(xTaskCreate(worker,"network_probe",12288,NULL,5,NULL)!=pdPASS)printf("NETPROBE_DONE result=nomem\n");
}
