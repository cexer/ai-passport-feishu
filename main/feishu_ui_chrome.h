#pragma once

#include "feishu_theme.h"
#include "feishu_model.h"
#include "ui_pixel.h"
#include <stdio.h>
#include <string.h>

/* Shared 240x320 frame. Default business content starts at y=46; message rows retain y=38. */
static inline void feishu_ui_rule(lv_obj_t *screen,int y)
{
    lv_obj_t *rule=lv_obj_create(screen);
    lv_obj_remove_flag(rule,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(rule,0,y);lv_obj_set_size(rule,240,1);
    lv_obj_set_style_radius(rule,0,0);
    lv_obj_set_style_border_width(rule,0,0);lv_obj_set_style_pad_all(rule,0,0);
    lv_obj_set_style_bg_color(rule,lv_color_hex(FEISHU_RULE),0);
    lv_obj_set_style_bg_opa(rule,LV_OPA_COVER,0);
}

static inline void feishu_ui_header(lv_obj_t *screen,lv_obj_t **title,lv_obj_t **battery,lv_obj_t **assistant)
{
    *title=lv_label_create(screen);lv_label_set_text(*title,"");
    lv_obj_set_size(*title,96,18);lv_obj_set_pos(*title,12,10);
    lv_label_set_long_mode(*title,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(*title,&lv_font_ai_passport_14,0);
    lv_obj_set_style_text_color(*title,lv_color_hex(FEISHU_TEXT),0);
    lv_obj_t *box=lv_obj_create(screen);
    lv_obj_remove_flag(box,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box,180,7);lv_obj_set_size(box,48,22);
    lv_obj_set_style_pad_all(box,0,0);lv_obj_set_style_radius(box,5,0);
    lv_obj_set_style_bg_color(box,lv_color_hex(FEISHU_SURFACE_FOCUS),0);
    lv_obj_set_style_border_width(box,1,0);
    lv_obj_set_style_border_color(box,lv_color_hex(FEISHU_ACCENT),0);
    lv_obj_t *cap=lv_obj_create(screen);
    lv_obj_remove_flag(cap,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(cap,229,14);lv_obj_set_size(cap,3,8);
    lv_obj_set_style_border_width(cap,0,0);lv_obj_set_style_radius(cap,1,0);
    lv_obj_set_style_bg_color(cap,lv_color_hex(FEISHU_ACCENT),0);
    *battery=lv_label_create(box);lv_obj_set_width(*battery,44);
    lv_obj_set_style_text_font(*battery,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_align(*battery,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_color(*battery,lv_color_hex(FEISHU_MUTED),0);
    lv_label_set_text(*battery,"?");lv_obj_center(*battery);
    lv_obj_t *badge=lv_label_create(screen);lv_label_set_text(badge,"智");
    lv_obj_set_style_text_font(badge,&lv_font_ai_passport_14,0);
    lv_obj_set_style_text_color(badge,lv_color_hex(0x657782),0);
    lv_obj_set_pos(badge,158,10);if(assistant)*assistant=badge;
    feishu_ui_rule(screen,34);
}

static inline void feishu_ui_footer(lv_obj_t *screen,lv_obj_t **notice,lv_obj_t *actions[3])
{
    lv_obj_t *footer=lv_obj_create(screen);
    lv_obj_set_pos(footer,0,272);lv_obj_set_size(footer,240,48);
    lv_obj_remove_flag(footer,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(footer,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_color(footer,lv_color_hex(FEISHU_RULE),0);
    lv_obj_set_style_border_width(footer,1,0);
    lv_obj_set_style_border_side(footer,LV_BORDER_SIDE_TOP,0);
    lv_obj_set_style_radius(footer,0,0);lv_obj_set_style_pad_all(footer,0,0);
    *notice=lv_label_create(footer);
    for(unsigned i=0;i<3;++i)actions[i]=lv_label_create(footer);
    lv_obj_t *labels[]={*notice,actions[0],actions[1],actions[2]};
    for(unsigned i=0;i<4;++i) {
        lv_label_set_text(labels[i],"");
        lv_obj_set_pos(labels[i],13,i ? 26:4);lv_obj_set_size(labels[i],212,18);
        lv_obj_set_style_text_font(labels[i],&lv_font_ai_passport_14,0);
        lv_obj_set_style_text_align(labels[i],LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_style_text_color(labels[i],lv_color_hex(FEISHU_MUTED),0);
        lv_label_set_long_mode(labels[i],LV_LABEL_LONG_CLIP);
    }
}

static inline void feishu_ui_actions(lv_obj_t *groups[3],const char *formatted)
{
    char text[128];feishu_utf8_copy(text,sizeof(text),formatted);
    char *cursor=text;int widths[3]={0},count=0,total=0;
    while(cursor && count<3) {
        char *next=strstr(cursor,"  ");if(!next)next=strstr(cursor," / ");
        char *rest=next ? next+(*next==' ' && next[1]=='/' ? 3:2):NULL;
        if(next)*next=0;
        lv_label_set_recolor(groups[count],false);
        if(strcmp(lv_label_get_text(groups[count]),cursor))lv_label_set_text(groups[count],cursor);
        lv_obj_set_style_text_letter_space(groups[count],0,0);
        lv_obj_set_width(groups[count],LV_SIZE_CONTENT);lv_obj_update_layout(groups[count]);
        widths[count]=lv_obj_get_width(groups[count]);total+=widths[count];++count;cursor=rest;
    }
    if(total+(count-1)*12>218) {
        total=0;
        for(int i=0;i<count;++i) {
            lv_obj_set_style_text_letter_space(groups[i],-1,0);lv_obj_update_layout(groups[i]);
            widths[i]=lv_obj_get_width(groups[i]);total+=widths[i];
        }
    }
    int gap=count>1 ? (218-total)/(count-1):0;if(gap>18)gap=18;
    int x=10+(218-total-gap*(count-1))/2;
    for(int i=0;i<3;++i) {
        if(i>=count){lv_label_set_text(groups[i],"");continue;}
        lv_obj_set_pos(groups[i],x,26);x+=widths[i]+gap;
    }
}

static inline void feishu_ui_battery(lv_obj_t *label,int percent)
{
    char value[8];
    if(percent<0)snprintf(value,sizeof(value),"?");
    else snprintf(value,sizeof(value),"%u%%",(unsigned)(percent>100 ? 100:percent));
    if(strcmp(lv_label_get_text(label),value))lv_label_set_text(label,value);
}
