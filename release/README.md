<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Firmware artifacts

Product: ZION Feishu Smart Client. Each version has its own directory:

- `VERSION/ZION-Feishu-VERSION-full.bin`: merged image, address `0x0`.
- `VERSION/ZION-Feishu-VERSION-app.bin`: APP-only update, address `0x10000`.
- `VERSION/manifest.json`: sizes, SHA256, flash addresses and matching source hashes.

Prepare artifacts from an already verified ordinary bundle:

```powershell
python tools/prepare_release.py build/ui-core-r80-product-verified
```

The command verifies the merged layout, matching sources, ordinary APP identity
and blank padding before copying. A conflicting BIN cannot overwrite the same
version. Keep ELF, MAP, logs and private device captures in the ignored build
directory. Binary files in this directory are explicitly tracked by request.

APP-only updates require the matching existing partition table and font. The
full image contains blank address gaps, including data storage; flashing it
may clear existing data. For an existing device, use the matching APP-only
update to preserve its data.

R80 passed the targeted power-policy device gate. The subsequent frozen-screen
report was traced to the test controller leaving the device in flasher download
mode after installing the ordinary APP: a no-reset handshake confirmed that
state. The LCD retained the last conversation frame; restart to run the APP.
Slow startup remains under investigation, and battery endurance was not measured.
