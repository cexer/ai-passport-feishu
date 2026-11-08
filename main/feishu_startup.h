#pragma once
#include "lvgl.h"
/* Shared fullscreen startup artwork. Existing LVGL timer owns all calls. */
static inline void feishu_startup_label(lv_obj_t *label,uint32_t tick)
{
    static const char *frames[]={
            "  +        \n .-------. \n |\\     /| \n | \\   / | \n |  \\_/  | \n |_______| \n           \n  FEISHU   ",
            "        +  \n .-------. \n |\\     /| \n | \\   / | \n |  \\_/  | \n |_______| \n           \n  FEISHU   ",
            "           \n .-------. \n |\\     /| \n | \\   / | \n |  \\_/  | \n |_______| \n        +  \n  FEISHU   ",
            "           \n .-------. \n |\\     /| \n | \\   / | \n |  \\_/  | \n |_______| \n  +        \n  FEISHU   "
        };
    lv_obj_set_width(label,240);
    lv_obj_set_style_text_font(label,&lv_font_unscii_16,0);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text_static(label,frames[(tick/200u)%4u]);
    lv_obj_set_style_text_color(label,lv_color_hex(0x70D6A1),0);
    unsigned pulse=tick%1200u;pulse=pulse<600u ? pulse:1200u-pulse;
    lv_obj_set_style_opa(label,(lv_opa_t)(190u+pulse*65u/600u),0);
    lv_obj_center(label);
}
