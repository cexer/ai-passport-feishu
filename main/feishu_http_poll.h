#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
/* IDF perform() recognizes both fetch_headers retry forms. A ready response
 * must never be retried just because errno retained an older EAGAIN. */
static inline bool feishu_http_headers_pending(int64_t fetched,int socket_errno,int sdk_again)
{
    return fetched==-(int64_t)sdk_again ||
        (fetched<0 && (socket_errno==EAGAIN || socket_errno==EWOULDBLOCK));
}
