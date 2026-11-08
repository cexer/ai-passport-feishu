"""Generate narrowly scoped LVGL 9.5 allocation-recovery sources at configure time.

The managed dependency stays untouched. Fail closed if its source contract changes.
"""
from pathlib import Path
import sys


def patch(source: Path, output: Path) -> None:
    replacements = {
        "lv_draw_label.c": [
            ("if(dsc->_draw_buf) lv_draw_buf_destroy(dsc->_draw_buf);",
             "if(dsc->_draw_buf) lv_draw_buf_destroy(dsc->_draw_buf);\n"
             "                dsc->_draw_buf = NULL;"),
            ("draw_buf = lv_draw_buf_create_ex(font_draw_buf_handlers, g.box_w, h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);\n                LV_ASSERT_MALLOC(draw_buf);",
             "#ifdef FEISHU_LVGL_RECOVERY_AUTOTEST\n"
             "                extern bool feishu_lvgl_fail_next_glyph(void);\n"
             "                draw_buf = feishu_lvgl_fail_next_glyph() ? NULL :\n"
             "#else\n                draw_buf =\n#endif\n"
             "                    lv_draw_buf_create_ex(font_draw_buf_handlers, g.box_w, h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);\n"
             "                if(draw_buf == NULL) {\n"
             "                    LV_LOG_WARN(\"Glyph scratch unavailable; keep UI responsive\");\n"
             "                    goto exit;\n                }")],
        "lv_draw_buf.c": [
            ("lv_draw_buf_t * draw_buf = lv_malloc_zeroed(sizeof(lv_draw_buf_t));\n    LV_ASSERT_MALLOC(draw_buf);\n    if(draw_buf == NULL)",
             "lv_draw_buf_t * draw_buf = lv_malloc_zeroed(sizeof(lv_draw_buf_t));\n    if(draw_buf == NULL)")],
    }
    text = source.read_text(encoding="utf-8")
    for old, new in replacements[source.name]:
        if text.count(old) != 1:
            raise ValueError(f"Unsupported LVGL source contract: {source.name}")
        text = text.replace(old, new)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    patch(Path(sys.argv[1]), Path(sys.argv[2]))
