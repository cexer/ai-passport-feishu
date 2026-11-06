<p align="right"><a href="README.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Synthetic network diagnostic

This optional firmware isolates Wi-Fi/TCP/TLS from the Feishu worker and audio
capture. It sends bounded synthetic byte patterns to a local test receiver,
verifies its acknowledgements, compares normal and reduced free heap, and
records AP selection and DNS. Public-root HTTPS GETs check reachability without
application tokens. These checks do not establish ASR or business-API acceptance.

## Prepare and build

Use Python with `cryptography` installed. Substitute the receiver computer's LAN
IPv4 address and the device's existing SSID; no Wi-Fi password is needed here.

```text
python tools/network_probe/prepare.py --host <LAN-IPv4> --ssid <SSID>
python tools/network_probe/server.py --config build/network-probe/config.json
```

The default output is the ignored `build/network-probe` directory. Its temporary
CA, private test key and generated header must not be committed. TLS verification
remains enabled; the fixture CA is trusted only by this diagnostic image. Changing
the fixture requires rebuilding that image. No system trust store is modified.

In an activated ESP-IDF environment, set `FEISHU_NETWORK_DIAGNOSTIC=1` for the
diagnostic build only, then run `tools/validate.sh --firmware`. CMake reads the
generated header from the default output directory. Ordinary builds must leave
the flag unset or set it to `0`.

## Device and server lifetime

Use the normal authorized device-test workflow with a verified compatible layout.
Preserve settings and flash only the application image when required; do not
write a merged image merely to run this probe. App navigation is disabled during
the diagnostic. It neither records audio nor sends IM messages.

The device emits `NETPROBE_DONE` after sixteen LAN cases and the WAN checks.
Restore the verified ordinary application afterwards. The server stops after
600 seconds by default, or when `server.stop` appears in its output directory;
`--timeout` accepts 1–3600 seconds. Do not leave receivers or serial monitors
running after a test. A blocked LAN receiver is not evidence of a device TLS fault.

Logs contain AP/network metadata and synthetic-test results. Keep them local
unless needed for the investigation; never add credentials or real message data.
