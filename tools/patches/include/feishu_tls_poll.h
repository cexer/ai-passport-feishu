#pragma once
#include <errno.h>
#ifndef FEISHU_TLS_POLL_TRACE
#define FEISHU_TLS_POLL_TRACE(text) ((void)0)
#endif
/* select mutates fd_sets even on timeout. Re-arm on EVERY async step. */
static inline int feishu_tls_poll_socket(int fd,fd_set *read_set,fd_set *write_set,
    struct timeval *wait,int (*poll)(int,fd_set *,fd_set *,fd_set *,struct timeval *))
{
    FD_ZERO(read_set);FD_SET(fd,read_set);*write_set=*read_set;
#ifdef FEISHU_TLS_POLL_AUTOTEST
    static unsigned injection;
    if(!injection){injection=1;FD_ZERO(read_set);FD_ZERO(write_set);
        FEISHU_TLS_POLL_TRACE("TLS_POLL_INJECT cleared=1");return 0;}
#endif
    int ready=poll(fd+1,read_set,write_set,NULL,wait);
    if(ready<0 && errno==EINTR)return 0;
#ifdef FEISHU_TLS_POLL_AUTOTEST
    if(injection==1 && ready>0){injection=2;FEISHU_TLS_POLL_TRACE("TLS_POLL_RECOVER rearmed=1");}
#endif
    return ready;
}
