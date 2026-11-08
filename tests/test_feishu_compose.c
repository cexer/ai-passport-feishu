#include "feishu_compose.h"
#include "feishu_outbox.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    feishu_compose_snapshot_t current={.text="Meeting at three",.chat="oc_actual",.message="om_reply",.uuid="draft-one",.length=16,.state=FEISHU_OUTBOX_READY};
    feishu_compose_snapshot_t next={0},original=current;
    char rev[FEISHU_COMPOSE_REV_CAP],changed[FEISHU_COMPOSE_REV_CAP];
    assert(feishu_compose_revision(&current,rev));
    /* A discussion leaves the draft and revision unchanged. Explicit editing
     * cannot accept a stale revision or alter the destination/reply thread. */
    assert(feishu_compose_replace(&current,rev,"Meeting tomorrow at three",&next));
    assert(!strcmp(next.chat,current.chat) && !strcmp(next.message,current.message));
    assert(!memcmp(&current,&original,sizeof(current)));
    assert(feishu_compose_revision(&next,changed) && strcmp(rev,changed));
    assert(!feishu_compose_replace(&next,rev,"stale write",&current));
    assert(feishu_compose_undo(&next,changed,&original,&current));
    assert(!strcmp(current.text,original.text));
    original.chat[3]='x';assert(!feishu_compose_undo(&next,changed,&original,&current));
    current.offset=1;assert(!feishu_compose_revision(&current,rev));current.offset=0;
    current.state=FEISHU_OUTBOX_UNCERTAIN;assert(!feishu_compose_revision(&current,rev));
    current.state=FEISHU_OUTBOX_READY;current.length=10000;assert(!feishu_compose_revision(&current,rev));
    current=next;assert(feishu_compose_revision(&current,rev));
    char oversized[513];memset(oversized,'x',512);oversized[512]=0;
    assert(!feishu_compose_replace(&current,rev,oversized,&next));
    assert(!feishu_compose_replace(&current,rev,"\xc0\xaf",&next));
    assert(!feishu_compose_replace(&current,rev,"\xed\xa0\x80",&next));
    assert(feishu_compose_replace(&current,rev,"",&next));
    assert(next.length==0 && feishu_compose_editable(&next));
    assert(feishu_compose_revision(&next,changed));
    assert(feishu_compose_replace(&next,changed,"Start from an empty editor",&next));
    assert(!strcmp(next.chat,current.chat));
    /* Repeated edits stay bounded; only caller-owned current/previous records
     * exist, not an ever-growing list of saved drafts. */
    for(unsigned i=0;i<10000;++i){
        assert(feishu_compose_revision(&current,rev));
        assert(feishu_compose_replace(&current,rev,i%2 ? "first":"second",&next));current=next;
    }
    puts("compose recipient/stale-edit/undo/UTF-8/pressure: PASS");
}
