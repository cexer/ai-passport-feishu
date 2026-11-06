#pragma once
#include <stddef.h>
#include <string.h>
/* Copyright (c) 2026 FoloToy, MIT; adapted from passport-os
 * main/launcher/wifi_keyboard.h (local reference, read-only).
 * Adapted interaction: bounded ASCII pages,
 * explicit multi-byte action tokens, so punctuation cannot become an action. */
static const char *const feishu_wifi_keys[4][35]={
    {"a","b","c","d","e","f","g","h","i","j","k","l","m","n","o","p","q","r","s","t","u","v","w","x","y","z"," ","ABC","123","DEL","GO"},
    {"A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"," ","abc","123","DEL","GO"},
    {"0","1","2","3","4","5","6","7","8","9","-","_",".","@","#","$","%","&","*","!","+","=","?","/",":",";","(",")","[","]"," ","abc","#+=","DEL","GO"},
    {"`","~","\\","|","{","}","^","\"","'","<",">",","," ","abc","123","DEL","GO"}
};
static inline size_t feishu_wifi_key_count(unsigned page)
{if(page>3)return 0;size_t n=0;while(n<35 && feishu_wifi_keys[page][n])++n;return n;}
static inline int feishu_wifi_password_valid(const char *password)
{
    if(!password)return 0;
    size_t n=strlen(password);
    if(!n)return 1;
    if(n<8 || n>64)return 0;
    if(n<64)return 1;
    for(size_t i=0;i<n;++i)if(!((password[i]>='0' && password[i]<='9') || (password[i]>='a' && password[i]<='f') || (password[i]>='A' && password[i]<='F')))return 0;
    return 1;
}
/* 1: connect; 0: edited; -1: full. No logs are emitted. */
static inline int feishu_wifi_key_apply(unsigned *page,size_t *selection,char password[65])
{
    size_t count=feishu_wifi_key_count(*page);if(*selection>=count)return -1;
    const char *key=feishu_wifi_keys[*page][*selection];size_t n=strlen(password);
    if(!strcmp(key,"GO"))return 1;
    if(!strcmp(key,"DEL")){if(n)password[n-1]=0;return 0;}
    if(!strcmp(key,"abc"))*page=0;
    else if(!strcmp(key,"ABC"))*page=1;
    else if(!strcmp(key,"123"))*page=2;
    else if(!strcmp(key,"#+="))*page=3;
    else {if(n>=64)return -1;password[n]=key[0];password[n+1]=0;return 0;}
    *selection=0;return 0;
}
