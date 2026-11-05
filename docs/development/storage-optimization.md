[简体中文](storage-optimization.zh_CN.md) · **English**

# Overall experience optimization implementation

The maintained plan and 25-case normal/stress/abnormal matrix are in the
[Feishu document](https://my.feishu.cn/docx/T902dXr8koRDJaxwhnDcMf0hnZf).
This is an overall experience task. SQLite is the first storage candidate;
FlashDB is a formal comparison candidate, not an excluded choice.

Keep the firmware address and identity region unchanged. All application data
shares one 1 MiB pool; quotas are limits rather than preallocated regions.
Required credentials, pending drafts and active readers survive cache eviction.
Every fault case must restore the main workflows after its fault is removed.

## Current status (2026-11-03)

The shared pool, compressed caches, long-message send/receive and mentions, global drafts, conversation/history pagination, JPEG preview/zoom/pan, and database isolation/recovery are integrated. One normal emulator image passes 62 checks; two separate fault fixtures pass three checks each. Repeated physical storage recovery passes. The current formal APP is 2721344 bytes; its merged image is 2786880 bytes and is installed on COM7 with a clean silent-startup observation.

Complete hardware acceptance remains unfinished: real OAuth/TLS/ASR, panel/buttons and whole-device resource peaks require device configuration first. The final checklist below records all 25 cases and implementation limits. There is no separate enterprise directory or received-audio playback; non-JPEG formats degrade explicitly. The 8 KiB generic write-batch candidate was rejected after native pressure timeout; the product uses 1 KiB transactions. Earlier checkpoints below preserve historical pending states and sizes.

P0: plan created and read back. P1: isolated storage qualification in progress.
SQLite 3.54.1 source archive SHA3-256 is
`92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`.
The generic probe is `tools/storage_probe/sqlite_workload.c`; it tests commit,
rollback, bounded allocation, full-database reclamation, pinned data and reopen.
It does not establish end-to-end message/audio recovery or hardware durability.
The working application now uses snapshot caches and streaming long-text reception;
configuration and send flows have not all migrated.

## Working increment, 2026-11-02

The isolated C3 SQLite probe passed all three 32/48/64 KiB budgets: 300 synthetic
2 KiB rows, `SQLITE_FULL`, bounded eviction, pinned-record preservation, forced
`SQLITE_NOMEM`, reopen and a successful subsequent write. SQLite's own measured
peak was 24,760 bytes; this excludes BSP, radio, TLS, UI, filesystem and task stacks.
Evidence: `build/storage-probe-report/20261102T081558Z/summary.json`.

FlashDB revision `d186ba950b14f9c731d87075c627e19837213aa9` passed the raw Flash/FAL
C3 probe with 273 rows and recovery after full storage. Its file-mode C3 probe
failed with a read error and remains failed. The host NOR model passed 24 injected
write/partial-write recovery cases. Neither a host NOR model nor orderly reopen
proves physical power-loss durability. Other FlashDB write granularities and the
upstream BSP/QEMU suite have not been verified.

The common codec has a fixed 1 KiB block and no heap allocation. It checks lengths
and CRC, rejects expansion past the declared size, and stores raw bytes when
compression is not smaller. The object layer stages immutable generations,
publishes replacements transactionally, pins active versions, and reclaims one
chunk per transaction. Host cases cover normal use, cancellation/reopen during
replacement, category/physical capacity, corruption and allocation failure.
The complete static gate passed after these tests were added.

Conversation/message snapshots and offline startup are integrated in the working
version. A 120,026-byte message was parsed, compressed and stored in fixed 1 KiB
blocks, with 469 reading pages. Emulator checks passed restart without an AP,
resumption at page 4 and offline access to the final page. Evidence:
`build/full-text-report/20261102T115225Z/summary.json`, image SHA-256
`8aff2a0c36d8ea30bfede80d66f5a3089c3b2d35d724d8069d585d1534f72251`.
Fixed windows do not grow with message length; synthetic HTTP heap measurements
do not establish the real TLS peak.

The first cache product build was 2,643,072 application bytes, an intermediate
artifact. Long-text sending, complete mention
replacement, drafts/outbox, image/audio integration, final normal/stress/abnormal
recovery checks and hardware acceptance remain pending.

After streaming OAuth parsing, image
`ee6395cc51a9196fd91f876a8b9987b27042994e83fabbda3eac0c609b87653e`
passed ten baseline/auth-fault checks and five cache checks. Truncated JSON,
nested revocation errors and partial new tokens in HTTP 500 preserve the binding
and recover after fault removal. Cache checks also reject offline recording
immediately without executing it later. Evidence:
`build/simulator-report/20261102T124422438567Z/summary.json` and
`build/cache-simulator-report/20261102T124655Z/summary.json`. The latest static
gate is `build/gate-storage-static6.log`; this increment has not completed the
full product firmware gate or hardware acceptance.

## Shared configuration pool increment

Configuration, tokens, cache ownership, seen state, provisioning flags and Wi-Fi
configuration now use the same 1 MiB `/feishu` filesystem. Small recovery records
have mirrored copies, generation numbers, lengths and CRCs, with no new partition
or value-sized workspace. SQLite still manages business caches. Recovery records
remain readable independently of a damaged disposable database. Cache writes leave
a 48 KiB working margin for necessary updates and journals; necessary writes can
use it. This is not a preallocated configuration region.

Imports are read back before deleting named legacy application keys, without
erasing NVS, identity or calibration partitions. A persistent migration marker
prevents old-token resurrection after unbinding. Access tokens and cache ownership
are tied to binding epochs. Wi-Fi restores from the pool into RAM storage, clearing
the SDK's old STA copy only after verifying the replacement. Host checks cover
7.5/8 KiB token import/rotation, account isolation, revocation, reboot after failed
legacy cleanup, database-independent recovery and monotonic seen state. Physical
power loss and actual SDK Wi-Fi migration still require device validation.

The mirror layer passes twenty partial/full-write fault positions, first-create
retry, single-copy damage, redundant-write failure, sync failure recovery, bounded
reads and refusal to overwrite two damaged copies automatically. Incremental DB
vacuum returns over 100 KiB of actual file space after eviction while preserving
required records and subsequent writes.

Shared-pool emulator image
`aba562d993d0ac163cfb965474c1f9654f49bfeb868b8ac2172265eda19c77c5`
passes ten baseline/auth checks, five cache checks and three 120 KB reading checks.
Evidence: `build/simulator-report/20261102T131440336972Z`,
`build/cache-simulator-report/20261102T131455Z`, and
`build/full-text-report/20261102T131505Z`. The fixture imports actual legacy NVS
formats and checks logical deletion of the old app secret.

The complete product gate `build/gate-config-product2.log` passes. APP is
2,670,080 bytes; merged BIN is 2,735,616 bytes. Matching ELF/MAP/configuration
are retained in `build/config-product-verified`. This is a configuration-stage
checkpoint, not final overall delivery. Outgoing double-JSON chunk encoding has
a 300 KiB independent decoding round-trip host check; ASR/outbox/product integration
is not yet implemented.

## Simulator Wi-Fi compatibility

PassportSim 1.0.3 binds Wi-Fi high-level emulation (HLE) using known ESP-IDF
instruction fingerprints, redirecting recognized calls to virtual APs and the
host HTTP bridge. It does not completely emulate the physical radio. Application
changes can affect the SDK's final instructions through optimization and linking,
so an application edit can prevent Wi-Fi binding without changing Wi-Fi logic.

SQLite's mutable static tables changed the GP-relative addressing range in this
build. One `lw` became `lui` plus `lw`, increasing a recognized function from 54
to 58 bytes and breaking its fingerprint. That diagnoses simulator binding,
not physical-device Wi-Fi behavior. The linker layout now preserves the SDK GP
access pattern, with SQLite's static region explicitly excluded from the heap.
`tools/verify_firmware.py` checks both exclusion and initialized BIN contents so
the compatibility fix cannot silently allow heap corruption. Firmware partition
addresses are unchanged.

Reports retain exact BIN/ELF hashes and HLE binding receipts. SDK or compiler
changes require new verification. A durable emulator improvement belongs in its
fingerprint mechanism and supported SDK build set. Radio, real TLS and physical
reconnection still require independent device acceptance.

## Sending and scheduling checkpoint

Long ASR, persistent drafts and a four-slot outbox are integrated in the working
application. Body and metadata publish atomically. Intent precedes POST and only
acknowledged parts advance the offset. Unknown outcomes never auto-replay; warned
explicit retries retain per-part UUIDs. Double-JSON requests are at most 128 KiB
and preserve UTF-8 scalars. Official deduplication lasts one hour, not indefinitely.

Image `18c94655d4abe31b3744ec5b073cb15388368e61ac8306859088c94a9fae5b3c`
passes six checks in `build/outbox-simulator-report/20261102T140157Z`: 180,029
ASR bytes/704 pages, offline restart/final page, two actual sends with combined
hash verification and large ACKs, lost ACK/reboot/explicit retry, API rejection,
and main-flow recovery after late ASR error. The earlier run remains FAIL after
five passing checks due to a pending synthetic microphone timeline. The fixed
script was rerun in full without overwriting that evidence.

Image `e7afe716f36870d91510a1018a970714b0476d240b9e7441550e2e2dc58fcdf6`
passes ten baseline/auth checks in `build/simulator-report/20261102T143448152168Z`
and three cancellation checks in `build/priority-simulator-report/20261102T143424Z`.
Recording starts in about 0.86 seconds behind a held background response;
cancelling held ASR returns to the list in about 1.95 seconds, followed by a
normal record/review/cancel with no sends. These are synthetic HTTP guest timings.

Subsequent fixes make chunk INSERT/size UPDATE atomic to prevent retry duplication,
reclaim only staging outside active writer leases, and use fixed independent
reader leases with recoverable failed releases and no per-redraw pin writes.
`build/gate-storage-static14.log` passes; the latest changes still need new target
regression. The JPEG viewport/crop host prototype passes below 14,000 workspace
bytes excluding network/DB/UI, without a whole-image bitmap. Product image cache/
display integration is being implemented and is not accepted yet.

Voice retains the agreed fixed PCM ring and persists text drafts, without whole
PCM retention absent a playback requirement. Remaining work includes full mention
mapping, list paging/window stress, draft management, final comparable memory/size
and recovery matrix, real-service and device acceptance.


## Image/display and first device checkpoint

The complete gate passes in `build/gate-media-product1.log`. The retained real-service
product set is `build/media-product-verified`: application 2,692,592 bytes, merged
image 2,758,128 bytes, merged SHA256
`6ee647e6af148254f0a1f2129f6414e4b9b9836d1724d44b0358485ffcdba45b`.
It includes bounded JPEG scanline previews, shared image caching and independent
reader leases. Preview RGB565 blocks reuse lossless block compression; compressed
JPEG originals do not undergo redundant compression. Unchanged labels no longer
allocate and invalidate their display area on each 100 ms timer tick.

Simulator image `03823fe05b127c451609802e48b88b30787f2ce29f5c2782c666e91725c519b2`
passes fitted preview and zoom/both pan-axis pixel checks without another HTTP
resource request. `build/media-simulator-report/20261102T175302Z` remains FAIL:
the offline latency assertion included an unnecessary second UI query. Its first
ready observation was 1.43 seconds; the corrected whole script is being rerun.
Earlier stale-frame failures are retained, and the image suite is not accepted yet.
UI-tree readiness and actual SPI framebuffer readiness are separate observations.

The user authorized device flashing and disposal of application data. COM7 was
confirmed as ESP32-C3 with 8 MB flash. The verified product image was flashed and
esptool reported write-hash verification. Only the old application data region
0x360000..0x46ffff was explicitly cleared; the identity/calibration window was
preserved. A bounded 35-second boot observation reports Button/Audio/Battery
initialization success and no detected panic/assert/watchdog. This is boot-log
evidence, not physical screen/audio or real Feishu acceptance. The onboarding AP
was not confirmed in that window. Sanitized evidence: `build/device-checkpoint/`.

Until the user is available after 10:00, testing stays silent and independent;
physical speaker tests and any phone/QR/button cooperation remain pending.


## Physical storage performance follow-up

Simulator `03823fe05b127c451609802e48b88b30787f2ce29f5c2782c666e91725c519b2` passes
ten baseline/auth cases (`build/simulator-report/20261102T180022762025Z`) and seven
media cases (`build/media-simulator-report/20261102T180404Z`), including a
416,456-byte original, 1.68-second offline first observation and post-fault send.

The silent physical codec/cache baseline `be5181ee...` FAIL is retained under
`build/device-probe-report/baseline-be5181ee3de9d0c8`: first preview 15.48 s,
local views 28.95--30.83 s, new view after a bad JPEG timed out. Matching baseline
BIN/ELF/MAP are preserved; its full source hash manifest was not captured before
staging the next revision. Configuration was unchanged.

Grouped 1 KiB transactions and bounded coalesced row output passed host rollback/
retry tests in `build/gate-storage-static20.log`. Reusing the inactive original
tee buffer avoids duplicate SQL reads without an additional read buffer. PERSIST
with a 32 KiB retained-journal cap and EXCLUSIVE single-connection locking retain
synchronous=FULL; selected-mode host checks pass in `build/gate-storage-static21.log`.
See [SQLite PRAGMAs](https://www.sqlite.org/pragma.html) for the configuration;
physical power-loss safety still depends on the VFS/filesystem and is unverified.

Physical probe `5b5e53c91716f7c84ceae384d19becd76a76d23d51ac64489cd15ad6aa1c7636`
passes six steps: first preview 4.14 s, zoom 12.79 s, pan 14.94 s, hit 0.19 s,
invalid format rejection and a new normal view after that fault in 16.87 s.
Matching artifacts/results/source hashes are in `build/device-probe-report/<hash>`.
Workspace is 12,084 bytes, below the existing 14,000-byte bound; actual usage in
some modes increased, so this is not zero added memory. Network/display/audio
were not initialized, so these are not whole-application peak measurements.

Each probe restored product image `6ee647e6...cdba45b` and cleared application
storage. Current source changes still require a new complete product build and
regression. Next: real reset during a transaction (not electrical power loss),
and improve immediate navigation feedback while a detailed image view is built.


## Upcoming mention handling

Keep the compressed raw body immutable. Store mention lookups and sparse logical
byte checkpoints in the shared database; resolve names while reading a viewport.
This avoids a second full-body copy and its temporary double quota. Mapping must
work when metadata precedes or follows the body, for more than eight mentions,
across token/block/page boundaries, and after offline reopen. Exact token matching
must not replace a prefix of a longer token. Late response failure must publish
neither a body nor its mapping. If mapping is invalid/unavailable, preserve the
raw body and original mention markers rather than lose the message. This is the
next implementation plan, not completed code.


### Mention mapping and silent physical storage verification (2026-11-03)

The compressed raw body is stored once. Mention dictionaries and sparse offsets share the metadata quota; names are rendered per byte window. Metadata may precede or follow the body. More than eight mentions, boundary reads and offline reopen are supported. Mapping failures preserve literal body tokens. Late errors, cancellation and account-scope changes never publish partial data. Protected outgoing drafts always use literal reads.

The actual production SAX/store/cache pipeline passes 42 host cases. The storage suite covers 32 names, numeric names, CRC corruption, atomic publication failure and quota reclamation. Runtime SQLite pages are 512 bytes with explicit indexes; its hard heap remains 32 KiB.

Silent physical probe PASS: application SHA256 bac162b528f8acfe57bc69e27e72244ae3463afe910bbb4c3599540450d02806; report `build/device-text-report/20261102T232056Z-bac162b528f8acfe/summary.json`. Indexed windows, reopen, corruption, transaction rollback, the full 128 KiB metadata quota, reclamation, subsequent writes and protected records passed. SQLite peak was 23152 bytes; the 12 KiB probe task retained at least 4752 stack bytes. Reference arrays are test-only. No network, display or audio was initialized. Formal image de28b10e955c9ccfd59fc2ca86df45c844b65756d9fedde324e09e43c58f2cd9 was restored.

Emulator SHA256 7061f0bd32b4e98eb428454fb04519657bf921743a20f2f9c4ddd3163b8c2fff passes three checks for 32 mentions, 68 pages, offline page-four restoration and tail access (`build/mentions-report/20261102T231856Z`), plus five cache checks (`build/cache-simulator-report/20261102T232054Z`). Remaining whole-application regressions are ongoing; older image results do not establish this image's acceptance.

Previous native watchdog/timeout reports remain preserved. Cooperative yielding, bounded metadata accounting and representative mixed-cache pressure now pass without disabling watchdog protection. Real networking, screen/buttons and complete resource acceptance remain unverified. List pagination, a global draft manager and whole-database corruption recovery remain pending.


### Conversation pagination and cache pressure

Conversation lists now use fixed ten-row windows, compressed page caches, cursor navigation, offline hits and cancellable loading. Cursors are capped at 512 bytes and percent encoded; invalid cursors/envelopes retain the displayed rows. The official SDK defines the pagination fields.

Emulator image 679c1911da2dbce2e581fad38483e9bac0ec129d773cf3999117bea7d8750d3e passed six paging, ten baseline/auth and nine image checks. Traversing 200 groups over 20 pages changed free heap from 92284 to 89720 bytes. Cached image display was 1.43 seconds; quick-pan framebuffer latency was 3.029 seconds. Reports: `build/page-simulator-report/20261103T000013Z`, `build/simulator-report/20261103T000024175301Z`, `build/media-simulator-report/20261103T000538Z`.

The actual host cache adapter passed 500 random page windows, admitting 2119491 compressed bytes cumulatively while retaining the 128 KiB metadata cap. Active pages and pending drafts survive; cold pages are reclaimed; a mid-write failure rolls back and normal writes recover. Evidence: `build/gate-page-static3.log`. This does not establish physical fault-injection acceptance.

Silent physical probe 615178028ffe2f382752fbdb759c04ef3ef95fc87ad67c7141281134b6c0eaf5 passed 100 random windows, active-page/draft protection, cold eviction, reopen and subsequent normal writes. SQLite peak was 22728 bytes; the 12 KiB task retained 5560 stack bytes. No fault marker occurred. Formal image 0c69f721c81e551d879b684d119027ef489edd02f35a614c1aa8be74cc729998 was restored. Report: `build/device-page-report/20261103T001728Z-615178028ffe2f38`. Network, display and audio were not initialized; reference arrays are test-only.

A 45-second offline retry reproduced cursor-reset failure on image 623e97c790b163f6f02136cacca20413da01ddeb9e1326442017721fe32b870c (`build/page-retry-report/20261103T001703Z`). Image a67d7430c7c4dc1faf947389175db7d438794de084318cb77473fec903d5a94d passes that exact scenario (`build/page-retry-report/20261103T001714Z`), but its baseline suite exposed a first-request failure-status regression. That issue has been corrected and is being retested; this image is not fully accepted.

Bounded grouped cache writes, message-list pagination, whole-database recovery, final resource/fault acceptance and real networking/UI acceptance remain in progress or pending. Physical tests remain silent; no user cooperation is requested before 10:00.


[Official SDK pagination fields](https://raw.githubusercontent.com/larksuite/oapi-sdk-python/v2_main/lark_oapi/api/im/v1/model/list_chat_response_body.py)


## Current acceptance checklist (2026-11-03)

Major software features are integrated. A single normal emulator image passes 62 checks; two separate fault fixtures pass three checks each. The 173 filesystem-state cases, actual cache adapter, static checks and firmware gate pass. Repeated physical DB recovery passes. These do not complete real authorization, networking/ASR, panel/buttons or whole-device TLS resource acceptance.

Audio remains streaming input through fixed PCM buffers plus compressed transcript drafts; received-audio playback was not added. Images currently use bounded JPEG decoding with explicit unsupported-format fallback. Retained failed databases/journals share the same pool and temporarily reduce cache capacity. Old drafts are marked unavailable pending recovery, not guaranteed recoverable from arbitrary corruption.

| Case | Current result |
| --- | --- |
| N01 | Startup/config/auth: simulator passed; real authorization pending |
| N02 | Conversation/group/message caches passed; no separate enterprise directory |
| N03 | Host integrity and simulator paging passed; real messages pending |
| N04 | Active-reader protection passed; full real edit/recall/rename flow pending |
| N05 | Synthetic capture/draft/recovery passed; real capture/ASR pending |
| N06 | Synthetic send/dedup/uncertain-state checks passed; real recipient audit pending |
| N07 | JPEG emulator/native codec checks passed; panel pending; unsupported formats degrade explicitly |
| N08 | Host migration/config faults and device boot/reset/layout passed |
| S01 | Storage pressure passed; combined whole-device workload pending |
| S02 | Bounded blocks, raw fallback and long-content checks passed; real limits pending |
| S03 | 500 windows/2119491 stored bytes, 100 native windows and 200-group/80-message UI checks passed |
| S04 | Simulator scheduling/queue/cancel passed; physical button stress pending |
| S05 | Pending; isolated SQL and synthetic HTTP do not establish this check |
| S06 | Finite host/native loops passed; extended whole-device soak not run |
| A01 | Synthetic outages/timeouts/5xx passed; real DNS/TLS pending |
| A02 | Synthetic revocation/malformed grants passed; real authorization pending |
| A03 | Synthetic resource/history 403 recovery passed; real permissions pending |
| A04 | Host streaming protocol and simulator malformed-response recovery passed |
| A05 | Quota/full/protection checks passed; whole-device FS-full pending |
| A06 | Host injection/classification passed; real media write failure not induced |
| A07 | SQL hard-limit recovery passed; whole-device fragmentation pending |
| A08 | Host partial writes and physical transaction reset passed; electrical cut not tested |
| A09 | CRC isolation and repeated DB recovery passed; mount/media faults never blindly format the pool |
| A10 | Synthetic cancel and active-operation corruption passed; physical buttons pending |
| A11 | Format/size fault recovery passed; physical decode-timeout trigger pending |


## Corrupt filesystem references and configuration-preserving recovery (2026-11-04)

Device open failures were SQLITE_IOERR_READ (extended 266), not evidence of missing drafts. The encrypted 1 MiB snapshot contained two inconsistent SPIFFS references inside objects.db; one addressed a deleted page and one the wrong data span. Offline SQLite also rejected the extracted database. No original plaintext was written to the host filesystem.

The recovery read fresh device state immediately before writing, verified nine configuration keys with two CRC-valid mirrors each, and rebuilt the pool preserving all 18 non-database files byte-for-byte. Only objects.db and its journal were excluded; a complete encrypted original remains restorable. The rebuilt pool and identity window passed readback. The physical device retained binding: initial empty-slot scan including new DB creation was 659 ms, subsequent scan 0 ms with no unreadable slots. Chat requests succeeded. This recovers the current failure; prevention of repeated SPIFFS corruption and near-full performance remain unresolved.

The current FlashDB raw-NOR host rerun passed full/eviction/reopen and 24 failed/partial-write recovery cases. That supports a backend candidate, not product migration, native timings, or physical power-cut safety. The next comparison must cover app object commits, required drafts, quotas, compression and fault recovery on the same device budget.

## SPIFFS net-gain recovery

The encrypted incident snapshot reproduces the failure with the exact SDK SPIFFS source in a RAM-backed NOR model. There are 1,076 deleted pages but only 29 free pages. The upstream candidate heuristic repeatedly chooses blocks whose deleted-page gain is consumed by extra object-index rewrites. Its no-progress guard then returns FULL. SQLite startup also needs to clear a stale pin, so avoiding redundant vacuum settings alone cannot reopen this particular database.

A local `components/spiffs` override changes only candidate ranking under fewer than 128 usable free pages, for the board's 15-entry block geometry. It subtracts extra index rewrites from the expected page gain; normal-space wear-leveling and the existing collector/move ordering remain unchanged. The other implementation files and configuration come from ESP-IDF. CMake checks the original GC source hash before applying the override; SDK changes require review. Scratch is bounded to 15 groups (90 bytes), with no dynamic allocation or format migration.

On the same RAM-replayed incident pool, journal creation succeeds, all 21 existing files remain byte-for-byte equal, and SQLite quick_check passes. This is a host model result, not physical timing or power-cut acceptance. The public host case tests pressure ranking, unchanged normal ranking and read failure without writes. Physical acceptance and remaining capacity/latency limits are recorded with the exact firmware report.
