# Read-only device MCP prototype

[简体中文](README.zh_CN.md)

Explicit `FEISHU_MCP_DIAGNOSTIC=1` with the XiaoZhi backend enables this diagnostic,
including its host protocol tests. It does not enable MCP in the ordinary ASR
product. Standalone heap tracing must be disabled for this resource comparison.

The device connects to its own configured service, negotiates MCP, and advertises
one read-only aggregate-state tool. It locally replays a status call, then reads
one real Feishu message while keeping WSS open and checks a ping/pong afterward.
The single-message page is deliberate: it does not qualify an eight-message
workspace or prove a cloud-initiated semantic tool call. Audio, message sending,
credential retrieval and setting changes are absent from the tool interface.

Protocol bounds: 4,096 input bytes, depth ten, 128 structural/scalar/key events,
1,024 output bytes, bounded ASCII request IDs and session strings. Validate UTF-8,
shape, duplicate keys, parameters and session before using the small cJSON tree;
free protocol workspaces before opening Feishu HTTPS. Errors and allocation
failure must permit the next request. No caller may access LVGL from this module.

Restore the verified ordinary APP after the bounded test. Preserve the partition
table, identity, matching font, existing pool and drafts. Keep all raw connection
configuration and tokens out of reports.
