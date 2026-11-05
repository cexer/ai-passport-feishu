<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Fonts

Store reusable font files and generated font sources here.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Bundled device font

`main/lv_font_ai_passport_14.c` is generated from Source Han Sans SC Normal
2.005. It uses 2-bit compressed grayscale glyphs at 14 px and covers ASCII, common
Chinese punctuation, full-width forms, and the complete U+4E00-U+9FFF CJK
Unified Ideographs block. The source font is licensed under SIL OFL 1.1; the
license text is stored in `SourceHanSans-OFL.txt`.

Regenerate it with `lv_font_conv` 1.5.3 using ranges
`0x20-0x7e,0x2000-0x206f,0x2713,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef`,
4 bpp, compression enabled, and kerning disabled. Enable
`CONFIG_LV_USE_FONT_COMPRESSED=y`. Keep the product metrics at
17 px line height and 4 px baseline after generation.

Use `tools/generate_feishu_font.py --converter <lv_font_conv.js> --font <2.005.otf>`
to reproduce the file. The source URL and SHA-256 are pinned in
`SourceHanSansSC-Normal-2.005.json`; the older managed LVGL font lacks the final
16 CJK characters and must not replace this input. The larger full-CJK bitmap uses
the original APP start with a `0x330000` capacity; identity and data offsets
remain unchanged. Compare actual BIN/MAP sizes rather than generated C length.

Full 4 bpp glyphs are generated as `feishu-14-4bpp.bin` (1,737,446 bytes), mapped from the read-only `feishu_font` partition at 0x500000. The generated C retains metrics and indexes; enable `CONFIG_LV_FONT_FMT_TXT_LARGE=y`. The boot path verifies the asset SHA-256 before starting Chinese UI. Reproduce with `tools/generate_feishu_font.py --converter <lv_font_conv.js> --font <pinned-2.005.otf> --bpp 4`; include both application and font image in any installation. The existing app, identity and shared-data addresses remain unchanged.
