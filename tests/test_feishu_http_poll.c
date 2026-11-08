#include "feishu_http_poll.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    const int sdk_again=0x7007;
    assert(feishu_http_headers_pending(-sdk_again,0,sdk_again));
    assert(feishu_http_headers_pending(-1,EAGAIN,sdk_again));
    assert(feishu_http_headers_pending(-1,EWOULDBLOCK,sdk_again));
    assert(!feishu_http_headers_pending(0,EAGAIN,sdk_again));
    assert(!feishu_http_headers_pending(1024,EAGAIN,sdk_again));
    assert(!feishu_http_headers_pending(-1,0,sdk_again));
    assert(!feishu_http_headers_pending(-1,ECONNRESET,sdk_again));
    assert(!feishu_http_headers_pending(-1,ENOMEM,sdk_again));
    puts("HTTP header polling: SDK/errno waits, ready response, close and fatal errors PASS");
}
