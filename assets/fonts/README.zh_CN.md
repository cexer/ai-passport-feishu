<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 字库资源（Fonts）

本目录存放项目可复用的字库资源。每个字库子目录或单个字库文件，应附说明。

## 如何使用

- 字库文件（如 `.ttf`、`.otf`、LVGL 使用的 C 数组字库等）复制到本目录，并在本项目 `README.md` 记录字名、字号、支持字符集与版权信息。
- 若需集成到 ESP-IDF 固件，参考 [`components/bsp/include/bsp_display.h`](../../components/bsp/include/bsp_display.h) 与 LVGL 字体接口，将字库转换为对应格式并放入正确资源目录。
- 字库占用 Flash 与内存，需在集成前评估 ESP32-C3 无 PSRAM 的限制（详见 `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`）。

## 目录说明

## 设备内置字库

`main/lv_font_ai_passport_14.c` 由 Source Han Sans SC Normal 2.005 生成，
采用 14 px、2-bit 灰度压缩字模，覆盖 ASCII、常用中文标点、全角字符以及完整的
U+4E00-U+9FFF 中日韩统一表意文字区。源字库使用 SIL OFL 1.1 许可，许可全文
保存在 `SourceHanSans-OFL.txt`。

重新生成时使用 `lv_font_conv` 1.5.3，字符范围为
`0x20-0x7e,0x2000-0x206f,0x2713,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef`，
选择 4 bpp、保留压缩并关闭 kerning，启用 `CONFIG_LV_USE_FONT_COMPRESSED=y`。
生成后将产品界面的行高和基线分别保持为
17 px 与 4 px。

可用 `tools/generate_feishu_font.py --converter <lv_font_conv.js> --font <2.005.otf>`
复现。源 URL 与 SHA-256 固定在 `SourceHanSansSC-Normal-2.005.json`；托管的旧版
LVGL 源字体缺少最后 16 个汉字，不能替代此输入。完整中文字模增大后，APP 起点不变，容量改为 `0x330000`；
身份区与数据区地址不变。体积比较以实际 BIN/MAP 为准，不用生成 C 文件长度代替。

完整 4 bpp 字形生成到 `feishu-14-4bpp.bin`（1,737,446 字节），从 0x500000 的只读 `feishu_font` 分区映射。生成的 C 保留字形指标与索引，启用 `CONFIG_LV_FONT_FMT_TXT_LARGE=y`。启动前校验资源 SHA-256，再打开中文界面。可用 `tools/generate_feishu_font.py --converter <lv_font_conv.js> --font <固定版本2.005.otf> --bpp 4` 复现；安装时须包含应用及匹配字形资源。现有固件、身份区及共享数据地址保持不变。
