#include <assert.h>
#include <stdio.h>
#include <errno.h>
#define fd_set fake_fd_set
struct fake_fd_set {unsigned mask;};typedef struct fake_fd_set fake_fd_set;
#define FD_ZERO(s) ((s)->mask=0)
#define FD_SET(fd,s) ((s)->mask|=1u<<(fd))
struct timeval {long tv_sec,tv_usec;};
#include "feishu_tls_poll.h"
static unsigned calls,waits;static int failure;
static int fake_select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *tv)
{
 (void)tv;assert(n==4 && !e);++calls;
 /* Both inputs must be freshly armed on every step. */
 if(!(r->mask&8) || !(w->mask&8))return 0;
 r->mask=w->mask=0;
 if(failure){errno=failure;return -1;}
 if(waits){--waits;return 0;}w->mask=8;return 1;
}
int main(void){fd_set r={8},w={8};struct timeval tv={0};
 waits=1;assert(fake_select(4,&r,&w,NULL,&tv)==0);assert(fake_select(4,&r,&w,NULL,&tv)==0);
 waits=40;for(unsigned i=0;i<40;++i)assert(feishu_tls_poll_socket(3,&r,&w,&tv,fake_select)==0);
 assert(feishu_tls_poll_socket(3,&r,&w,&tv,fake_select)==1);
 failure=EINTR;assert(feishu_tls_poll_socket(3,&r,&w,&tv,fake_select)==0);
 failure=ECONNRESET;assert(feishu_tls_poll_socket(3,&r,&w,&tv,fake_select)==-1 && errno==ECONNRESET);
 failure=0;assert(feishu_tls_poll_socket(3,&r,&w,&tv,fake_select)==1);
 printf("TLS select: old cleared-set dead end reproduced, 40 timeouts, rearm/ready, EINTR, hard error and recovery PASS; polls=%u\n",calls);
}
