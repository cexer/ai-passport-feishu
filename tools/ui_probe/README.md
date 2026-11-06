# Device UI and network smoke probe

[简体中文](README.zh_CN.md)

The installed ordinary variant selects `-DFEISHU_ASR_BACKEND=xiaozhi`.
The source default remains the Feishu backend pending physical acceptance;
selecting a backend alone is not validation.

For the diagnostic build, set both `FEISHU_UI_AUTOTEST=1` and
`FEISHU_NETWORK_UI_AUTOTEST=1` in the build environment, then build with the
XiaoZhi backend. Keep these flags unset for the ordinary APP. Build and flashing
are separate steps; preserve the partition layout, identity, matching font,
binding and shared data pool.

The LVGL-thread probe checks pairing refresh/cooldown, draft colors, keyboard
layout, default Add focus, per-network actions and focus redraw, then actual
scan-to-password navigation. A worker probe checks cancellation, editing while
retaining the active profile, rollback after a wrong saved password, deliberate
disconnect/reconnect, a wrong candidate, scanning and real conversation reads.
With exactly one saved network, it temporarily forgets that profile and restores
it from RAM before reconnecting. Do not interrupt power during that test.

Success requires both `PAIR_UI_SMOKE_DONE pass=1` and
`NETWORK_UI_SMOKE_DONE pass=1`, followed by restoration and boot verification of
the ordinary APP. Failure is not a pass. These probes never send messages,
unpair the device or play audio, and never print SSIDs, passwords or tokens.
They do not replace physical-key, microphone, phone-provisioning or power-loss
acceptance. Ordinary firmware has neither probe enabled.
