#include "http_tx_lifetime.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static unsigned allocated,freed;
static void *make(size_t n){assert(n==64);++allocated;return malloc(n);}
static void *oom(size_t n){(void)n;return NULL;}
static void drop(void *p){for(unsigned i=0;i<64;++i)assert(((unsigned char *)p)[i]==0);++freed;free(p);}
int main(void)
{
    char *p=NULL;
    assert(esp_http_tx_acquire(&p,64,make));memset(p,0xa5,64);
    esp_http_tx_release(&p,64,drop);assert(!p && freed==1); /* opt-in before TLS */
    esp_http_tx_release(&p,64,drop);assert(freed==1);
    assert(esp_http_tx_acquire(&p,64,make));memset(p,0x5a,64); /* headers after TLS */
    assert(esp_http_tx_acquire(&p,64,make) && allocated==2);
    assert(!esp_http_tx_finish(true,true,1,0,&p,64,drop) && p[0]==0x5a); /* partial header */
    assert(!esp_http_tx_finish(true,true,0,1,&p,64,drop) && p); /* GET body */
    assert(!esp_http_tx_finish(true,false,0,0,&p,64,drop) && p); /* POST */
    assert(!esp_http_tx_finish(false,true,0,0,&p,64,drop) && p); /* no opt-in */
    assert(esp_http_tx_finish(true,true,0,0,&p,64,drop) && !p && freed==2);
    assert(esp_http_tx_acquire(&p,64,make));memset(p,0xa5,64);
    esp_http_tx_release(&p,64,drop);assert(allocated==freed); /* cancel/reuse */
    assert(!esp_http_tx_acquire(&p,64,oom) && !p);
    esp_http_tx_release(&p,64,drop);assert(allocated==freed);
    assert(!esp_http_tx_acquire(&p,0,make));
    puts("HTTP TX lifetime: before/after TLS, partial headers, POST/body isolation, wipe, cancel/reuse and OOM PASS");
}
