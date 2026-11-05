<p align="right">
  <a href="feishu-messenger.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Feishu Messenger Product and Technical Specification

This document is the implementation baseline for the ESP32-C3 AI Passport Feishu messenger. The device calls Feishu OpenAPI directly and requires no phone, computer, or cloud bridge at runtime. It does not run the `lark-cli` executable; firmware implements the equivalent authentication and messaging APIs required by this product.

## 1. Product scope

This is a personal Feishu terminal, explicitly reconfirmed by the user. It reads and sends with the authorized user identity, so outgoing messages show that user as sender. Names such as neo identify the authorizing application; selecting or creating an application does not switch the sender to a bot. Bot-identity sending is a separate mode and is not enabled here.

The first accepted release provides only four core capabilities:

1. Show recent conversations and a dot when a message arrived after this device last viewed that conversation.
2. Show recent messages in the selected conversation.
3. Select a specific message as the reply target.
4. Record speech, transcribe it, and reply to that message after confirmation.

The first release does not render images, play voice messages, expose thread details, edit sent messages, or promise the same exact unread count as the Feishu client. Image, file, audio, and other message types use textual placeholders.

## 2. First use and every boot

The device does not show the development demo menu or require the user to find `Setup`. Startup routing is fixed:

```text
boot
 ├─ no usable Wi-Fi → AP provisioning page
 ├─ Wi-Fi available but no Feishu account → Feishu binding QR
 └─ Wi-Fi and Feishu account available → conversation list
```

### 2.1 AP provisioning

- The device advertises `FoloPassport-XXXX`. The screen prompts the user to join the open hotspot and shows its SSID and the `192.168.4.1` fallback address; Wi-Fi provisioning requires no QR scan.
- The phone joins the hotspot and opens a Chinese setup page to select 2.4 GHz Wi-Fi and enter its password.
- After submission, the device keeps the AP available while attempting the station connection. Success closes the AP and advances to Feishu binding. Failure keeps the setup page available with a specific reason and permits another submission.
- If saved Wi-Fi cannot connect within 15 seconds at boot, provisioning starts automatically without requiring a restart.
- Wi-Fi passwords never appear on screen or in logs.

### 2.2 Feishu account binding

- Generic firmware contains no Feishu App ID/Secret. An advanced owner writes their own developer application's credentials locally over physical USB or Web Serial before authorization.
- The device then displays a device-authorization QR for that owner-provided application. The intended user must be included in that application's availability and grant the requested permissions; tenant administrators can still restrict application access or permissions.
- The screen shows the device authorization QR and remaining lifetime. If it is not scanned, expires, or a transient service error occurs, the device replaces it automatically instead of entering a permanent failure page.
- Once the device has safely stored both access and refresh tokens, it shows binding success and automatically opens conversations. A failure to load profile or conversations must not turn a successful binding into a binding failure.
- After restart, cached tokens are preferred. Rebinding is required only after Feishu explicitly rejects the user token and also rejects refresh. DNS, TLS, timeout, or offline errors must not clear binding.

## 3. Pages, navigation, and copy

The normal path has four pages. Loading, offline, empty, and failure states render within the current page instead of creating more intermediate pages.

| Page | Primary content | `UP` / `DOWN` | `OK` click | Hold `OK` |
| --- | --- | --- | --- | --- |
| Conversations | name, latest summary, new-message dot | move selection | open conversation | open settings |
| Messages | sender, time, summary; selected row expands to three lines | move selection | reply to selected message and start recording | back to conversations |
| Recording | reply target, duration, level indicator | none | stop and transcribe | cancel to messages |
| Review | complete recognized text | `UP` record again, `DOWN` cancel | send reply | cancel to messages |

Settings is a low-frequency maintenance entry and is not part of the normal path. It contains only re-provision Wi-Fi and unbind Feishu; unbinding requires a second confirmation.

The visual design uses one flat list and whitespace, not grids of cards. All user-facing copy uses the bundled CJK font. Conversation names, sender names, and messages are truncated only at UTF-8 boundaries, never as missing glyph boxes or partial characters. A fixed footer shows available keys in Chinese without abbreviations.

## 4. Conversation, message, and unread rules

- A user token lists visible P2P and group chats, retrieves recent messages, and replies to the selected `message_id` as the user.
- The first sync after binding records each conversation's current newest message as its baseline and does not mark history as new.
- The device polls every 30 seconds. A new incoming message newer than the local read baseline adds a dot. The first release guarantees only new/not-new, not an exact count.
- The read baseline advances only after a conversation opens and its messages render successfully. Merely selecting a conversation does not clear the dot.
- Keep the latest 8 conversations and 12 messages. Preserve selection across refreshes when possible; otherwise select the nearest remaining row.
- Empty conversations show `No messages`. A failed refresh preserves cached content and shows an inline retry state instead of clearing the list.

Replies use a stable idempotency ID. Definite failures may be retried. If a request times out with an unknown result, refresh messages before allowing another send; never retry automatically and risk a duplicate.

## 5. Speech-to-text reply

- Capture 16 kHz, 16-bit mono PCM and stream it in small blocks. Never allocate a whole-recording buffer.
- Obtain the tenant token lazily when recording starts; it must not block boot or the conversation list.
- `OK` stops recording. Silence timeout or the 30-second cap also stops it. Empty recognition returns to recording with a clear retry prompt.
- Successful recognition opens the single review page. Send, re-record, and cancel all happen there; there is no separate actions page.
- ASR failure preserves the reply target and allows retry. Cancel closes audio and network streams without leaving a worker task behind.

## 6. Software states and recovery

Startup and interaction use explicit state machines:

```text
BOOT → WIFI_CHECK → AP_PROVISION → WIFI_CONNECT
                    ↓ success
       APP_CHECK → USB_PROVISION → QR_BIND → INITIAL_SYNC → CHATS

CHATS ⇄ MESSAGES → RECORDING → TRANSCRIBING → REVIEW → SEND → MESSAGES
```

All network requests use one authentication-aware API client:

- HTTP 2xx and successful business code: commit the result.
- Definite user-token expiry: refresh once, then replay the original request once.
- Explicit refresh-token rejection: preserve Wi-Fi and enter QR binding.
- DNS, TLS, offline, or server 5xx: back off while preserving page, cache, and binding.
- QR authorization pending is normal, not an error; expiry generates a new QR automatically.
- Out of memory releases request resources and returns to an operable page; it never erases NVS or enters a reboot loop.

Logs contain only state, HTTP status, Feishu business error code, free heap, and largest contiguous heap. They must not contain QR parameters, App Secret, access/refresh tokens, Wi-Fi passwords, or message bodies.

## 7. Authentication, storage, and memory boundaries

- Firmware and release images contain no Feishu App ID, App Secret, or user token. After Wi-Fi setup, onboarding offers a link/QR to select or create the user's application, followed by user authorization. Physical USB import remains an advanced fallback.
- The versioned USB protocol is accepted only while onboarding has no application credentials. Importing another application clears access and refresh tokens from the previous application. App Secret and tokens must never appear in logs or a generated firmware image.
- Access tokens can exceed 7 KiB. TLS completes its handshake before firmware writes the long `Authorization` header in chunks, preventing simultaneous copies of the token and large HTTP/TLS buffers.
- User tokens serve chat, message, and reply APIs. Internal ASR uses a separate owner-authorized web session; the legacy public-ASR tenant token is not fetched by the active recorder.
- JSON responses stream into bounded storage and are parsed after TLS/HTTP objects are released. UI models have fixed capacities and UTF-8-safe truncation.
- ESP32-C3 has no PSRAM. Even under the heaviest TLS plus Wi-Fi path, the target is at least 20 KiB free heap and a 12 KiB largest contiguous block. Log and validate both around every critical request.
- Production firmware must enable Flash Encryption and NVS Encryption. Development firmware must still avoid serial disclosure of secrets.

## 8. Implementation order and acceptance gates

Implementation proceeds in this order, and firmware is flashed only after the previous layer passes:

1. Pure C state machines: startup routing, forward/back navigation, QR renewal, auth error classification, first-sync unread baseline, and send idempotency.
2. API/parser fixtures: long tokens, pagination, empty chats, non-text messages, 401 refresh, 5xx, and truncated JSON.
3. Static checks and a complete firmware build.
4. One end-to-end device acceptance run.

The device matrix covers: factory-fresh AP provisioning, incorrect Wi-Fi retry, automatic QR renewal without scanning, automatic advance after scanning, power-cycle without another scan, offline recovery, rebind after token revocation, no false initial unread state, new-message dot, voice reply to a specific message, empty recognition/re-record/cancel, exactly-once send behavior, and 20 minutes of repeated use without reboot or sustained heap loss.

Delivery reports Build, Host tests, Device tests, and remaining unverified items separately. A successful compile is not hardware acceptance.


## Internal ASR integration (2026-11-04)

The alternative uses the owner's separately authorized web session with internal gateway command 1305. The peer package hash is `a39518a189fd0b4dea39326383b804a4a52b878a1382e7ecaef17e24cccc2e73`; no peer cookies were transferred. Independent QR login succeeded. Host credentials remain DPAPI encrypted and are never built into firmware or logged. QR creation, independent decoding, preflight validation and delivery run atomically, avoiding expiry during model/tool delays.

The C request builder and streaming response parser were tested directly against the live gateway with both synthetic WAV fixtures; both final texts matched exactly. Response command is ACK 1 and success codes are 0/10000. Live response `sequenceId` is a recognition revision and may be zero or repeated: it does not echo the request sequence. Correlation uses the mandatory matching Packet cid and response sourceId. Request sequences still start at one and increment for every request.

The recorder uses binary internal transport: an empty BEGIN establishes the connection before microphone capture. Each 100 ms capture block is decoded to PCM16k/mono/s16le for six-block, 600 ms uploads, followed by an empty END. The compressed queue and RAM budget are described below. There is no Base64 or full-recording PCM copy. Every exit releases microphone, connection, and temporary queues.

Interim results are cumulative previews, not appended fragments. Ordinary recording now uses persistent drafts; the existing 512-byte RAM buffer is only a preview. Fully validated final text follows the existing up-to-300 KiB storage quota and paged review. END must validate completely. Oversized or truncated interim text cannot become a valid draft. Failures preserve existing durable drafts, and voice-login errors do not revoke ordinary chat OAuth.

Voice credentials use the mirrored shared-pool configuration key 8, scoped to the current application-binding epoch, with a 4,096-byte ASCII limit and header-injection rejection. Rebinding isolates the previous session. Development provisioning uses a fresh encrypted device snapshot and preserves existing files byte-for-byte; automatic on-device web-session QR renewal is not implemented yet.

Host checks cover fragmented/invalid UTF-8, missing/wrong correlation IDs, zero/repeated revisions, late errors, 300 KiB, sink failure/recovery, and credential persistence/binding isolation. Device microphone, resource peaks, network interruption and expiry recovery must be reported separately from these host results.

### Upstream XiaoZhi comparison and remaining congestion work

Source snapshot: [78/xiaozhi-esp32, a78ca19](https://github.com/78/xiaozhi-esp32/tree/a78ca19d5bc9a12ee0853b9f509530276d36aa03). Its input, Opus codec and network work are separate. PCM encoding queue capacity is two 60 ms frames; the compressed send queue holds up to 40 frames (2.4 seconds). Full input/send queues discard the oldest frame; a send failure drains the remaining send queue. The WebSocket transport sends binary audio without awaiting an ASR result for every frame.

The historical failure combined 238/257 ms requests for 200 ms audio with buffers held until the response, exhausting a 0.4-second pool. Batching, early release, and the compressed queue below replace this behavior. Neither Opus wire transport nor a Flash audio queue is implemented. Complete messages must not silently discard old speech frames.

`FEISHU_VOICE_AUTOTEST=1` explicitly enables temporary diagnostics, overlaying public fixtures after real I2S reads to test ASR, persistent UI drafts, and recovery without sending business messages. Ordinary firmware excludes fixtures and automatic actions.

### Persistent voice drafts restored

The ordinary path enters the recorder immediately without a foreground draft
scan. Its worker chooses a free protected slot, or the existing slot for an
explicit re-record. A validated END result and destination/UUID metadata are
published atomically in the shared pool. Four occupied or unreadable slots
reject another recording without overwriting saved content. Returning from
review keeps the draft; saved drafts are reopened through the draft manager.
The 512-byte RAM text is a preview, while validated persistent results retain
the existing bounded storage quota and paged review. An oversized interim
preview followed by empty END is never treated as complete content.

Offline confirmation stays in review with a saved-draft notice and does not
queue a send. Failure or cancellation preserves the previous draft. A send
intent is persisted before POST; SENDING/UNCERTAIN states survive reboot and
require explicit review rather than automatic retransmission. The temporary
RAM-only fallback was introduced for the historical SQLite/SPIFFS journal
failure; it is no longer the ordinary recording default.

### Compressed RAM queue supersedes raw PCM queuing

Capture now reads 100 ms (1,600 samples) and encodes each block as self-contained IMA 4-bit ADPCM, 804 bytes including its state header. The sender decodes queued blocks back to PCM and streams six blocks per validated 600 ms request. Two PCM work buffers use 6,400 bytes; the compressed queue temporarily allocates 8 to 32 slots (about 0.8 to 3.2 seconds) from measured heap headroom. Queue expansion reserves the capture stack plus a 32 KiB runtime target for TLS/network buffers. After capture joins, audio queues, PCM and driver resources are released before final END and draft publication; the callback retains only a copied error value. Full queues fail explicitly rather than dropping old speech while claiming success.

ADPCM is lossy local buffering; the service still receives 32,000 bytes/second PCM. This extends jitter tolerance, not indefinite low-throughput capacity. The C codec matches independent Python audioop output byte-for-byte. Two synthetic fixtures measured 22.16/23.89 dB round-trip SNR and retained exact live-ASR text; this does not replace real-speaker quality acceptance. Flash audio spooling remains unimplemented.

Native regression adds a recoverable 1.5-second network pause, bounded failure after a 5-second pause followed by a normal retry, a 30-second recording, and persistent drafts through the actual UI worker. Restore ordinary firmware without fixtures or automatic actions after diagnostic testing.

### SQLite pager startup audit

The bundled SQLite 3.54.1 source emits OP_Transaction and OP_SetCookie for the auto_vacuum setter even when INCREMENTAL is already active. Repeating this setter on every open makes cache reads depend on journal creation. Startup now reads the current mode first and invokes the setter only if needed. It also queries for stale pins before issuing their reset, avoiding an unconditional UPDATE.

The source-level fault VFS denies journal opening. Existing databases without stale pins reopen without any journal attempt; explicit setter/write failures preserve protected data; stale-pin cleanup still fails safely when writing is unavailable and succeeds after pressure is removed. The ordinary 32 KiB limit remains in force for these operations. A separate host quick_check uses a larger scratch budget and is not evidence of firmware peak memory.

A prior bounded xOpen collection experiment passed host tests but failed on the existing physical SPIFFS pool and delayed startup; it was withdrawn. Journal/sync durability is retained. This startup change does not claim to solve sustained physical-page exhaustion or replace the storage-backend comparison.

### Authorized diagnostic voice provisioning

A diagnostic build can additionally enable `FEISHU_VOICE_AUTOTEST_PROVISION=1`. After ordinary chat authentication, it accepts one bounded local USB voice-credential frame, writes it through the real mirrored configuration API under the current binding epoch, clears the temporary buffer, and runs the existing silent ASR suite. It times out after 60 seconds and reports numeric outcomes only. The host reads the owner's existing DPAPI state; credentials are neither compiled into firmware nor written to plaintext snapshots. This tests actual configuration writes on the unchanged physical pool. It is not an ordinary-product login/renewal feature; normal builds exclude the receiver, fixtures and automatic actions.

### Current storage backend and clean setup

The default product selects FlashDB over the existing 1 MiB `feishu_img`
partition at `0x370000`. Firmware, font and device identity locations do not
move. The user explicitly authorized clearing application data and starting
over, so this change does not migrate the SQLite/SPIFFS pool or import legacy
robot credentials. A clean pool creates empty binding state and enters the open
Wi-Fi provisioning AP, followed by application selection/creation and user
authorization. SQLite remains an explicit reference build option through
`FEISHU_STORAGE_BACKEND=sqlite`.

The cache uses the `feishu_data` object contract and the `feishu_pool` platform
contract. Raw mounting, capacity checks and recovery do not appear in UI or
network code. Configuration and object data share one serialized engine;
logical category caps and reclaimable caches do not create more partitions.
A working margin and two erase sectors are retained for necessary writes and
GC. Room accounting is conservative and recounts only under pressure.

The object catalog has 128 slots and a 6,176-byte encoding. Compressed 1 KiB
blocks remain immutable; catalog publication makes a body/state pair visible
atomically. IDs reserve durable ranges, staging chunks are garbage after
interruption, and reader pins live only in RAM. Mention dictionaries and batched
sparse indexes preserve bounded text reads. Publication uses temporary catalog
buffers; its real product/TLS peak must be measured rather than inferred from
the standalone diagnostic's heap.

Configuration uses independent 512-byte chunks for both mirrored copies and
FRC1 headers, including 16 KiB records and zero-filled parts. Four small RAM
proofs retain successful validation until any raw mutation or engine reopen;
every requested chunk still passes the KV checksum. This avoids rereading a
whole record for each slice. Superseded generations are reclaimed only when a
valid manifest authorizes it; an active write generation cannot be collected.

### IO, GC and scheduling guarantees

A failed or short raw operation latches the open instance and blocks subsequent
writes. The cache invalidates outstanding leases and reopens on a later action;
it does not classify an IO error as permission to erase the pool. Unknown
formats or occupied sectors are not automatically reformatted. After interrupted
GC, a compatible EMPTY header can be completed only if the entire sector
payload is erased and another recognized raw sector exists.

A 192-byte compact directory caches sector states and verified tail offsets. Header-only queries no longer traverse record payloads; a host regression admits 140 blocks across sector boundaries with only 100 metadata bytes read. GC must complete every live-record move before erasing its source. Sector
header writes and record copies fail immediately on error. CRC reads use
256-byte pieces, moving records uses a 512-byte buffer, and positive key lookups
borrow the same temporary 1 KiB read window as scans. A 128-entry positive cache
compares full key lengths/names and ages old generations; a fixed 1 KiB negative
filter avoids unnecessary miss scans. The raw port yields periodically so long
maintenance cannot starve the display, Wi-Fi or watchdog idle task.

### Acceptance status

Host coverage includes the same object contract, text mapping, configuration,
real cache/store/outbox integration, interrupted publication, full-store GC
faults, CRC16 prefix collisions and recovery without reviving legacy NVS
credentials. Complete build and host gates are required for the final image.
Standalone object tests passed on the physical device; they do not establish
full-product RAM or UI behavior.

The current complete build and host gate passed. The full-product silent physical
stress test passed: 40 cache objects of 6 KiB, 24 revisions of 16 KiB mirrored
configuration, injected read/write recovery, and protection of the pending draft,
current reader and configuration. Total stress duration was 366.292 seconds;
minimum free heap was 42,524 bytes, with no watchdog or reboot. This is a
synthetic storage workload, not UI latency or an authenticated TLS peak.

A separate physical reset retained every byte of a 6,000-byte draft and 16 KiB
configuration. Its UNCERTAIN outbox state and UUID survived, and automatic
retry was rejected. Test data was cleared and that storage-stage ordinary image was
installed: APP 2,004,848 bytes, font 1,737,446 bytes, unchanged 1 MiB pool.
Identity, partition table and font hashes were unchanged. Evidence is in
`build/device-checkpoint/flashdb-product-native.json` and
`build/device-checkpoint/flashdb-reboot-native.json`.

Earlier watchdog and timeout reports and matching images remain as failure
evidence. Ordinary voice replies now use protected persistent drafts, with real UI-worker
record/save/offline/reboot qualification in the latest increment. USB provisioning of the test Wi-Fi network, device-generated application selection and
personal authorization passed. A physical reset retained bound=1, wifi_setup=0
and auth_request=0, loaded 10 cached conversations, then obtained live chat
HTTP 200. Phone hotspot provisioning, human microphone speech, physical-key confirmation
and sustained UI use remain unverified. Real message and injected-audio speech
paths were subsequently qualified as described below.
Physical power interruption during writes, bit corruption and long-run filter
saturation remain separate qualifications.

QR diagnosis: both device-generated authorization links succeeded. Closing the
host serial monitor after application selection missed the first personal link;
continuous monitoring captured its automatic refresh. The UI previously discarded QR draw results and marked failed draws complete.
It now checks the result, hides a failed QR, displays an error and retries no
more than once per second. A new URL/phase retries immediately; only a
successful draw is remembered, and recovery restores normal text. The portable
retry state uses at most 8 bytes; host cases cover repeated failures, recovery,
new phases and tick rollover. This fixes the known retry weakness, while
physical QR display/scanning remains unverified; it was not established as the
cause of the earlier missed host link.

### Real-message and speech-send qualification

An explicitly enabled full-product diagnostic received a synthetic post from
the authorized group, checked its durable text, sent a Unicode message with
quotes, backslashes and a newline, repeated the same send UUID, and sent a
quoted reply. A 4.8-second known PCM fixture overlaid actual I2S capture passed
through the normal buffering, internal-ASR and text-send APIs. Both outbound
bodies were read back byte-for-byte. An independent Lark user API found exactly
one ordinary message, reply and speech-result message, all sent as the user.
This qualifies speech-to-text sending, not a native audio attachment or human
microphone intelligibility. Silent speech normal/stress/error coverage passed
9/9, including 30 seconds, cancel, capture failure, delayed upload, overflow
and subsequent recovery.

This test found a real parser defect: Feishu returned parallel `content` and
`content_v2` representations and both were appended. The parser now traverses
only canonical supported content paths and ignores unknown nested alternatives.
The new fragmented host regression fails before the fix and passes afterward;
the parser context stays 6,992 bytes. The synthetic post shrank from the wrong
148 bytes to the correct 75-byte paragraph view, with its 73-byte text intact.
PowerShell also stripped a quote from the initial test stimulus; its independent
server value became the explicit post reference. Earlier failed test reports
and matching binaries remain available.

The real-message-stage ordinary image including this fix was
`build/flashdb-business-fix-final-product-verified`, APP 2,004,880 bytes. It
excludes business/voice test actions and preserves binding, the shared pool,
identity, font and partition locations. Evidence is in
`build/device-checkpoint/flashdb-business-native.json`,
`build/device-checkpoint/flashdb-business-independent-delivery.json`, and
`build/device-checkpoint/flashdb-business-final-boot.json`.

### Current persistent-draft acceptance

The latest ordinary bundle is `build/flashdb-draft-final-product-verified`, APP
2,006,144 bytes. Complete build/host gates passed, including four protected
slots reopened independently and cancellation of a staged replacement. In a
full-product diagnostic, actual I2S capture with a known 4.8-second fixture
produced a 48-byte draft through the normal UI worker. Stored text and review
matched; offline confirmation retained it. Two physical resets verified the
body, target and UUID, READY recovery and UNCERTAIN/no-auto-retry, then removed
only the matching synthetic draft. No business message was sent.

Speech normal/error/stress coverage passed 9/9 again, including a 1.5-second
upload pause and bounded failure/recovery for 5 seconds, plus 30-second capture.
The larger network reserve yielded 17–20 compressed blocks (1.7–2.0 seconds) in
this run. Minimum free heap over that diagnostic was 14,176 bytes; the separately
monitored UI record-through-save interval had 16,488 bytes. These are different
measurement windows, not interchangeable peak claims. Prior lower-margin
reports remain available. The final ordinary image retained binding and live
chat HTTP 200 after flashing, without erasing the pool or changing identity,
partition table or font. Reports: `build/device-checkpoint/flashdb-draft-native.json`
and `build/device-checkpoint/flashdb-draft-final-boot.json`.

Device-side voice login/renewal, long mixed-operation soak, human microphone
speech, physical buttons, screen/QR appearance and real write-time power cuts
remain separate unfinished or unverified items.

The earlier 20 KiB minimum-free-heap target remains an optimization goal: the latest 14,176-byte diagnostic minimum is below that target. Passing this bounded functional regression does not close the overall memory-budget or long-soak acceptance item.

### Microphone start acceptance (in progress)

Two physical attempts speaking immediately after “recording” lost the first clause, while waiting two seconds after that indication produced the full phrase. Queue high-water marks were only 200–400 ms against about 1.9 seconds capacity, with no capture error. This supports investigating startup readiness; it does not prove a codec fault or buffer overflow.

The current candidate pre-reads two seconds of real microphone samples while the UI still says preparing, reusing the existing PCM buffer. It signals recording after the first retained frame and permits cancellation during preparation. The settling interval does not count toward the 30-second recording limit. This is a bounded mitigation pending human-microphone confirmation, not a proven minimum warmup requirement. The existing Feishu backend remains selected; Xiaozhi integration is deferred at the user's request.

Separately, the preceding ordinary build completed a 1,201.16-second silent network/cache observation: 24 successful requests, no failed requests or reset/fault markers, and a 392-byte decline in the sampled free-heap upper envelope. This is neither a full mixed-UI soak nor proof of no memory leak. Evidence: `build/device-checkpoint/flashdb-draft-20min-soak.json`.

The installed candidate is now `build/voice-prime-final-product-verified`: APP 2,006,352 bytes, SHA-256 `0ddfe73b73f6aece5d4bca338ae6ee3d2d277abeeb24b1a267b0ad9f5720deb8`. Normal and diagnostic complete gates passed. The silent native suite passed 9/9, the normal UI worker saved/reviewed/retained the 48-byte fixture draft offline, and two resets preserved its target/body/UUID and rejected automatic retry. Only the matching synthetic draft was removed. The diagnostic minimum free heap was 15,228 bytes; the separately monitored UI recording-to-save window reached 17,916 bytes. Neither meets the earlier 20 KiB goal. Human prefix confirmation remains pending.

Latency remains open: the UI END interval was about 13.1 seconds followed by about 9.8 seconds before publication completed. END includes streaming draft writes, so this cannot all be attributed to remote recognition. Separate network, storage and publication timing is the next diagnostic. Reports: `build/device-checkpoint/voice-prime-suite-native.json` and `build/device-checkpoint/voice-prefix-investigation.json`. The earlier diagnostic without the suite flag was rejected and rolled back; its report is retained rather than counted as a nine-case pass.

### Slow-send catalog diagnosis and candidate (not installed)

The original real-device short-send baseline measured 4,142 ms to persist intent, 1,096 ms for the send API (1,035 ms HTTP), and 4,090 ms to persist acknowledgment. Intent/ACK each performed about 22,000 Flash reads. An eight-message readback took 34,101 ms including cache storage. These are different phases; they must not be reported as remote latency alone.

A read-only device-pool snapshot replayed with the actual FlashDB source showed all 128 catalog entries occupied: 106 live and 22 retired. Allocating a new catalog entry triggered whole-pool orphan scans despite reusable retired entries. The candidate reuses only unpinned, unreferenced retired entries, leaving them unchanged until the new immutable key is written. Live/protected versions and active readers remain protected; a reset before publication can restore the old retired entry, and object IDs do not repeat. No resident cache or media-format change was added.

On the same snapshot in the host Flash model, intent-update reads fell from 23,216 to 128; draft-publication reads fell from 13,343 to 31. This is an I/O-count comparison, not a physical latency claim. A new full-catalog regression fails against the previous implementation and passes after the fix; it covers pinned exhaustion, a failed begin mutation, protected latest content and reset before publication. Existing atomic-pair, fault/recovery, draft and cache-pressure gates also passed.

Candidate bundle: `build/send-slot-final-product-verified`, APP 2,006,752 bytes, SHA-256 `830e8b083329eb4811d202c1f7dadb422b2399e49223828f1bc42923c36939f5`. Build and Host tests: PASS. New Device tests: NOT RUN because the ESP32 USB device is no longer enumerated; only Bluetooth COM3/COM4 are visible. No candidate write or second test message occurred. The last verified installed ordinary image remains the preceding microphone-start candidate (`0ddfe73b…`), restored after the successful original performance baseline. Reconnect and identify the exact USB device before native timing and installation. Reports: `build/device-checkpoint/send-perf-baseline-native.json`, `send-perf-host-stack-baseline.txt`, `send-perf-host-stack-slot-reuse.txt`, and `send-slot-probe-native.json` in the same directory.

The user reported one apparently normal human-microphone retest after the startup change; repeated physical prefix and UI-latency acceptance remain separate.
