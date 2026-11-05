#include "feishu_qr_retry.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    feishu_qr_retry_t s;
    assert(sizeof(s)<=8);
    feishu_qr_retry_reset(&s);assert(feishu_qr_retry_due(&s,0));
    feishu_qr_retry_result(&s,0,false);
    assert(!feishu_qr_retry_due(&s,100) && !feishu_qr_retry_due(&s,999));
    assert(feishu_qr_retry_due(&s,1000));
    feishu_qr_retry_result(&s,1000,false);
    assert(!feishu_qr_retry_due(&s,1001) && feishu_qr_retry_due(&s,2000));
    /* A new URL/phase gets an immediate attempt, including during backoff. */
    feishu_qr_retry_reset(&s);assert(feishu_qr_retry_due(&s,1001));
    feishu_qr_retry_result(&s,1001,true);assert(!s.failed);
    /* Millisecond tick rollover must not lock out the onboarding screen. */
    feishu_qr_retry_result(&s,UINT32_MAX-499u,false);
    assert(!feishu_qr_retry_due(&s,499) && feishu_qr_retry_due(&s,500));
    puts("QR retry: failure throttled, new phase immediate, recovery and tick rollover PASS");
    return 0;
}
