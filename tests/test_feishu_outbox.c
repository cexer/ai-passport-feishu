#include "feishu_outbox.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    feishu_outbox_t p;char first[41],retry[41];
    assert(feishu_outbox_create(&p,1,300*1024,"draft_fake","oc_fake","om_fake","00000000-00000000-00000000-00000000"));
    assert(feishu_outbox_uuid(&p,first) && strlen(first)==40);
    assert(feishu_outbox_attempt(&p,false));
    /* A process interruption leaves SENDING persisted; it cannot auto-retry. */
    feishu_outbox_t reopened=p;
    assert(!feishu_outbox_attempt(&reopened,false));
    feishu_outbox_failed(&p,false);assert(p.state==FEISHU_OUTBOX_UNCERTAIN && !p.offset);
    assert(!feishu_outbox_attempt(&p,false));
    assert(feishu_outbox_attempt(&p,true));assert(feishu_outbox_uuid(&p,retry) && !strcmp(first,retry));
    feishu_outbox_failed(&p,true);assert(p.state==FEISHU_OUTBOX_READY);
    assert(feishu_outbox_attempt(&p,false));assert(feishu_outbox_ack(&p,128*1024));
    assert(p.offset==128*1024 && p.state==FEISHU_OUTBOX_READY);
    assert(feishu_outbox_uuid(&p,retry) && strcmp(first,retry));
    assert(feishu_outbox_attempt(&p,false));assert(!feishu_outbox_ack(&p,300*1024));
    assert(feishu_outbox_ack(&p,172*1024) && p.state==FEISHU_OUTBOX_EMPTY);
    assert(!feishu_outbox_attempt(&p,true));
    assert(!feishu_outbox_create(&p,2,300*1024+1,"draft","oc_chat","","uuid"));
    assert(!feishu_outbox_create(&p,2,100,"bad/namespace","oc_chat","","uuid"));
    assert(feishu_outbox_create(&p,2,100,"draft","oc_chat","","uuid"));
    p.offset=101;assert(!feishu_outbox_valid(&p));
    puts("outbox: durable attempt, explicit uncertain retry, stable per-part UUID and progress PASS");
    return 0;
}
