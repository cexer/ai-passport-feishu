#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FEISHU_TEXT_PAGE_STEP 256u
#define FEISHU_TEXT_PAGE_CAP (FEISHU_TEXT_PAGE_STEP+4u)

/* raw starts at max(page*STEP-3,0), and includes the byte at the next boundary
 * unless it is EOF. Both cuts round down to the same UTF-8 character boundary. */
bool feishu_text_page(const uint8_t *raw,size_t size,uint64_t offset,uint64_t total,
                       uint32_t page,char output[FEISHU_TEXT_PAGE_CAP],size_t *length);
