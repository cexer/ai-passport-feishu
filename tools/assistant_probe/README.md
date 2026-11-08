[简体中文](README.zh_CN.md) · **English**

# Muted XiaoZhi assistant diagnostic

This opt-in build runs after normal Feishu initialization on an already bound
device. It uses real cloud MCP calls and the real Opus decoder. PCM fixture
overlay replaces microphone samples; output volume is always zero. It never
exposes a message-send tool. Do not ship the diagnostic as ordinary firmware.

Prepare six synthetic 16 kHz, mono, PCM16 WAV files with these single intentions:

| Input | Command intention |
| --- | --- |
| List chats | List the currently cached Feishu conversations. |
| Navigate | Open the device settings page. |
| Draft | Prepare a draft in the first conversation; do not send it. |
| Open chat | Open the first conversation and show its message list. |
| Read latest | Read the first conversation's latest message. |

```powershell
python tools/assistant_probe/make_fixture.py --list-chats list.wav --navigate navigate.wav --draft draft.wav --open-chat open.wav --read-latest read.wav --goodbye goodbye.wav
$env:FEISHU_VOICE_AUTOTEST='1'
$env:FEISHU_ASSISTANT_AUTOTEST='1'
idf.py -B build/assistant-probe -DFEISHU_ASR_BACKEND=xiaozhi "-DSDKCONFIG=$PWD/build/assistant-probe/sdkconfig" reconfigure build
```

Fixture output is confined to ignored `build/` paths. Existing ordinary voice
fixtures are preserved. On a fresh checkout, guarded aliases allow the common
injection driver to compile; they are not valid ordinary-ASR test fixtures.
Combined audio is limited to 960 KiB to leave application Flash headroom.

The suite runs conversation listing, navigation, draft, cancellation after 400 ms, injected
capture failure after 400 ms, latest-message reading to establish recovery, and opening a
conversation. Its controller must also observe the applied message-list load. It validates
actual tool results, not just the presence of a JSON request. The draft case
removes only the draft created in that turn after verifying its object and UUID;
existing drafts are preserved. A full draft store is a failed test, not permission
to delete somebody's drafts.

Compound commands require model planning and are evaluated separately. A model
reply that merely lists conversations is not evidence that it read a message or
saved a draft. The controller should verify the actual selected conversation as
well as the tool's completion.

Keep `ASSISTANT_RPC`, `ASSISTANT_TOOL_*`, `ASSISTANT_AUDIO`, `ASSISTANT_DONE`,
`ASSISTANT_SMOKE_DONE` and `ASSISTANT_SUITE_DONE` markers. Normal cases require
decoded audio frames; cancellation/failure cases require their expected error.
Negotiation, local replay, real remote calls, device PCM writes and human audible
quality are distinct evidence. The last still needs physical acceptance.

Use the repository's authorized device-test policy and exact target/layout
guards before flashing. Restore the qualified ordinary APP in a `finally` path;
preserve binding, identity and data. Before building a deliverable, unset both
diagnostic environment flags and use a separate build directory. The ordinary
APP must not contain `ASSISTANT_SUITE_DONE` or synthetic fixture audio.

Eighth case: one greeting and three turns on the same connection (list, read, goodbye); require silence submission, decoded output and farewell exit, with no business sends.

Use FEISHU_ASSISTANT_AUTOTEST=2 for only the continuous case. Reconfigure after changing diagnostic environment flags; an incremental Ninja build alone can retain the prior mode.
