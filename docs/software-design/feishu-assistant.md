<p align="right"><a href="feishu-assistant.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Global assistant and collaborative composition

The Feishu terminal keeps its business screen visible while XiaoZhi is active.
Speech can query conversations, read messages, navigate, edit a draft and change
supported settings. Sending requires a preview followed by explicit confirmation
in a later recognized user turn or through the physical OK key.

## Activation and screen frame

- Double-press OK toggles activation on every configured business page.
- The assistant indicator beside the battery is always visible: gray when inactive and
  bright teal with a two-second breathing cycle when active.
- Successful initialization plays the success chime when enabled. Boot activation
  leaves the conversation list visible rather than opening a separate assistant page.
- Header and footer separator rules span all 240 pixels. Text retains its margins.
- Key names use the same color as other control hints. Ordinary load/cache notices
  become the activation/deactivation shortcut; errors and necessary operation state
  remain visible. Normal/success notices are green, warnings amber and errors red.

## Audio and worker ownership

One existing business worker owns the persistent WSS session. No second permanent
assistant or feedback worker stack is added. The codec stays at 16 kHz across
listening, UI loads, tool operations and reply playback; negotiated 24 kHz Opus
output is resampled locally. Capture queues and the Opus decoder do not coexist.
The microphone remains open during an active session; self playback is not treated
as a new command. This is not acoustic echo cancellation or simultaneous barge-in.

Local VAD retains six 20 ms frames before activation. Quiet time does not create
empty cloud conversations. On a quiet-work transition, a one-frame speech-onset
race resumes capture with its existing preroll instead of dropping the prefix.
Ordinary queued reads and thirty-second passive refreshes retain capture and yield
to speech; battery sampling continues every fifteen seconds. Exclusive dictation, network
changes and reauthorization hand control back to the ordinary worker as needed.
Logical activation resumes after dictation, and send completion returns to the
business context with the assistant available.

Business pages preserve their original OK action; commands submit after the configured
two-to-five-second silence (default two). The zero/OK setting applies in the editor
and dedicated legacy assistant page; other business pages retain a two-second fallback.
In the draft editor, spoken-command OK submits it, while idle OK opens send
confirmation and a second OK sends. Long OK cancels confirmation and retains text.
Double-press OK always deactivates instead of sending.

## MCP contract

Discovery is paginated and every request is validated before execution. Current
page, account epoch and immutable recipient checks prevent using stale UI context.

| Tool | Behavior |
| --- | --- |
| `self.feishu.list_chats` | Search the current cached page, at most four results per call; follow its continuation offset. |
| `self.feishu.get_messages` | Fetch a fresh latest-message preview; never mistake a preview for complete long text. |
| `self.feishu.open_chat` | Select a known conversation and load its list before returning the tool result. |
| `self.feishu.open_composer` | Open collaboration for one known conversation, optionally bound to a validated original message ID; does not send. |
| `self.feishu.navigate` | Open conversations, draft management, or supported account/network/assistant/sound settings pages. |
| `self.feishu.get_context` | Read current page, selected chat/message preview, draft size/state and send state; no credentials. |
| `self.feishu.read_draft` | Read bounded text pages plus the current revision; mention prefixes are separate from editable body text. |
| `self.feishu.prepare_message` | Prepare one complete draft requiring device confirmation. |
| `self.feishu.edit_draft` | Replace the body using its revision; recipient and reply thread stay fixed. |
| `self.feishu.undo_draft` | Restore one optional volatile prior edit; no send or recipient change. |
| `self.feishu.request_send` | Preview the current revision, then accept a later-turn explicit confirmation and enqueue one send. Queued is not delivered. |
| `self.feishu.settings` | Read or persist seven supported settings; only volume supports relative delta. |
| `self.feishu.find_member` | Verify one named user or bot in a known/current group; return an ID only for a complete unique match. |

Arguments reject duplicate/unknown fields, wrong types, arbitrary IDs, oversized
text and stale revisions. A failed named-chat lookup stays unresolved for that
utterance; listing all cached chats cannot authorize a different guessed target.
Tool output and received message text are data, never executable instructions.

## Collaborative draft editing

Dictation records literal text. Collaboration treats speech as discussion or an
explicit editing instruction. Advice does not automatically become outgoing text.
The draft remains visible in the middle of the screen; assistant explanations use
speech and the footer. The model's discussion/edit interpretation still needs real
conversation acceptance.

Only complete drafts below 512 bytes can be replaced through these tools. Missing
bodies, incomplete previews, uncertain or partial sends, malformed UTF-8 and stale
revisions are rejected without replacement. One global draft is retained; a new
complete composition replaces it. Flash publication failure preserves complete
short text in RAM with a volatile warning instead of blocking confirmation.

Undo keeps at most one bounded optional snapshot for the active editing session.
Leaving that context or closing the session releases undo, not the current draft.
Idle draft bodies are disposable Flash cache; active read/upload leases protect
necessary content. A missing evicted body is absent, never a ghost draft.

## Mentions and acknowledged sends

Optional `mentions` accepts at most two display names. The device resolves users
and bots through the owner's app-scoped OAuth identity with bounded streaming
pagination. Missing/ambiguous/incomplete results fail closed. Plain literal at-name
text and supplied markup do not become verified mentions. Edits preserve existing
mentions by default; an explicit array replaces them, and an empty array clears them.
The review shows readable names, not internal markup or IDs.

A successful POST supplies the real message ID and a bounded preview of the actual
sent segment. A short-lived, display-only echo keeps it visible while the latest
server page catches up. It is deduplicated when the server returns that ID, never
persisted as invented history, and expires after thirty seconds. Real structured
mention delivery and message-list behavior still require a confirmed target send.

## Feedback and memory bounds

Success, failure and key sounds have independent persistent switches, all enabled
by default. Short harmonic tones follow the 438 reference: 18 ms key, 48 ms success
and 42 ms failure. One coalesced request prevents late queued key sounds. The active
audio owner plays feedback; an inactive worker borrows and restores prewarming.
A short key-tone VAD guard retains PCM in preroll rather than erasing user audio.

While listening quietly, the capture producer keeps only its newest six PCM6 frames
(120ms) in the existing packet pool. It does not queue an endless silent stream
for the network worker. Speech onset publishes that prefix in order and then queues
every spoken frame without overwriting. A voiced backlog still has a finite bound
and reports audio backpressure explicitly; it never silently truncates a message.

| Resource | Bound |
| --- | --- |
| MCP request / response | 4096 / 2048 bytes |
| Tool result | 640 bytes |
| Transient PCM6 queue | 80 initial standby slots; promote to the original 120 slots / 2.4 seconds before upload |
| Complete editable draft | 511 bytes maximum; tool text also limited to 160 Unicode characters |
| Outgoing mentions | Two names; twenty user-list pages; fixed parser and streamed response |
| Undo | One optional bounded snapshot, released on context/session exit |
| Pagination URL | First page reuses its small base; encoded cursors allocate on demand |
| Sound record | Two-byte versioned payload plus FlashDB/mirror metadata |

While the assistant connection is active, background conversation refresh fetches
one latest-message preview per chat instead of eight full messages. List reads
defer full-body persistence; explicitly opening a message still loads its body on
demand. Before nested Feishu HTTPS tool calls, the assistant releases its Opus
reply decoder while retaining the audio codec and WebSocket session. Member-list
cursor URLs are allocated only for subsequent pages. These are allocation-policy
changes; the qualification records report observed peak-heap effects, not universal reserves.

Automated assistant diagnostics force both reply and feedback output volume to
zero while continuing to exercise decoding and audio writes. Audible quality and
real microphone acceptance remain separate checks.

These limits are not total heap measurements. TLS, Opus, LVGL, DMA and worker stacks
also count. Build/Host/Device results and remaining physical acceptance are recorded
separately in [the validation record](../development/feishu-validation.md). A passing
fixture does not establish physical speech, acoustic quality, long-term RF stability
or real power-loss behavior. No universal 20 KiB heap reserve is claimed.

Protocol references: [XiaoZhi WSS](https://github.com/78/xiaozhi-esp32/blob/main/docs/websocket.md),
[Feishu message content](https://open.feishu.cn/document/server-docs/im-v1/message-content-description/create_json).

XiaoZhi settings exposes inline volume editing from zero to one hundred. A four-byte
version-two record reuses the former reserved byte; version-one records preserve
boot/timing choices and load volume60. Reply volume is read dynamically. Startup
uses a breathing title/progress dots and the optional short electronic success tone.

Invisible capture time does not invalidate a business page. The capture task reads
a four-byte atomic effective-submit snapshot instead of taking the UI mutex on
every frame. Empty feedback requests return before locking; work polling uses a
nonblocking UI snapshot. Binary audio writes are bounded to two seconds, below
the existing2.4-second queue budget; failure closes rather than replaying a frame.


Assistant inactivity has a configurable timeout (default ten minutes; zero disables
expiry). Real recognized speech, reply text and ongoing speech/processing/playback renew
the clock. Silent transport pings and background refresh do not renew it. Expiry cancels assistant-owned capture and releases its microphone without
cancelling an independent send/dictation handoff. The four-byte version-three record
packs the timeout into the boot/timing record; earlier records retain their settings
and load the ten-minute default. The reply watchdog measures ninety seconds without
application payload progress, rather than total reply duration. Exit logs identify
deadline, handoff, cancellation, farewell, or error; a previous unrecorded exit cannot
be retrospectively assigned a cause.

The listening footer keeps the centered phrase stationary. A separate, preallocated
suffix label animates zero to three dots with the badge breathing cycle; it never
recenters the phrase or invalidates the whole business page. Other notices retain
their ordinary layout and full opacity.


The C3 has one CPU but uses separate RTOS tasks for LVGL/input, audio capture,
and business/network work. Reply decoding and Feishu work share the existing
business worker. Only a new user operation may preempt audio that is already
playing: send abort, release Opus and wait at most three seconds for its stop
acknowledgement. MCP-selected navigation belongs to the current reply; automatic
refresh and owner-deferred reads never interrupt it. Ordinary reads run with the
existing capture task and bounded queue alive, yielding to detected speech.
Explicit exclusive handoffs still release audio resources, and connection faults
use bounded recovery. No additional persistent worker stack is introduced.

The former 24 KiB private LVGL pool could exhaust independently of the system heap.
LVGL now uses the shared C allocator without reserving another pool. Configure-time,
source-contract-checked overrides make glyph scratch allocation failure return safely
and clear a retired scratch pointer. Other assertions remain enabled. A failed glyph
may be absent from one repaint; subsequent repaints retry it. Diagnostics inject
that failure while reply audio is active and check continued UI ticks and the actual
back-navigation handler. Allocation recovery does not guarantee survival of every
possible system-wide OOM at unrelated object-creation sites.


Voice composition opens the collaborative editor immediately, even with an empty
body. A complete draft for the same recipient is retained; a new empty editor uses
a disposable RAM slot. Empty content has a valid editing revision but cannot be
confirmed or sent. The model reads/edits that revision, then request_send opens
a confirmation preview without a POST. A stale revision cannot change the confirmation.
Confirmation keeps listening and sends a WebSocket ping every fifteen seconds.
A later-turn voice confirmation of the same revision, or physical OK,
hands the send command to the ordinary worker after WSS/codec cleanup; logical
assistant activation resumes afterwards. Recognition/reply events no longer clear
the working status into an artificial listening frame. The original reported
connection failure requires an actual exit/error trace before assigning its cause.


## Connection lifetime and truthful readiness

Logical activation is independent of an individual WSS connection. Transport
recovery retains the opened codec, current page and draft, discards old wire/parser
state, and renegotiates hello and the complete MCP tool listing. It does not replay
recorded speech, tools, or a Feishu POST. Completed activation greetings are not
repeated on a wire reconnect. User cancellation, farewell and the activation
lifetime still stop recovery. Two rapid transport retries are allowed; a genuinely
stable listening/response interval can replenish that budget.

Readiness requires the session hello, initialization, the final tools/list page
(without nextCursor), and the first successfully captured/encoded microphone frame.
Connecting, greeting and microphone preparation cannot map to the listening notice.
Mic priming uses elapsed codec-open time, accounting for prewarming. The inactivity clock is not renewed by transport recovery alone.

TLS peer certificates are released after normal handshake verification. Network
business work polls the WSS receive path so the SDK can answer control frames;
client-side PING alone is insufficient. Complete MCP requests are deferred until
the other HTTP call stack has unwound, with a bounded dispatch budget and no second
resident worker. Losing the voice socket does not cancel an unrelated Feishu read.

Night-time diagnostics persist volume zero and disable success/failure/key sounds,
then verify the stored preferences before testing. Ordinary delivery retains those
mute settings; original preferences are saved locally for an explicitly requested
restoration. Fixed-seed device scenarios distinguish cold and cache reads, real
input-task cancellation, draft confirmation, deliberate memory/wire faults, quiet
listening, and actual cloud MCP operations. Branch coverage does not establish
physical button debounce, acoustic quality, every media format, or real send delivery.


Activation and send recovery (R26): a single boolean remembers whether the greeting request was sent during the current activation. UI deactivation followed by a new activation clears it; a worker handoff, memory recovery or transport reconnect does not. Automatic resume uses the existing owner loop without restarting the activation timeout. The first connection sends a concise-answer hint through MCP initialize instructions; the wake-word field retains its original value. The client may ignore this hint, so it is not a strict length guarantee. No new audio pool or persistent worker is added. A send keeps recipient/body visible and shows progress in the footer while voice resources remain released. Silent maintenance has a paused-listening phase, not a fabricated command-processing phase. Three presses are supported by the existing button component's multiple-click counter, but no new shortcut is assigned in this change.


Continuous idle capture (R28): the existing capture task and fixed 120-frame queue remain alive across ordinary queued reads. Background timers no longer stop capture; battery I2C sampling runs while it continues. Passive list refresh is coalesced to 30 seconds while the assistant is active, keeping capture alive; explicit page changes and voice queries still fetch. A producer-published atomic speech-onset flag cancels a cancellable read through the existing HTTP progress hook, retaining the queued prefix for upload. The GET connection uses the SDK asynchronous open path under the assistant owner so TLS establishment can yield too. Interrupted list reads are deferred until after speech. No new audio pool or persistent worker is allocated. Exclusive send/dictation/auth/network handoffs, actual wire reconnection and turn-taking playback remain separate boundaries; this does not implement acoustic barge-in or unbounded offline recording. Native acceptance must overlap real HTTPS with injected speech and verify the same capture queue survives, then cloud recognition and recovery.


Deferred work and reply ownership: one coalesced read is owned by the active assistant visit, outside the ordinary UI command queue. It resumes with capture alive after the current reply; a new page supersedes stale work. MCP-selected navigation belongs to the current reply and cannot abort it as if it were a new user command. Only actual user work may interrupt audio that is already playing. Playback-end packets received while capture runs cannot finish a future command; completion is reset at stream start and before waiting for that command's reply. Outgoing audio renews the keepalive timer, avoiding redundant explicit PINGs during upload. Focus validation rejects unexpected reconnect/replay rather than counting a replayed fixture as success.

Tool completion (R39): open_chat loads the cached/current message window inside the MCP callback, before returning the tool result. It does not wait for completion speech. navigate loads the draft list before acknowledging it. The Opus decoder is already released before nested HTTPS; no new worker, stack, or audio pool is introduced. Ordinary background reads keep their capture-first/deferred policy. A missing open_composer recipient refers only to a currently open conversation/editor; a highlighted list row is not a recipient. Task acknowledgements are requested to be brief, while questions retain concise answers. This is a model instruction, not a guaranteed speech-length limit. Global takeover retires legacy dictation pages; normal explicit dictation remains supported. Loading stays within its business page, preserving already loaded rows only when they belong to that conversation.

R40 controls: request_send without confirmed previews the current immutable recipient/reply target and revision. Only a later recognized user turn may call confirmed=true with the same current revision; same-turn self-confirmation, stale/empty/uncertain drafts, duplicate sends and a full work queue are rejected. Confirmation keeps listening. Accepted sends use the existing WORK_SEND handoff, not nested POST during MCP; queued does not mean sent. Physical OK remains available. open_composer optionally accepts reply_to_message_id, validated against the selected conversation window; unknown/cross-chat targets never fall back to ordinary sends. Pending message identity reaches the existing reply API and survives draft editing. settings provides seven persisted controls: boot, silence_seconds, volume (including delta), timeout_minutes and independent success/failure/key sounds. Save errors preserve RAM, and concurrent local edits supersede the request. navigate supports XiaoZhi/assistant/pairing/Feishu account/network/sound pages; opening a page is not account reset or credential mutation. No new persistent task or audio queue is added.

R46 queue allocation: quiet assistant capture owns 80 PCM6 slots (1.6 s including preroll), enough to cover the one-second cancellable read step. Speech cancels the current read; before listen/start or socket writes, the existing worker allocates the remaining 40 slots, restoring the original 120-slot/2.4-second active backlog. The 4,960 payload bytes are not reserved beside cold HTTP/TLS while idle. Primary storage never moves; the extension is published atomically before its indices enter the existing queue. Dictation still allocates all 120 slots initially. Allocation failure exits through ordinary audio teardown; no new task, encoder, or wire format is introduced. Native acceptance must prove cold reads with live capture, promotion before upload, complete first-utterance recognition, no real TLS allocation failure and all recovery checks.


## Resource ownership and fragmentation

Keep allocation policy in the HTTP/audio/BSP adapters. Business pages do not borrow
buffers from one another. The current change preserves continuous standby capture
and UI interactions; it does not add a global allocator or a permanent scratch pool.

| Owner | Allocation lifetime and bound |
| --- | --- |
| Streamed GET | Reserve full TLS RX after handshake through response completion; cleanup releases it. WSS/POST keep their existing policy. |
| Opt-in HTTP client | Redirect-disabled zero-body GET only: defer TX scratch until after TLS, retain partial headers, wipe/release after completed headers. POST stays outside this path. |
| Message parser | Create the 6,984-byte workspace at the first body chunk, after TLS/header scratch; release after finish, cancel or error. Static RX remains owned throughout parsing. |
| Audio adapter | Stable 80-slot standby allocation plus atomically published 40-slot extension before upload; active PCM6 format/rate/backlog unchanged. |
| Display adapter | Original single 20-row/9,600-byte draw buffer; use the vendor 7,168-byte LVGL stack when built-in TJPGD is disabled, otherwise retain 12,288 bytes. |

The earlier parser-before-TLS workaround protected a dynamic RX hole. It is no
longer needed with a response-lifetime RX allocation: delaying the parser removes
6,984 bytes from the handshake overlap without introducing screen-specific rules.
Native qualification checks this ordering for each observed parser lifetime and
rejects real TLS OOM, watchdogs, unexpected reconnects and inadequate UI stack margin.

Potential future infrastructure, not implemented here:

- ESP-IDF already uses [TLSF](https://github.com/espressif/esp-idf/blob/v5.5.3/components/heap/multi_heap.c).
  Replacing general malloc alone does not establish a larger continuous free block.
- [Zephyr slabs](https://docs.zephyrproject.org/latest/kernel/memory_management/slabs.html)
  show fixed-size pools for repeated equal-size objects. They avoid external
  fragmentation within that pool but reserve capacity and may waste space inside blocks.
- [Mbed TLS allocator hooks and buffer allocator](https://mbed-tls.readthedocs.io/en/latest/kb/how-to/using-static-memory-instead-of-the-heap/)
  could isolate TLS allocation; the pool still must fit simultaneous connections,
  thread safety and failure recovery. Global hooks must not be swapped around live sessions.

Choose a pool only from measured live allocations and peak/largest-block evidence.
A passing finite suite does not establish immunity to fragmentation during arbitrary
network churn or unlimited runtime. See the R53 validation record for the accepted
build and the remaining physical acceptance boundaries.


## Detail reading and scheduler progress (candidate R61)

A selected original message ID remains available while its body loads, provided
its loaded conversation matches the current conversation. Opening a quoted reply
uses that same validated ID; it never waits for the whole body or substitutes a
normal message. The exact original indefinite-processing incident had no retained
trace, so this guard correction alone is not proof of its complete cause.

Speech-preempted full-body fetches join the same owned deferred-read mechanism as
message lists. A committed readable body is displayed before optional message-list
snapshot persistence. Fetch and local-page timings are logged separately, without
content or credentials. Long mapped-body processing polls the existing owner
progress hook for speech cancellation; no nested business tool runs from that hook.

FlashDB's RAM read window can bypass physical Flash reads. Scheduler cooperation
therefore runs before every logical read, including cache hits, through one callback
registered by the Flash adapter. It yields two ticks after a four-millisecond CPU
budget. The callback must not re-enter the DB or perform business operations; storage
serialization and CRC checks stay unchanged. This is a foundation-layer scheduling
rule, not a per-screen workaround or a disabled watchdog. Multi-chunk root reads
prefetch the exact key/slot/generation before checking every chunk and its CRC;
this reuses the existing bounded locator cache and does not bypass integrity checks.
The selected host fixture (7KiB,180 unrelated records) reduced reads from990 to322
with zero writes; this does not establish device latency.

R54 caught IDLE starvation after an actual HTTP401 during credential loading;
matched symbols locate CRC validation in read_kv. R55 adds explicit real401/refresh,
committed full-body read, four random 12KiB pages and the actual group's named-bot
lookup to the existing full native gate. See the validation record for qualification;
build success or these candidate descriptions do not establish device PASS.


### Error envelopes and async connection recovery

Message/member GET may opt into numeric root-code extraction for non-2xx responses.
Parse at most4,096 error bytes in a2,080-byte workspace allocated only for an error
and released at cleanup; never forward error content to message/media storage.
Only a complete well-typed known access-expiry code triggers the existing single
refresh policy. Duplicate/nested/wrong-type/truncated/oversized envelopes and unrelated
permission failures are not evidence to revoke binding; no POST is replayed.

The current ESP-IDF5.5.3 async path fills fd_sets in initialization, but does not
refill them after select clears them on timeout. Later polls can wait on empty sets
even after TCP connects. An upstream [issue19378](https://github.com/espressif/esp-idf/issues/19378)
reports this mechanism. Configure-time original-source SHA256 checks generate a
project-local esp_tls.c override: re-arm every poll, retry EINTR and explicitly fail
other select errors. The shared SDK, thirty-second deadline and speech cancellation
remain intact.

Host tests reproduce the old cleared-set dead end and cover forty timeouts followed
by readiness, EINTR and hard-error recovery. The probe clears sets once on a real
connection and requires subsequent recovery; ordinary firmware contains no injection
string. Both variants retain their actual generated source and original SDK copy.
Unsupported source hashes stop configuration instead of silently applying a stale patch.

## Visible-page refresh and startup handoff (R69)

The visible latest message page polls one ephemeral preview every2s when the normal
worker is idle, or3s while the assistant owns it. Unchanged previews cause no
message-cache writes. New IDs are published before full-page reconciliation;
selection stays on the same row where possible. Older preview results, duplicates
and deleted rows are not appended. Full reconciliation still runs on change or
about30s to catch gaps, edits and deletion; it defers full text-body storage, so
opening long content retains the existing on-demand body path. These are polling
intervals, not a guarantee that every network request finishes within3s.

Conversation enrichment uses only latest-message previews in both ownership modes;
assistant conversation-list refresh uses8s instead of30s. It remains a rotating
batch, not push delivery to every conversation simultaneously.

Both startup owners use the same flash-resident ASCII artwork. The application
has a temporary full-screen overlay until cached conversations or initialization
completion/error, then deletes it. Cached content is published before draft
indexing; background connection text does not replace a populated list. The
initialization retry/error path remains accessible rather than hiding indefinitely.

A pending send still releases the assistant owner for Feishu HTTPS. After it
finishes, an enabled assistant resumes ahead of a sole optional history-refresh
command. That command remains queued and is serviced after capture is ready.
This reduces the pause but does not promise the WebSocket remained connected.

## Authorization reentry (R72)

Settings authorization still durably records the requested operation and restarts
so the old voice/network owner is fully released. The UI now cancels optional
reads on this action and shows a single BOT-authorization preparation message.
After the durable write succeeds, an8-byte RTC-noinit cookie marks this specific
software restart. It contains no account data and is not an authorization grant.
The next onboarding entry consumes it once; software reset, tag and complement
must all match. Cold/unexpected resets and damaged cookies keep ordinary startup.
Durable auth state is still read and verified before any authorization request.

Authorization reentry skips the boot chime and decorative minimum delay, starts
on the preparation page, and proceeds to the QR without the cold-start logo.
An already-started storage transaction must still finish safely; network waits
remain possible. This does not promise a zero-delay or reboot-free handoff.

## Preview-first cold loads and body cache validity (R74)

Initial message-list loads and older-page network fetches explicitly use the
preview-page API. It preserves server pagination and bounded text previews but
does not install body compression/storage callbacks. Full-message retrieval still
uses those callbacks when content is opened. Plain complete short text may render
directly; unresolved mention mappings require full retrieval.

The message model retains the server update_time field documented in the official
[message-list response](https://open.feishu.cn/document/uAjLw4CM/ukTMukTMukTM/reference/im-v1/message/list).
A cached body reference is reused only inside the caller-validated account/chat
scope and when ID, creation time, nonzero update time and text type match, with
neither row deleted or unavailable. Missing, malformed or duplicate revision data
disables reuse rather than trusting a possibly edited body.

A full-text fetch displays the committed body first, then optionally updates its
reference in the framed page snapshot read by normal list-cache lookup. It no
longer writes only the legacy messages/chat key. Persistence uses the captured
scope version; cache failure never hides an already-readable body. Old disposable
message snapshots naturally miss their unit-size check after the model change;
credentials and draft formats do not migrate.

At the last conversation row, short Down reuses the existing cursor/cache/network
page workers if has_more is true. At the final page it reports the boundary.
Repeated presses while loading do not enqueue another page request. Existing
long-press paging remains available.


## Power policy (0.2.1-r80)

Power decisions use meaningful user activity, not background polling or transport keepalives. Physical input, recognized speech/replies and actual speech upload renew activity. Auto policies use a fixed 120-second threshold; the display timeout is independently adjustable.

| Setting | Values | Default | Automatic behavior |
| --- | --- | --- | --- |
| Idle screen off | 2–10 minutes | 2 minutes | Backlight off; the first wake gesture and its trailing double-press event are consumed |
| CPU saving | Off / On / Auto | Auto | Allow DFS and automatic Light-sleep after 120 seconds, subject to work/audio locks |
| Refresh | Foreground only / Background / Auto | Foreground only | Auto stops periodic reads after 120 seconds; page entry and explicit commands still refresh |
| Wi-Fi saving | Off / On / Auto | Auto | Use MIN_MODEM after 120 seconds without reconnecting |
| Microphone prewarm | Fixed | 60 seconds | Release codec/DMA/task and return the reserved cache-worker resources; passive refresh cannot rearm it |

The application initializes power management once (160/40 MHz, automatic Light-sleep enabled) and switches existing locks instead of repeatedly reconfiguring clocks or restarting Wi-Fi. Two PM locks keep audio capture/playback and active business operations responsive. Silent listening still owns audio and therefore does not permit CPU Light-sleep; screen and Wi-Fi saving remain independent. Actual speech restores the interactive network profile before PCM upload, while pings do not reset idle time. This follows the SDK's [power-lock mechanism](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-reference/system/power_management.html) and [station modem-sleep API](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32c3/api-guides/wifi.html#esp32-c3-wi-fi-power-saving-mode).

There is no new application worker. The existing UI timer evaluates policy and changes display/PM locks; the existing business/assistant worker applies Wi-Fi changes only at state edges, with five-second error backoff. GUI ticks are 20ms rather than 5ms. Timers, USB debugging and driver locks can still limit actual sleep duration; enabling the policy is not an electrical measurement.

Settings use a versioned five-byte ROOT_POWER record appended after existing root keys. Missing/corrupt records select safe defaults and never reset the account or Wi-Fi book. UI editing follows the existing select/edit/save pattern; a failed write reports that the in-memory choice is temporary.

Foreground mode keeps cached rows readable and refreshes when entering a conversation/list, including returning from details. Existing periodic refresh paths in both the business and assistant loops obey the same policy. Background traffic does not wake the screen or renew user activity. A prewarm timeout remains latched until the focus is released, including when feedback tones run.
