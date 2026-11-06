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

Settings is a low-frequency maintenance entry: Feishu account, XiaoZhi settings, Network, Draft management and Return. Feishu account owns application selection and user authorization in one flow. Changing the binding requires confirmation; DOWN double-click in a message list focuses the composer without recording.

Account menu labels are BOT authorization, Select or create BOT, and Return. BOT is the user-facing name of the Feishu application; OAuth still authorizes access to the user's data and messages are sent as the user. Network settings owns a list of up to ten saved networks, nearby-network selection, a clear-text ASCII password keyboard and a phone provisioning entry. Its page title is Network rather than Confirm operation. Device-side switching tests the candidate connection before atomically publishing the active profile; failure attempts the previous network. The previous single-network recovery record remains untouched for firmware rollback. This recovery guarantee applies to the device-side switch, not to the existing phone provisioning flow.

On this ESP-IDF build a driver Wi-Fi config is 184 bytes: ten records plus a 16-byte book header require 1,856 logical bytes. Root redundancy, FlashDB metadata and reclamation add physical overhead. A 2 KiB temporary book buffer is released after each operation; list/editor state is allocated on entry and cleared on exit. Capacity is a maximum, not a reserved partition. The keyboard interaction references passport-os `main/launcher/wifi_keyboard.h` (MIT).

Add network and manual provisioning stay below the saved-network list, separated by whitespace. Entry defaults to Add. A network opens Connect, Disconnect, Forget and Set password. Disconnect preserves the record and suppresses automatic reconnect for this runtime; editing a password preserves the active profile and does not connect. Forgetting the last profile leaves an explicit empty book so legacy credentials cannot silently reappear in the current firmware.

Ordinary DOWN crosses from the final list row to the primary action, and UP returns from that action to the final row. Double UP restores list focus; double DOWN focuses Send/Add without executing it. Network/editor state participates in the repaint signature; no always-redraw loop or extra permanent image buffer is needed.

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

### Device reconnection and send timing

The catalog-reuse ordinary image (`830e8b08…`, APP 2,006,752 bytes) is now installed. The full-product diagnostic preserved binding, pool, identity, font and partition table. A unique 55-byte user message was sent and read back; an independent Lark user API found it exactly once with the user sender type. Intent persistence took 144 ms, the send API 1,317 ms, and ACK persistence 32 ms. The preceding 45-byte baseline took 4,142 / 1,096 / 4,090 ms respectively. These are two short-message device runs, not identical-payload laboratory benchmarks.

An important remaining failure of the latency target: reading and caching eight messages still took 55,508 ms and 292,976 Flash reads. The send-path improvement does not qualify overall UI/cache latency. Evidence: `build/device-checkpoint/send-slot-probe-native.json` and `send-slot-independent-delivery.json`.

A subsequent cache-pressure candidate collects up to eight unreachable keys per scan, ends iteration before deleting, and falls back to one stack entry if the transient allocation fails. It also combines up to four small eligible cold-object retirements, stopping at 4 KiB stored data; a large object is retired alone. All catalog removals publish atomically, rollback restores the selected entries, and pinned/protected objects remain excluded. No resident cache or format change is added. On the same host-replayed device snapshot, reclaiming about 16 KiB fell from 118 calls / 266,695 reads / 942 writes to 20 calls / 71,884 reads / 309 writes. Physical cache-pressure timing remains pending. A new host regression passed atomic grouped retirement, multiple-orphan deletion, 16 interrupted writes, read failure, IO-latch stop and protected-owner/reader recovery.

### Located collection and mapped-read qualification

The final candidate uses up to 32 selected key/locator hints per sweep; normal lookup revalidates CRC, status and the complete name before deletion. A 16-byte omission mask publishes up to 16 small cold-object evictions, stopping at 4 KiB stored data, without modifying RAM entries before the catalog is durable. Root-only mutation epochs avoid repeated empty root scans; conservative usage recounts repeat only after operations that may overcount. The same approximately 16 KiB host cleanup now uses 7 calls / 16,478 logical reads / 177 writes, versus 118 / 266,695 / 942 originally. These remain whole-pressure-workload counts, not per-message costs.

ESP-IDF maps the 1 MiB pool read-only into the CPU data address space. It does not allocate a 1 MiB RAM copy; small mapping metadata is additional overhead. Writes/erases remain normal driver operations with SDK cache invalidation. Mapping allocation failure falls back to ordinary reads, and unmount releases the mapping. Diagnostic code primes cached data then compares every affected write/erase span to independent ordinary reads. This qualification performed 3,305 reference reads without a mismatch. Logical mapped reads, normal read-driver calls and diagnostic verification reads are separate counters; zero normal read-driver calls does not mean the Flash chip is never accessed.

The first native run passed message/coherence tests but only 5/9 speech cases; four timed out and the controller restored the previous ordinary image. An older same-network speech baseline then passed 9/9. The unchanged new binary was rerun with request/queue telemetry and passed 9/9, including the 1.5-second pause, deliberate 5-second failure/recovery and 30-second capture. The first timeout cause is not established and remains open; passing the rerun does not erase that failure.

Installed ordinary bundle: `build/cache-read-final-product-verified`, APP 2,007,888 bytes, SHA-256 `669928f5ea9dfb91e65191796fb9ad1b2d037e39b1048264e748e98cb47db5e8`. Build and Host tests passed. On the successful rerun, intent/API/ACK took 31/1,288/144 ms, totaling 1,463 ms; eight-message retrieval/cache took 5,478 ms (26,403 mapped logical reads, zero ordinary read-path driver calls). The earlier candidate run took 4,574 ms (1,627 mapped logical reads). The prior installed image took 55,508 ms and 292,976 ordinary read callbacks for its eight-message workload. Payloads, pool state and history changed across native runs, so these demonstrate observed improvement rather than fixed per-message bounds. Reports: `build/device-checkpoint/cache-read-probe-native.json`, `voice-network-baseline.json` and `cache-read-rerun-native.json`. Binding, pool, device identity, partition table and font were retained. Physical UI acceptance and the first intermittent timeout remain open.

### Recording and conversation readiness

The capture task drains real microphone samples while the empty ASR BEGIN request establishes the connection. Only its capture PCM, task and synchronization resources exist during this handshake; the upload PCM and bounded compressed queue are allocated after BEGIN. The microphone continues draining until both the two-second settling interval and network preparation finish. Recording readiness follows the first retained frame. Failed preparation and cancellation join the capture task before releasing audio resources. This overlaps existing work without a second task or an unbounded recording backlog.

A received or cached message page clears the loading state when applied to the selected conversation, before optional snapshot persistence and read-state updates. Navigation cancels obsolete message downloads; page-request and owner guards reject stale results. Numeric readiness timing distinguishes RAM hits, disk hits and network loads. The diagnostic build exercises two conversation loads followed by durable cache hits; physical button-to-screen latency and microphone prefix quality remain separate acceptance checks.

For cold multi-chunk snapshots, a read-only prefix iteration warms the existing bounded KV locator table once; a cached first/last pair skips this iteration. Chunk reads still validate the exact key, record status and CRC, and I/O failure remains latched until reopen. No extra resident index or media-format change is introduced. The host comparison read the same 16 chunks amid 360 other keys: 619 reads individually versus 111 with grouped warmup; repeated warmup, replacement and failed-read recovery are covered.

The first concurrent-preparation prototype recorded readiness near 2.12 seconds but passed only 8/9 voice cases: its 16-frame queue filled in the 1.5-second upload-pause case. It was rolled back. The next candidate budgets 30 KiB of network headroom after the already allocated capture task and queues, allowing additional transient compressed frames within the existing 32-frame ceiling. The original 20 KiB minimum-free-heap goal remains open; native stress qualification is required before installation.

Qualification completed on 2026-11-06: the full gate passed, and the second candidate's first native run passed 8/9 (the first normal recording exhausted its bounded queue during a roughly 2.5-second request). It was rolled back. A bounded rerun of the identical image passed all 9 cases, including upload pause, failure recovery and 30-second capture. Both runs and the earlier 8/9 prototype are retained; intermittent timeout is still open.

The installed ordinary APP is 2,008,736 bytes, SHA256 `e1183c110d27aac154a04145ef36c07fb84b3594e69afa32c441a85995fd52dd`. ASR preparation measured 2,117--2,120 ms, two network page displays 5,560/4,829 ms and their later disk hits 619/341 ms. These are distinct stages, not end-to-end physical button timings or a controlled same-pool before/after comparison. Minimum free heap was 13,884 bytes, below the 20 KiB goal. Real send/readback and independent user-identity API checks passed. Installation touched only the APP; binding, data pool, identity, partition table and font remain intact. Physical prefix and button/screen acceptance are pending.

### XiaoZhi ASR and compact dark UI candidate

The source default remains Feishu pending human microphone/UI acceptance. The installed, automatically qualified variant explicitly selects `-DFEISHU_ASR_BACKEND=xiaozhi`. Changing a build selection is not itself acceptance; UI and erase-recovery changes are independent of that selection.

The earlier Feishu-backend UI/storage build was `theme-feishu-stable-v15-final-product-verified`: APP 2,011,712 bytes, SHA256 `385097017562bbb894d5b23812b4a2658aca645bc57c003b5c6593024b9aeeb4` (2,976 bytes larger than e118). Build and the full host gate passed. Device checks verified 64 pool headers, binding, the 1 MiB raw store, 16-level antialiased font, 96% SOC and a connected Wi-Fi link. Partition table, identity and font bytes were unchanged. Five simulator screens were reviewed and the visual direction accepted. Its device evidence covered startup only, not new recording/send acceptance; ordinary firmware performs no automated test sends.

Before the AP-selection fix, the experimental 8 kHz run passed only 3/9 voice cases. Recorded RSSI was -77 to -79 dBm and a separate 55-byte text POST waited about 30.2 seconds before a response-header timeout. Its outcome was ambiguous and was not automatically retried. The harness-owned pending test draft was subsequently cleared without touching genuine drafts or sending another message, then the ordinary v15 image and binding were restored. RSSI alone did not establish a cause; the user subsequently confirmed that the device was already beside the router. The candidate's 13,672-byte minimum heap belongs to that earlier candidate. Evidence: `build/device-checkpoint/theme-feishu-v15-boot.json`, `theme-owned-cleanup-native.json`, and `theme-xiaozhi-v14-native.json`.

The latest requirement enables XiaoZhi evaluation. The candidate obtains WebSocket configuration from the device service, stores its credentials in a separate mirrored root record and uses a verified TLS connection for manual listening. Only STT text is consumed; audio playback is never started. The older Feishu implementation remains selectable with `FEISHU_ASR_BACKEND=feishu`. A host sample using 20 ms, mono 16 kHz PCM after the actual local IMA4 encode/decode returned the exact expected 48 bytes of text about 172 ms after stop; this is host evidence, not an ESP32 timing result.

The capture queue covers 2.4 seconds using local IMA4 compression; the wire carries PCM. Overflow fails explicitly instead of silently deleting old speech. Entirely zero PCM capture cannot produce a draft, because the server returned unrelated text for a digitally silent host test. Optional local-cache work is joined before audio allocation and recreated after audio/TLS teardown; its reader and draft data remain in the shared pool. Capture uses a 4 KiB task stack and roughly 22 KiB context, avoiding the Opus encoder. Both earlier Opus attempts (60 ms/6 KiB and 20 ms/12 KiB) triggered stack protection and were rolled back. The PCM native candidate had no stack fault but failed continuous upload; it was also rolled back. Device qualification remains open.

Upload telemetry identified socket-write readiness timeouts while capture remained healthy. Disabling Nagle improved progress but still exhausted the queue. The next candidate packs the masked WebSocket header and PCM into one TLS write and completes positive partial writes under an absolute deadline; an ambiguous failure closes the connection without replay. Host tests cover frame lengths through 65535 bytes, masking, aliasing and insufficient capacity without buffer mutation. Its first native run was blocked by Feishu initialization connection/header errors before ASR started; this does not establish the transport fix's outcome. The 16 kHz native rerun still failed upload. The next candidate keeps 16 kHz capture and its IMA4 queue but averages adjacent sample pairs for an 8 kHz PCM wire stream, halving upload traffic without increasing the queue. The matching averaged host sample returned exact text 157 ms after stop. Native long-recording and physical recognition quality remain unverified.

A subsequent reset exposed an interrupted 16 KiB logical-sector erase: its first physical units were blank while the tail retained data, so safe open rejected it. The encrypted device snapshot reproduced this error on the host. A local repair experiment validated all ten configuration types and 98 live objects before the single damaged sector was cleared on-device; every byte outside that sector was verified unchanged, and binding was read back. To prevent recurrence, the raw adapter now erases physical 4 KiB payload units before the logical sector's header unit, retaining its GC recovery marker until the payload is blank. The regression reproduces failure with the old order and verifies reopen, retained values and new writes after interruptions at all four units with the new order. Whole-device power-loss acceptance remains separate.

UI layout uses six 36-pixel conversation rows and four 44-pixel message previews. Recognition review contains only the outgoing body; notices/errors and controls remain in separate footer lines. Details begin below the header rule and use a 512-byte UTF-8 window, with bounded scrolling for longer content. The shared dark palette also covers owned setup pages, the boot screen and the Wi-Fi portal; QR modules use a local pale green surface. Backlight starts at 70 percent.

The fuel-gauge driver verifies the known 520 mAh board profile and samples outside the network-idle timeout. The device reported 96 percent around 4.11 V after convergence; complete charge/discharge accuracy has not been validated. A dependency refresh unexpectedly selected LVGL 9.6 and expanded its pool from 24 to 64 KiB, preventing Feishu initialization. The candidate was rolled back, LVGL was pinned to 9.5.0, and verification now checks the actual ELF pool symbol.

### AP selection and qualified XiaoZhi device variant (2026-11-06)

The saved configuration used `WIFI_FAST_SCAN` with a channel-1 hint. It selected a same-SSID AP around -70 dBm while a channel-6 AP scanned around -49 dBm. The SDK stops a fast scan at the first SSID match. For an unpinned BSSID, the runtime configuration now clears the channel hint, uses `WIFI_ALL_CHANNEL_SCAN` and sorts by signal. Explicit BSSID pins and stored credentials are retained.

Without moving the device, the diagnostic selected the stronger AP at about -54 dBm. Both baseline and fixed versions passed 16 synthetic LAN cases, including certificate-verified TLS and approximately 96 KiB free heap. The same 16 KiB raw TCP upload measured 897 ms before and 74 ms after selection changed. DNS resolved in 3–30 ms; verified WAN TLS and unauthenticated root GETs also responded. These isolate observations, not business-API qualification. The optional fixture is documented in `tools/network_probe/README.md`.

The installed ordinary XiaoZhi variant is `ap-selection-v17-xiao-final-product-verified`: APP **2,020,544 bytes**, SHA256 `bd8b57f136067b1612bb21b9dd2f43f1fb76b53699556d1b5d9ea34e745064c0`. Build and full host tests passed. Native diagnostics passed **9/9 voice cases**, including 30-second capture, 1.5-second upload pause, cancellation, capture failure, 5-second timeout and recovery. Known PCM fixtures overlay real I2S reads; this is not human speech acceptance.

| Device measurement | Observed result | Scope |
| --- | --- | --- |
| Recording preparation | 2,040 ms | Stable pre-read retained; not physical button-to-screen timing |
| Final recognition | 139–187 ms after stop | Complete text; ordinary queue high-water 1 frame, injected pause 75 frames |
| Real text send | POST 1,199 ms, HTTP 200 | Unique 55-byte message and API readback |
| ASR to persistent review/send | Durable=1, exact UI text=1, POST 1,260 ms, HTTP 200 | Actual ASR writer published and ACK persisted |
| Conversation switching | 4,095 / 4,443 ms cold; 29 / 34 ms disk hits | Includes diagnostic work; not a latency guarantee |
| Minimum free heap | 18,564 bytes | Diagnostic workload only; 20 KiB target remains open |

The first end-to-end diagnostic failed because its helper appended a test suffix after ASR's final short block, violating the text writer's documented range contract. The corrected helper passes the original ASR writer through publication, exact UI comparison and sending without altering its text. That failure remains recorded; no production storage guard was relaxed. The installed ordinary image has no automatic test actions. Binding, pool, identity, partition table and font were preserved. Human microphone quality, physical UI/buttons, long mixed workloads and real power-loss acceptance remain pending.

Evidence: `build/device-checkpoint/network-probe-v1-native.json`, `network-probe-v2-native.json`, `ap-selection-v16-xiao-native.json`, and `ap-selection-v17-xiao-native.json`.
## Interaction and typography refinement (2026-11-06)

One OK on the message composer starts recording. Focusing the composer optionally opens the silent microphone and discards settling samples; leaving it requests immediate cancellation. The worker joins the audio owner before handoff or teardown. Low-memory failure skips optional warm-up and retains the cold-start path. No warm-up samples are stored or transmitted. Once settled, XiaoZhi capture retains audio while connecting, within the existing 2.4-second bounded queue; overflow fails explicitly and the next attempt remains usable.

Successful sends invalidate the latest-page cursor and request a fresh current-conversation page. This uses the same production send handler in the native diagnostic, rather than testing the HTTP helper alone. One-line bubbles use 28 pixels, two-line bubbles 44 pixels; a tested variable-height window keeps selection visible and fits up to five messages above the composer.

An earlier iteration used five title/description cards; the current UI uses compact text-only rows without icons or subtitles. The read-only design reference is [passport-os settings](https://github.com/manchunx7-bit/passport-os/blob/b70879b3c4be0751cb728b8b0c421dd2d1524fdb/main/launcher/settings.cc), linked by community play 438. Product colors remain the Feishu dark palette. Body text changes to pale gray-teal #B9CDD0; muted text #8DA7B3. Source Han Sans SC Medium 2.005 replaces Normal at the same 14 px size, 17 px line height, baseline 4 and compressed 4 bpp (16 gray levels). The mapped bitmap is 1,814,190 bytes, +76,744 bytes; APP and font must be updated and rolled back as a matching pair.

## XiaoZhi conversation and MCP feasibility

**Feasible as a staged extension; current ASR integration is not a complete voice assistant.** The current hello advertises `mcp:false` and closes the voice connection after recognition. Full integration needs a persistent conversation session, MCP negotiation, tool dispatch, interruption and optional TTS.

The [upstream protocol](https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-protocol.md) and [usage guide](https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md) describe the backend as MCP client and device as MCP server. Cross-check at local upstream commit `72da544d1b51678f5d88967adc299b3aec956946`: `main/mcp_server.cc` handles `initialize`, `tools/list`, `tools/call` and schedules tool execution; `main/protocols/protocol.cc` wraps JSON-RPC inside a session-scoped `type:mcp` message. A bounded host probe against the configured service then completed hello, initialize, notifications/initialized and tools/list with zero tools, zero audio and zero Feishu actions. This is actual host negotiation evidence, not device-side MCP execution or a memory qualification.

| Stage | Device capability | Required acceptance |
| --- | --- | --- |
| Read-only MCP | Current page, bounded conversation/message queries, page navigation | Real initialize/list/call; pagination, invalid arguments, cancellation and reconnect |
| Draft and send | Speech creates a reviewable draft; physical confirmation invokes existing user-identity outbox | Correct recipient and text, stable UUID, uncertain-send recovery, no duplicate replay |
| Full conversation | Keep voice session, route replies, optional interruptible TTS | Heap/stack high-water marks, two-network-session pressure, microphone/output arbitration |

Use the existing worker and typed domain actions; MCP callbacks must not call LVGL directly. Responses have explicit byte/count limits and cursors; chat content is data, never tool instructions. Duplicate request IDs must not repeat writes. Credential-bearing fields are excluded from tools and logs.

The principal open resource question is overlap between a persistent XiaoZhi TLS session and Feishu HTTPS. Release recording queues and DMA before HTTP tools; measure the actual peak with the 24 KiB LVGL pool. Do not import the entire upstream C++ framework or promise simultaneous TLS/TTS fits on this C3 without measurements. A server bridge is a separate deployment choice, not required by this proposal. This increment researches MCP; it does not enable unrestricted remote sends or deploy a new service.

## Resource lifetime and device MCP experiments (2026-11-06)

The first three-cycle heap snapshots did not establish an audio leak. Two independent four-phase diagnostics each completed 80 cycles. In the second run, direct codec use and short-lived warm-up tasks each held exactly 80,608 bytes of settled heap and eight tasks across twenty iterations. During twenty network/cache reads, each additional TIME_WAIT control block correlated with about 212 fewer free bytes. TCP control blocks are 208 bytes in the matching ELF; the configured MSL is 60 seconds.

A 130-second quiet window with no requests or reboot observed TIME_WAIT 16 -> 0 and free heap 83,272 -> 86,712 bytes (+3,440). This supports delayed TCP teardown for the observed network trend. It does not certify lifetime freedom from every leak or explain every earlier short-lived fluctuation. Frame-pointer/heap-trace diagnostics have different overhead from the ordinary product. Evidence: `build/device-checkpoint/memory-v20-native.json`, `memory-v21-native.json`, and the numeric `memory-v21-summary.json`. Reproduce the summary with `tools/memory_probe/analyze.py`.

The ordinary resource lease now pauses the optional 8 KiB cache worker before microphone warm-up and resumes it after focus loss, warm-up failure or record completion. Disk reads still fall back to the business worker while that optional task is paused. Bindings, pool addresses and protected drafts are unchanged. The qualified lease-only build was 2,022,768 bytes; voice nine-case, warm/cancel/recovery, production send/current-page refresh and three focus/network cycles passed. Its diagnostic minimum heap remained 16,448 bytes; the 20 KiB target was still open.

### Compact audio queue candidates

The capture format stays 16 kHz, while the existing wire uses 8 kHz PCM. Move the same bounded two-sample averaging before local frame-scaled six-bit PCM encoding. The RAM-only packet stores a 16-bit peak and 160 six-bit values; error is bounded by peak/62 + 1. This is lossy and requires speech-quality regression.

| Property | Previous queue | Candidate |
| --- | --- | --- |
| Frame duration / depth | 20 ms / 120 | Unchanged: 2.4 seconds backlog |
| Buffered samples per frame | 320 at 16 kHz | 160 at 8 kHz |
| Packet including length | 166 bytes | 124 bytes |
| Audio context estimate | 21,928 bytes | 16,240 bytes; saves 5,688 |
| Wire audio | 320-byte 8 kHz PCM per frame | Same format, duration and byte count |

The first 8 kHz four-bit ADPCM candidate (V25) saved 10 KiB but passed only 8/9 native voice cases. Three interleaved host comparisons each reproduced the brand-name error on the candidate while the prior encoding matched all three times; its golden transcript was not weakened. V25 was automatically rolled back. The six-bit candidate then matched all three host comparisons. Host tests cover the entire signed 16-bit input domain against an absolute-error bound, pair-average rounding/saturation, packet bounds, malformed recovery and forty seconds of repeated encode/decode. V26 passed all nine native voice cases, warm-up/cancel/recovery, durable ASR review/send, current-page refresh and three focus/network cycles. It is installed as the ordinary product. The minimum diagnostic heap was 22,372 bytes, above 20 KiB for this workload; human speech quality and longer mixed use remain unverified.

### Device MCP prototype, separate from ordinary ASR

The explicit diagnostic completes device-to-service initialize/tools-list, advertises only `self.feishu.get_status`, and locally replays that read-only call. It then frees request/config/parser workspaces, keeps WSS open, reads a one-message Feishu page and verifies an echoed ping nonce. The validated prototype's minimum heap was 37,644 bytes. It transmitted no microphone audio, sent no Feishu messages and exposed no credentials or personal content as tools. `remote_calls=0`: cloud semantic tool invocation was not exercised. Ordinary ASR still advertises MCP disabled.

The first device prototype failed its final liveness assertion because the IDF WebSocket transport consumed PONG internally and reset the visible opcode. The corrected version explicitly propagates control frames and uses a tested allocation-free assembler for split payloads and text fragments interrupted by ping/pong. Matching-nonce PONG passed on the device. The earlier failed run and its successful restoration are retained in `mcp-v23-native.json`; the corrected run is `mcp-v24-native.json`.

Before a full assistant, move the receive loop outside the business worker and queue typed requests so HTTPS cannot block control-frame servicing. The bounded prototype measured only 900 bytes of remaining worker stack during the nested HTTPS call. Full query paging, UI navigation, voice/TTS concurrency, semantic calls, reconnects and reviewed sending remain unimplemented/unverified; one-message dual-TLS success does not qualify an eight-message workspace.
