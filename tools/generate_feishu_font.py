"""Reproduce the full-CJK 14 px grayscale font with lv_font_conv 1.5.3."""
import argparse
from pathlib import Path
import re
import subprocess
import hashlib
import json

RANGES = '0x20-0x7e,0x2000-0x206f,0x2713,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--converter',required=True,help='Path to lv_font_conv.js version 1.5.3')
    p.add_argument('--font',required=True,help='SourceHanSansSC-Normal.otf 2.005, matching assets/fonts source manifest')
    p.add_argument('--output',default='main/lv_font_ai_passport_14.c')
    p.add_argument("--bpp",type=int,choices=[2,4],default=4)
    p.add_argument("--bitmap",default="assets/fonts/feishu-14-4bpp.bin")
    args=p.parse_args()
    manifest=json.loads(Path('assets/fonts/SourceHanSansSC-Normal-2.005.json').read_text(encoding='utf8'))
    if hashlib.sha256(Path(args.font).read_bytes()).hexdigest()!=manifest['sha256']:
        raise SystemExit('Source font does not match pinned Source Han Sans SC Normal 2.005')
    version=subprocess.check_output(['node',args.converter,'--version'],text=True).strip()
    if version!='1.5.3':raise SystemExit('Expected lv_font_conv 1.5.3, got '+version)
    subprocess.run(['node',args.converter,'--size','14','--bpp',str(args.bpp),'--format','lvgl',
        '--font',args.font,'-r',RANGES,'--no-kerning','--lv-font-name','lv_font_ai_passport_14',
        '--lv-include','lvgl.h','-o',args.output],check=True)
    out=Path(args.output);text=out.read_text(encoding='utf8')
    text=re.sub(r'\.line_height = \d+,.*', '.line_height = 17,          /*Product line height*/',text)
    text=re.sub(r'\.base_line = \d+,.*', '.base_line = 4,             /*Product baseline*/',text)
    text=re.sub(r'/\*\*?\*.*?\*/','/* Source Han Sans SC Normal, 14px, 2 bpp compressed; SIL OFL 1.1. Full original CJK ranges; lv_font_conv 1.5.3. */',text,count=1,flags=re.S)
    if f'.bpp = {args.bpp}' not in text or '.bitmap_format = 1' not in text:raise SystemExit('Unexpected font encoding')
    if args.bpp==4:
        text=externalize(text,Path(args.bitmap))
    out.write_text(text,encoding='utf8')

def externalize(text,bitmap):
    match=re.search(r'static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap\[\]\s*=\s*\{(.*?)\};',text,re.S)
    if not match:raise SystemExit('Missing bitmap array')
    raw=bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]+)',re.sub(r'/\*.*?\*/','',match[1],flags=re.S)))
    bitmap.parent.mkdir(parents=True,exist_ok=True);bitmap.write_bytes(raw)
    digest=hashlib.sha256(raw).digest()
    text=text[:match.start()]+text[match.end():]
    text=text.replace('static const lv_font_fmt_txt_dsc_t font_dsc','static lv_font_fmt_txt_dsc_t font_dsc').replace('.glyph_bitmap = glyph_bitmap,','.glyph_bitmap = NULL,')
    text=text.replace('14px, 2 bpp','14px, 4 bpp')
    init = '''
/* Immutable glyphs are mapped from unused Flash, never copied into heap.
 * Failure is detected before opening any Chinese UI or changing stored data. */
#include "esp_partition.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include <string.h>
esp_err_t feishu_font_init(void)
{
    static esp_partition_mmap_handle_t handle;
    if(font_dsc.glyph_bitmap)return ESP_OK;
    const esp_partition_t *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,0x40,"feishu_font");
    if(!part || part->size<FONT_BYTES)return ESP_ERR_NOT_FOUND;
    const void *mapped=NULL;
    esp_err_t err=esp_partition_mmap(part,0,FONT_BYTES,ESP_PARTITION_MMAP_DATA,&mapped,&handle);
    if(err!=ESP_OK)return err;
    static const unsigned char expected[32]={FONT_HASH};
    unsigned char actual[32];
    if(mbedtls_sha256(mapped,FONT_BYTES,actual,0) || memcmp(expected,actual,32)) {
        esp_partition_munmap(handle);return ESP_ERR_INVALID_CRC;
    }
    font_dsc.glyph_bitmap=mapped;
    ESP_LOGI("feishu_font","4bpp mapped bytes=%u hash_verified=1",(unsigned)FONT_BYTES);
    return ESP_OK;
}
'''
    init=init.replace('FONT_BYTES',str(len(raw))).replace('FONT_HASH',','.join('0x%02x'%v for v in digest))
    at=text.rfind('#endif /*#if LV_FONT_AI_PASSPORT_14*/')
    return text[:at]+init+text[at:]

if __name__=='__main__':main()
