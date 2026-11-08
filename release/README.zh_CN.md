<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 固件产物

产品：ZION 飞书智能客户端。每个版本独立存放：

- `VERSION/ZION-Feishu-VERSION-full.bin`：完整合并包，地址 `0x0`。
- `VERSION/ZION-Feishu-VERSION-app.bin`：仅APP升级包，地址 `0x10000`。
- `VERSION/manifest.json`：大小、SHA256、刷写地址和对应源码哈希。

从已经验证的正式构建目录归档：

```powershell
python tools/prepare_release.py build/ui-core-r80-product-verified
```

命令先检查合并布局、源码匹配、正式APP标识和空白填充，再复制产物。同一版本不允许被内容不同的BIN覆盖。ELF、MAP、日志和设备私有采集保留在已忽略的build目录；本目录中的BIN按用户要求纳入版本管理。

仅APP升级要求设备已有分区表和字体与版本匹配。完整包包含空白地址间隙，其中涉及数据存储；整包刷写可能清空既有数据。已有设备需要保留数据时，使用匹配的仅APP升级包。

R80已通过省电策略的针对性真机验收。之后报告的屏幕卡住，已通过不复位的下载协议握手确认：测试控制器刷入正式APP后停在刷写下载模式，LCD保留最后的会话画面；重启后才能运行APP。启动偏慢仍在排查，尚未测量续航。
