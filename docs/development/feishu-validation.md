[简体中文](feishu-validation.zh_CN.md) · **English**

# Feishu reliability and validation

Independent checkout: `apps/feishu`, branch `feature/feishu-reliability`, based on
upstream `c101494ff9031e29cf147923f434edea5d6c71cb`. Scope: standalone /26 reliability,
image size, PassportSim integration and subsequent authorized device acceptance.
This does not extend the ZION Runtime experiment.

## Provisioning cancellation, status refresh and app registration (2026-11-04)

The device had reached Feishu app preparation after Wi-Fi submission. The live
endpoint returned a 514-byte device code, exceeding the old 256-byte capacity;
same-page status changes were also not redrawn. The new bound is 1,024 bytes and
poll forms use their exact encoded allocation instead of a larger stack array.

| Validation | Result |
| --- | --- |
| Host protocol regression | 514-byte and 1,023-byte cases, over-limit rejection, worst-case escaping and existing negative cases pass |
| Native API cancellation/recovery | Wi-Fi cancelled in 154 ms then reconnected; HTTPS cancellation followed by successful registration of a 514-byte code and normal polling |
| Formal product boot | App-selection phase at about 5.17 seconds; no fault markers in 40 seconds; identity readback equal; APP 3,222,800 bytes |
| Human acceptance pending | Physical long-press back to open Wi-Fi setup, resubmission back to app QR, app selection/creation and user authorization |

The API probe used a 16 KiB task without the complete UI; its timing/heap does
not establish full-product metrics. The product retains 2 bpp text, an open
hotspot, existing binding and submitted Wi-Fi. Evidence:
`build/onboarding-cancel-product-verified/report.json`. Storage pressure remains open.

## 2 bpp font and manual Wi-Fi setup (2026-11-03)

The first Wi-Fi screen shows the password-free device hotspot and browser address
directly, without a QR code. Feishu app selection/creation and user authorization
retain QR codes. Source Han Sans Normal 2.005 uses 14 px, compressed 2 bpp grayscale,
17 px line height and a 4 px baseline.

| Item | This build |
| --- | --- |
| Coverage | Same 21,421 codepoints; complete U+4E00–U+9FFF. The pinned source's 1 bpp control matches the original bitmap byte for byte |
| APP / merged BIN | 3,220,864 / 3,286,400 bytes; APP grows by 490,016 bytes |
| Layout | APP remains at 0x10000 with capacity 0x330000; identity and the 1 MiB pool stay in place; 121,472 bytes remain |
| Memory | ELF static BSS grows by 16 bytes; runtime drawing peak not separately measured |
| Validation | Complete build/host gate passed; matching table and APP flashed separately, identity readback equal, 40-second boot without fault markers; human visual and full onboarding acceptance pending |

Pinned font provenance, gate log, matching BIN/ELF/MAP and device results are in
`build/font2-product-verified/`. The larger APP requires its matching partition
table; application-only flashing onto the old 3 MiB table is incompatible. Current
Wi-Fi/binding progress was retained. Previous storage pressure issues remain open.

## Journal truncation fix and filesystem candidates (2026-11-03)

The product change only flushes SQLite VFS buffers before truncation. On the board,
the original case requested 1,024 bytes but close regrew the file to 2,560 bytes;
the fixed case kept 1,024 bytes and matched the content. The regression source is
`tools/storage_probe/vfs_contract.c`. See this increment's report for the complete
product gate and final device restoration.

| Comparison | Observed result | Acceptance boundary |
| --- | --- | --- |
| Redundant SQL mark candidate | Row updates 9 to 1, but both had 45 underlying writes and took 663/662 ms | No measured benefit; reverted |
| Faulted SPIFFS pool | 627,500 bytes used; 2,500 live, 1,307 deleted and 33 blank pages | ENOSPC remains unresolved; encrypted snapshot retained |
| Initial SQLite / LittleFS | Corruption on the ninth object; unsynced fstat=0 versus descriptor end=512 | EOF/size and truncation changes stayed in the isolated project |
| Retest SQLite / LittleFS | No corruption at the same point after the read fix, but preparation took 191.665 seconds | Budget exceeded before target pressure rounds; not adopted |
| Main-path recovery after timeout | Old body readable, normal data writable, exact read after reopen; 8.598 seconds | Candidate storage recovery only; no UI/ASR acceptance |

The experiment used only the existing 1 MiB pool at `0x370000`. The original SPIFFS
pool was restored from DPAPI in RAM and the complete readback matched; Wi-Fi and
authorization settings survived. LittleFS was not adopted, and FlashDB still needs
measurement. Report: `build/vfs-investigation/report.json`. Pressure performance is
still FAIL and overall P5 remains incomplete.

## Current-group native interop and pressure recovery (2026-11-03)

The user explicitly reconfirmed a personal terminal using THEIR identity; neo is the
authorizing app. Only the current group was authorized. Tests were silent, without a
simulator or desktop control. Temporary native fixtures used the production modules,
a16KiB test stack and no full UI; these are not full-product UI or memory acceptance.

Normal API checks passed: exact Chinese/quote/backslash/newline roundtrip, quoted reply,
known-ACK UUID deduplication, cancellation followed by successful read, and invalid-empty
send followed by valid send. Independent group history confirmed the user's sender identity.
Two earlier fixture errors (PowerShell input encoding and an invalid slash in a draft name)
were excluded from PASS; production validation was not weakened.

A12,288-byte RAM control and a Flash-streamed message each matched every byte in group
history. Flash receipt: [local verification receipt]. On a separately rebuilt healthy
pool, the device retrieved/cached that existing message and matched all bytes; after Wi-Fi
was stopped and the DB closed/reopened, all bytes matched again offline. Normal retrieval
and publication took about1.9s. This covers12KiB single-part, not maximum quota or multipart.

Changes: complete short writes, bounded EINTR/ENOSPC recovery, and a shared two-collection
budget across nested cache operations; reuse the existing bounded DB pages while streaming
drafts, releasing them before TLS setup and after transfer. No content-length-proportional
RAM buffer was added. Unconfirmed network outcomes remain protected and are not auto-replayed.

Pressure performance FAILED: four12KiB append/abort rounds took210s against180s. A metadata
write failed after54s, but the next read recovered the original unchanged draft. Long-body
readback on that pressured pool timed out. The full1MiB pool was DPAPI-backed-up and verified;
healthy-baseline rebuilding preserved16 configuration copies/16,216 bytes with identical
content, Wi-Fi and grant, while discarding test caches/drafts. Rebuilding is not a pressure
PASS or an automatic format-on-I/O-error product policy.

Final formal bundle: build/neo-final-product-verified, app2,730,832 bytes, merged2,796,368 bytes;
merged SHA25666248ab0f8c24babc3f75d7c92606437e0161dffc87a42575284fc832d8fe3ad.
Full Build/Host gate build/gate-neo-final-product.log PASS. Its120s native observation had7
successful chat requests,5 pages/28 stored text bodies, zero unavailable bodies or allocation/
watchdog markers. A further40s cold restart retained binding, hit10 cached chats before
networking and reused the saved token. Evidence: build/neo-interop-utf8, SHA-specific
build/device-tls-report bundles, and build/device-checkpoint/neo-final-cold-reboot.json.

Outstanding: pressure latency/quarantine policy, quota/multipart native pressure, complete
power/network-failure matrix, ASR and physical UI keys/display. P5 and overall optimization
remain incomplete; SQLite/FlashDB comparison is not yet established by live benchmarks.
## RAM ownership and on-demand release (2026-11-03 native increment)

Final formal bundle after these development images: build/ram-cache-product-verified;
full gate build/gate-ram-cache-product.log PASS. Application2,729,376 bytes, merged2,794,912 bytes.
Merged SHA25648c374a81e95b7a861e99006e2b5168eaaaf765d9c378112f290c5be281890ee;
application9efeba992eb50bda1332cdc7cd21b56614dcfc92662bba1ba5b2df94a797fa60.
Its120-second native run had6 successful real chat requests and4 pages with26 text bodies
stored, zero unavailable bodies and no TLS allocation/watchdog markers. A further40-second
cold restart retained binding, hit10 cached conversations before networking and reused the
saved token. Evidence: final-bundle observations.json and build/device-checkpoint/consent-reboot.json.
Tests remained silent and sent no business messages. Full ASR, sustained pressure and complete
failure-recovery acceptance remain outstanding.


Additional native finding: SQLite peaked at15,760 bytes, below its32KiB limit. The VFS
returned ENOSPC opening objects.db-journal despite about312KB of reported free space.
Old fault-test quarantines and allocation not explained by live file sizes invalidated the
assumption that reported free space guaranteed writable storage. After taking a DPAPI-encrypted
snapshot of the entire1MiB pool, the explicitly authorized device-data cleanup rebuilt only
that application filesystem. Sixteen cfg00..cfg07 copies (15,928 bytes) were restored with
matching content hashes; old caches and the cfg7f quarantine marker were intentionally dropped.
Firmware/identity regions were untouched. FS usage became22,590 bytes and Wi-Fi/user grant survived.
This is a test-device recovery, not an automatic format-on-error product policy.

Development product d6807d18... then read five real pages with8,5,4,5,6 text bodies durably
stored respectively and zero unavailable bodies. Earlier HTTP200 observations do not establish
body-cache acceptance. Long-duration filesystem pressure/power-loss recovery and a bounded
quarantine retention policy remain outstanding.


User-data consent completed; binding and cached token survive reboot on GONGNIU-8999.
Real conversation and message reads are now observed. No simulator or business-message sends were used.
TLS failures are contiguous-RAM allocation failures, not exhaustion of the 1MiB Flash cache.

The matching ELF/MAP reports182,482 bytes of linked SRAM (IRAM code97,758, data14,116,
bss70,608). The bss already includes the24,576-byte LVGL allocator pool,11,232-byte UI
state and10,496-byte API session; do not add those twice. Task stacks (LVGL12,288,
business12,288, cache8,192),9,600-byte display DMA, driver allocations, SQLite and TLS
are separate runtime consumers. SQLite32,768 bytes is a hard heap LIMIT, not constant usage.
The token capacity is8,192 bytes; the current token length is3,306. Neither is logged verbatim.
The business stack previously had only1,064 bytes remaining, so stack reduction is unsafe
without further measurements. Wi-Fi/TCP allocation attribution remains unmeasured.

Implemented: publish/cache conversations and release their temporary frame before message
prefetch; defer the6,984-byte parser/store workspace until the first received body block;
initialize audio only for recording and release codec interfaces and I2S channels after the
capture task stops, including cancellation and failure exits. Initial stereo DMA alone was
11,520 bytes by configuration. Stable UI redraws are skipped and storage yields cooperatively;
the watchdog remains enabled. JPEG contexts/strips are already freed after decoding, including
errors; compressed lists/text and image assets stay in bounded Flash caches.

Native evidence is image-specific. Releasing the list alone still failed an8,708-byte TLS
allocation. With on-demand audio, development product c90d1897... had67,568 bytes free before
message TLS and40,520 on the first body block, largest block22,528. Its120-second run read one
complete8,822-byte real message response without TLS allocation or watchdog markers; this is
not multi-page or stress acceptance. Separate diagnostic0cc4ebd7... completed8 silent
init/capture/release/double-release cycles: after one-time88-byte initialization overhead,
all remaining cycles recovered to307,096 bytes. That probe did not run the full UI/Wi-Fi;
its free-heap value must not be presented as product memory.

Artifacts: build/device-tls-report/<full application SHA256>/ retains matching BIN/ELF/MAP
and sanitized observations; build/device-checkpoint/ram-ownership-audit.json records ownership.
Next: measure Wi-Fi/TLS/SQLite peaks, page workspace overlap and LVGL pool utilization before
changing driver buffers, IRAM options or display buffer lines. Flash caching cannot replace
active task stacks, TLS records or DMA. Real ASR, allocator/peripheral failure injection,
network recovery, long-content and sustained-pressure acceptance remain incomplete.
## Implemented behavior

Earlier checkpoint (2026-11-03 15:20): the earlier timeout tests targeted
`PXJ-X1ELQ7`, automatically taken from the PC, rather than the user's intended
`GONGNIU-8999`. They do not establish a problem with GONGNIU. Native USB setup
now explicitly targets GONGNIU-8999; using the saved same-name 5 GHz profile's
credential succeeded. The device reports network ESP_OK and returned a real
user-data authorization URL after receiving the previously selected owner app
as a RAM candidate. User consent and subsequent real API/UI acceptance are still
pending. See `build/device-checkpoint/gongniu-connection.json`. The saved desktop
profile name must not be silently assumed to be the device's intended SSID;
`--wifi-profile` permits explicit Windows profile selection. Ten obsolete local
HTTP fixture jobs (17 Python processes) were stopped; none remain. This cleanup
is separate from diagnosing a stalled model request.


Current device checkpoint (2026-11-03): the installed merged SHA-256 is
`4a9bf19f2c14e74a0eab6bfb3fe560d4a12eb40f973a95baba7f1696d9c629ce`;
app 2,726,608 B, merged 2,792,144 B. Build and Host tests PASS via
`build/gate-registration-product3.log`. This increment uses direct physical-device
tests; no simulator tests were run at the user's request.

- After Wi-Fi setup, missing app settings now open the device's official
  selection/creation QR. The Wi-Fi form asks only for network credentials.
  The registration protocol uses bounded form requests and a fixed 183-byte
  compressed permissions parameter, not the Node SDK in firmware.
- Settings separate same-app reauthorization from app/account changes. A durable
  intent survives restart without first erasing the old binding. Explicit
  invalid_client enters app repair with the original App ID preferred. Candidate
  credentials stay in RAM until user authorization succeeds; cancellation before
  commit preserves the old binding. A commit already in progress wins cancellation.
  Runtime app-key rejection retains data and directs the user to settings repair.
- Device tests PARTIAL: the silent protocol/config probe passed normal and invalid
  responses, truncated/duplicate/nested/type/size/URL cases, 2,000 decode iterations
  and 40 durable intent changes with existing app/Wi-Fi records unchanged. Probe
  SHA `6500b6c61a2eff832729a94fc409f3911d7ad8106781bf072b5d3e274d1a02d0`,
  report `build/device-registration-report/20261103-6500b6c61a2eff83/summary.json`.
  Its 16 KiB test task includes large test-oracle arrays; its stack/heap numbers
  are not production TLS/UI memory measurements.
- Native Wi-Fi remains blocked. The production scan sees the configured AP at
  about -77 to -79 dBm, but station attempts report reasons 2/205/201 and time out.
  A separate silent RF probe tried 12/30 seconds with normal association and an
  early request to disable power save: all four attempts timed out. Its completion
  marker was not observed within 130 seconds; the formal firmware was restored.
  Report `build/device-wifi-report/20261103-29f9001fd9c291fc/summary.json`.
  No experimental RF setting was promoted. The PC cannot create a 2.4 GHz hotspot;
  no hotspot configuration was changed. Final formal startup is fault-free over
  the observed window and ends in Wi-Fi setup (see
  `build/device-checkpoint/registration-final-startup.json`).
- Unverified: live on-device app selection/user grant/TLS/API, physical QR
  scannability and settings/cancel/rebind interaction. A working nearby 2.4 GHz
  network or repair of the current AP connection is needed for those checks.
  The previous desktop registration result remains DPAPI-encrypted pending USB;
  no real user API/message acceptance is claimed.


Earlier remote-setup checkpoint (2026-11-03 11:32): Build and Host tests PASS. Then installed
merged SHA-256 `e1439dda650a4387c33cddad6acc84060f311aeede51621ba9ff408a8f22e4c8`
(app 2,722,384 B; merged 2,787,920 B). Device tests are partial: four malformed /
wrong-phase / oversize input cases recover, and real Windows Wi-Fi settings save
over USB. The bare-VFS receive and storage-stack faults were fixed. Real RF is
blocked by disconnect reason 201 (NO_AP_FOUND) on the current 2.4 GHz WPA2
network, including after reset. User authorization, TLS/API and physical UI
remain unverified; do not treat earlier simulator results as tests of this image.
See `build/gate-usb-network.log` and `build/device-checkpoint/usb-network-reasons.json`.

- USB first-run Wi-Fi setup and app setup use separate bounded frames and a
  phase handshake. Wrong-phase writes are rejected before sending credentials;
  overlong lines are discarded through the next newline. Configuration uses the
  shared store and the normal onboarding transitions. The AP form remains.
- `tools/feishu_provision.py --wifi-current --port <port> --timeout 30` reads
  the connected Windows WLAN profile in memory and provisions Wi-Fi over USB.
  It neither disconnects the host nor exports a key file. Unsupported enterprise
  networks or unavailable keys fail explicitly.
- For app selection/creation, run `npm ci --prefix tools/feishu-onboarding`, then
  `node tools/feishu-onboarding/onboard.mjs --python <python> --port <port>
  --status-file build/onboarding/status.json`. The official SDK prefills the
  device's user scopes. App credentials go directly to USB via child stdin,
  never command-line arguments, status files or a cloud relay. The expiring app
  registration URL and subsequent user authorization URL require separate
  confirmations; the latter is forwarded over USB. Keep status URLs private.
  Re-run after denial/expiry/failure. These helpers require updated firmware.
- If Wi-Fi/USB is unavailable, `--pending-file build/onboarding/app.dpapi`
  retains the registration result encrypted with the current Windows user's
  DPAPI key. Resume using `feishu_provision.py --encrypted-file <file> --port
  <port> --wait-auth-url --timeout 90`; never decrypt to a plaintext file.
  Retain the encrypted file until the device has completed user authorization
  and saved the binding; then remove it manually. Reuse of an
  existing pending-file path is rejected. Owner-bound roundtrip and tamper
  rejection are checked on Windows. USB input reuses the onboarding worker for
  persistence, avoiding a second storage-sized task stack.
- Remote USB/network/API tests do not validate physical buttons/display or
  microphone/speaker quality. Host tests cover setup framing, phase rejection,
  UTF-8 byte limits, authorization URL hosts, denial/expiry/cancel, incomplete
  credentials and USB failure (`tests/test_feishu_provision.py` and
  `node tests/test_feishu_onboarding.mjs`; SDK dependencies required for Node).

- Permission errors, malformed responses and transient failures preserve binding.
  A rejected access token refreshes once; only HTTP 400 `invalid_grant` from refresh
  permits clearing user tokens. Reauthorization keeps the owner app and Wi-Fi.
- A Bearer HTTP 401 is still processed when ESP-IDF's Basic/Digest handler
  returns `ESP_ERR_NOT_SUPPORTED`; it no longer becomes an endless transport retry.
- Failed initialization retries after eight idle seconds; failed authorization
  startup retries after five seconds. Neither path replays sends or recordings.
- Message-page feedback displays successful sends and recording errors.
- Product startup/navigation uses onboarding and messenger pages only. Unused demo
  sources remain for reference but are excluded from linking. Application and
  LVGL use `-Os`; SDK radio configuration stays at its supported reference profile.
- Original image storage overlapped `0x356000..0x35a000`. Disposable token/image
  caches move to `0x360000`/`0x370000`, keeping their capacities. Main NVS credentials
  survive; cached access tokens/images are rebuilt. Never migrate image bytes over
  identity data. Validation now rejects overlapping partition tables.

## Validation commands

Run `tools/validate.sh` in ESP-IDF 5.5.3. It includes repository checks, C model,
UI math and auth-policy tests, eight provisioning tests, four layout tests,
compilation, merged-image byte checks and identity-window protection. Keep the
matching APP, full BIN, ELF, MAP, sdkconfig and flash metadata together.

For simulator integration:

1. Run `python tools/prepare_simulator.py`.
2. Build and merge `build/simulator-source` with ESP-IDF into a separate directory;
   use absolute paths for `-B` and `SDKCONFIG`.
3. Start `python tools/simulator_peer.py --port 18028 --events build/peer-events.json`.
4. Run `python tools/test_simulator.py --simulator <passportsim.exe> --build <fixture-build> --peer-port 18028`.

Assertions, receipts, image hashes, synthetic HTTP requests and screenshots go to
`build/simulator-report`. Corpus IDs and image paths are content-addressed because
PassportSim caches ELF resolution. Exit 10 means PASS_WITH_CAVEATS; tripwires and
other errors remain failures. The loopback bridge uses wall-clock pacing.

The fixture retains the actual authentication, parsing, storage, microphone,
workers and UI. Explicit differences: loopback HTTP endpoints, one-time synthetic
NVS bootstrap, virtual Wi-Fi configuration after driver initialization, and
omission of unmodeled RF power-save configuration. It is not a release/flash image.
No real accounts, credentials, messages, device dumps or eFuses are loaded.

## Acceptance boundary

### Long-content audit

Additional simulator tests use `tools/test_long_content.py` and the same guest
image. Incoming 12,000/30,000-byte text is retained only as a 255-byte UTF-8-safe
preview; 120,000-byte text exceeds the 32 KiB message-response cap and produces
a recoverable load error. A 3,000-byte ASR result is silently reduced to 510
bytes (170 three-byte characters) and that shortened text is actually sent after
confirmation. A 12,000-byte ASR result exceeds its 8 KiB response cap and fails
without a send. These five bounded-behavior assertions pass, not full-text support.

The normal UI's reply capacity is 512 bytes including NUL, but the lower-level
send/reply functions have no independent text-length cap. HTTP response chunks
coexist with their joined buffer (about 64 KiB at the message cap); cJSON and
nested message parsing create further copies. This is bounded reception, not a
proof against allocation failure. PCM uses four 3,200-byte buffers rather than
retaining an entire thirty-second recording. Outstanding improvements include
explicit truncation feedback, API-level send limits and lower-peak reception.
No production firmware behavior was changed for this audit. Real TLS/heap stress
and sustained device behavior remain unverified.

Validated on 2026-11-02: complete build/host gate PASS; seven simulator integration
checks PASS with hardware fidelity caveats. Production first boot reaches the
Wi-Fi QR page. APP: 2,188,352 bytes; merged BIN: 2,253,888 bytes. The APP is 335,200
bytes (13.28%) smaller than the first local build, with the full Chinese font.
Merged SHA-256: `ab84f805336eef4373fa27ca0000fc128edb3bc91e3a467c88ed208844cad911`.
Matching artifacts are under `build/accepted`; the integration summary and
screenshots are under `build/simulator-report/20261102T051423956875Z`.

Report Build, Host tests (Emulator, not hardware), Device tests and Unverified
separately. Real Feishu OAuth/TLS/ASR interoperability, mobile provisioning,
physical buttons/microphone, reconnection and sustained use require device tests.
The last USB inventory contained Bluetooth serial ports only; no device was
flashed. Simulated HTTP success does not establish real Feishu message delivery.


### Mention mapping and silent physical storage verification (2026-11-03)

The compressed raw body is stored once. Mention dictionaries and sparse offsets share the metadata quota; names are rendered per byte window. Metadata may precede or follow the body. More than eight mentions, boundary reads and offline reopen are supported. Mapping failures preserve literal body tokens. Late errors, cancellation and account-scope changes never publish partial data. Protected outgoing drafts always use literal reads.

The actual production SAX/store/cache pipeline passes 42 host cases. The storage suite covers 32 names, numeric names, CRC corruption, atomic publication failure and quota reclamation. Runtime SQLite pages are 512 bytes with explicit indexes; its hard heap remains 32 KiB.

Silent physical probe PASS: application SHA256 bac162b528f8acfe57bc69e27e72244ae3463afe910bbb4c3599540450d02806; report `build/device-text-report/20261102T232056Z-bac162b528f8acfe/summary.json`. Indexed windows, reopen, corruption, transaction rollback, the full 128 KiB metadata quota, reclamation, subsequent writes and protected records passed. SQLite peak was 23152 bytes; the 12 KiB probe task retained at least 4752 stack bytes. Reference arrays are test-only. No network, display or audio was initialized. Formal image de28b10e955c9ccfd59fc2ca86df45c844b65756d9fedde324e09e43c58f2cd9 was restored.

Emulator SHA256 7061f0bd32b4e98eb428454fb04519657bf921743a20f2f9c4ddd3163b8c2fff passes three checks for 32 mentions, 68 pages, offline page-four restoration and tail access (`build/mentions-report/20261102T231856Z`), plus five cache checks (`build/cache-simulator-report/20261102T232054Z`). Remaining whole-application regressions are ongoing; older image results do not establish this image's acceptance.

Previous native watchdog/timeout reports remain preserved. Cooperative yielding, bounded metadata accounting and representative mixed-cache pressure now pass without disabling watchdog protection. Real networking, screen/buttons and complete resource acceptance remain unverified. List pagination, a global draft manager and whole-database corruption recovery remain pending.


### Global draft acceptance and formal startup

Emulator image 7028fbe0e052f26542530883f9c776ea1b2c3106f1ecb1aea4b9b5bda2e3efd0 passes six global-manager checks (`build/draft-manager-report/20261102T233143Z`) and the six existing long-outbox regressions (`build/outbox-simulator-report/20261102T233331Z`). These cover missing visible destinations, offline reboot/rejection, failed rerecording, immutable targets and subsequent ordinary compose/cancel.

Formal image 0c69f721c81e551d879b684d119027ef489edd02f35a614c1aa8be74cc729998 passes the complete gate. APP is 2711392 bytes; merged image is 2776928 bytes. COM7 startup was observed silently for 40 seconds without fault markers and reached unconfigured-network onboarding. Real networking, display and button acceptance remain unverified. Previous image 7061f0bd32b4e98eb428454fb04519657bf921743a20f2f9c4ddd3163b8c2fff separately passed all 39 regressions; cached image startup was 1.43 seconds and quick-pan framebuffer latency 2.419 seconds. Do not pool results across images. List pagination is in development; whole-DB recovery and final resource/fault acceptance remain pending.


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


## 4 bpp and onboarding UI iteration (2026-11-04)

The user confirmed first-run Wi-Fi cancellation, resubmission, app selection, personal authorization and conversation entry on the previous product. The current iteration distinguishes the blue app step and green personal-account step, separates QR and bottom copy, and renders a normalized two-line message prefix before ellipsis. Full 21,421-character 4 bpp glyphs occupy 1,737,446 bytes in a read-only partition at 0x500000; firmware, protected identity and shared data offsets stay fixed. SHA-256 is verified before use; missing/corrupt font assets show an English recovery screen without modifying settings.

The physical device initialized the microphone normally, then rejected the first ASR response after about one second. A host request using retained app credentials returned HTTP 400 / 99991672, missing speech_to_text:speech. Registration now requests that scope as app identity, while message operations remain user identity. Existing apps need permission repair; this host result alone does not prove the device used the same app or that end-to-end ASR passes. Runtime diagnostics record numeric status/code only.


## Empty-draft lookup and footer (2026-11-04)

Four atomic empty-slot bits avoid queue/DB waits for a verified-empty outbox. Scope changes, recovery, close and slot writes invalidate knowledge; read failures never prove absence. Initialization primes the index before background requests. Cold/unknown reads remain cancellable with a long press and retain durable drafts on error. The footer separates a red error/normal status line from page/key guidance below a horizontal separator, with the same dark background.

ASR permission repair was confirmed for the retained app. Subsequent sparse host calls returned 400 / 99991400 instead of missing scope. Both official streaming and file ASR documentation exclude free tenants; the user confirmed a free tenant. Client voice features have separate entitlements. ASR remains unavailable until an alternative service/flow is chosen; no paid service was enabled.

Latest UI refinements: exclusive focus between message bubbles and composer, centered composer label "Send message", explicit long-press hints and current/total page notation (unknown totals stay unknown), header divider and measured battery retained. Startup logs the actual decoded glyph alpha levels, display antialiasing and color depth.


## Corrupt filesystem references and configuration-preserving recovery (2026-11-04)

Device open failures were SQLITE_IOERR_READ (extended 266), not evidence of missing drafts. The encrypted 1 MiB snapshot contained two inconsistent SPIFFS references inside objects.db; one addressed a deleted page and one the wrong data span. Offline SQLite also rejected the extracted database. No original plaintext was written to the host filesystem.

The recovery read fresh device state immediately before writing, verified nine configuration keys with two CRC-valid mirrors each, and rebuilt the pool preserving all 18 non-database files byte-for-byte. Only objects.db and its journal were excluded; a complete encrypted original remains restorable. The rebuilt pool and identity window passed readback. The physical device retained binding: initial empty-slot scan including new DB creation was 659 ms, subsequent scan 0 ms with no unreadable slots. Chat requests succeeded. This recovers the current failure; prevention of repeated SPIFFS corruption and near-full performance remain unresolved.

The current FlashDB raw-NOR host rerun passed full/eviction/reopen and 24 failed/partial-write recovery cases. That supports a backend candidate, not product migration, native timings, or physical power-cut safety. The next comparison must cover app object commits, required drafts, quotas, compression and fault recovery on the same device budget.

## Interaction/font V19 and automatic acceptance (2026-11-06)

The ordinary XiaoZhi variant is installed: APP **2,022,688 bytes**, SHA-256 `650dfc55133d25c28ddbd619b8fe1e8f980954781eb2bfe886186a161cd122aa`. Matching Medium font: **1,814,190 bytes**. Identity window, partition table, binding and the existing 1 MiB pool were preserved. The controller left the matching font untouched during V19 APP updates.

| Evidence | Result | Boundary |
| --- | --- | --- |
| Build / Host tests | Full gate PASS; 24 KiB static LVGL pool verified | Extra geometry test passed after the gate: 256 height mixtures x 10 selected positions |
| Simulator | Six screens rendered; no pure-white pixels | Synthetic UI only; no networking/microphone/battery claim |
| Voice | Original nine cases plus warm-start/cancel/recovery PASS; warm first frame 34 ms | Known PCM inserted after real I2S reads; human voice remains unverified |
| Send / current page | Durable ASR review exact, production send handler and fresh current-page readback PASS | POST 1,218 / 1,480 ms; not button-to-screen totals |
| Focus + HTTPS | Three cycles of settled microphone + real message refresh + focus-loss release PASS | Heaps after cycles 85,444 / 85,232 / 81,216 B; this is not a long-term no-leak qualification |
| Memory | Diagnostic low-water 17,576 B; no panic/watchdog | 20 KiB target remains open; long mixed workload and real power interruption unverified |
| MCP | Configured service hello/initialize/initialized/tools-list PASS on host | Empty tools, zero audio/Feishu actions; no device MCP or simultaneous-TLS qualification |

Local reproducible evidence: `build/device-checkpoint/experience-v19-xiao-native.json`, `build/experience-screenshots-v18/result.json`, `build/device-checkpoint/mcp-host-negotiation.json`. V19 contains lifecycle/queue coalescing refinements; its visual palette/layout matches the V18 fixture. No human participation requested while the user is unavailable.

## Resource qualification V26 (2026-11-06)

Ordinary XiaoZhi APP **2,022,064 B**, SHA-256 `dcc58162d19c58521aa939ac17654ec30545b23609473e7ac2a6a0b0e3ef74e4`, is installed with the unchanged matching 1,814,190-byte Medium font. Identity, partition table and binding checks passed; the existing pool/drafts were retained.

| Check | Result | Scope |
| --- | --- | --- |
| Build / Host tests | Full gate PASS | PCM6 absolute-error/domain/bounds/40s reuse; prior protocol/layout/storage regressions |
| Voice | Nine cases PASS; warm-start, warm-cancel and recovery PASS | Known PCM after real I2S reads; capture 16 kHz, network 8 kHz PCM |
| Queue | Context 21,928 -> 16,240 B, saves 5,688 B | Same 120 x 20 ms = 2.4-second backlog; frame-scaled six-bit RAM packets |
| Minimum heap | 22,372 B | Exceeds 20 KiB in this diagnostic load; not a universal floor |
| Send | Exact durable ASR review, production send/current-page refresh PASS | POST 1,186 / 1,415 ms; three focus/refresh/release cycles PASS |
| Separate memory diagnostics | Two independent 80/80 runs, stable audio tasks | Quiet TIME_WAIT 16 -> 0; 3,440 B recovered without reboot |
| Separate MCP prototype | Device initialize/list, local status, one-message dual TLS and nonce PONG PASS | Minimum heap 37,644 B, smallest worker-stack margin 900 B; remote semantic calls=0 |

V25's smaller 8 kHz/four-bit ADPCM candidate was rejected at 8/9: three host A/B trials each changed the fixture's brand name on the candidate while the previous codec matched. V26's six-bit candidate matched all three host trials before native qualification. Golden text was unchanged. V23's failed PONG assertion was corrected by explicitly propagating SDK control frames and testing partial/interleaved WS payloads.

The first V26 launcher accidentally selected the old V25 controller; its pool-header preflight stopped before any APP write. The initial mismatching header bytes were not retained. A subsequent encrypted full-pool read-only backup found 64 valid headers, with no manual erase/repair. The corrected controller checks the target MAC and PCM6 artifact marker and captures any future bad sector encrypted before resetting. It then passed 64 headers and completed V26. Do not claim a confirmed cause for the initial mismatch.

Evidence: `ram-v26-native.json`, corrected `ram-v26-native-console-corrected.txt`, `queue-compare-v25.json`, `queue-compare-v26.json`, `memory-v21-summary.json`, `mcp-v23-native.json`, `mcp-v24-native.json`, and encrypted `pool-preflight-20261106/` under local `build/device-checkpoint` (console in `build/`). The overwritten old V25 JSON is not its original acceptance record; its earlier 8/9/fallback console remains preserved.

Unverified: natural human speech/screen/button experience, longer mixed workload and true power removal. Full assistant/UI control/query paging/reviewed sending are not implemented; ordinary ASR still advertises MCP disabled. The 900-byte prototype stack margin requires redesign before widening the tool path.

The initial ordinary-boot serial window missed the binding log line. A separate continuous capture using the existing serial handle verified the same APP hash, `bound=1 wifi_setup=0 auth_request=0`, normal network entry and no faults; see `ram-v26-boot-confirm.json`. The initial false log-capture flag is retained in the final report.

## Unified screens and account settings (2026-11-06)

The ordinary XiaoZhi build is installed after the complete gate and silent native regression. APP 2,026,640 bytes, SHA-256 `4890374b747c501bc43a60b71312801512633b24186d02c35d40bbd7005cdbf6`; the mapped font remains 1,814,190 bytes. The existing binding, identity area, partition table and font were checked, and existing application data were preserved.

| Evidence | Result | Boundary |
| --- | --- | --- |
| Build / Host tests | Complete repository gate PASS; composer shortcut and account/settings routing pass | Includes activation-code priority, bounds, malformed data and recovery |
| Simulator | 33 states plus a double-click focus screenshot; 34 images and shared chrome/gap checks PASS | Synthetic data and invalid demo QR payloads; not device photographs |
| Device voice | 9/9 plus warm-up/cancel recovery, 30 s, 1.5 s backlog and 5 s timeout recovery PASS | Real I2S reads with injected known PCM; human voice quality remains unverified |
| Device account/query | XiaoZhi query cancellation then paired-state query PASS | Already paired device; first-time code entry/activation not physically exercised |
| Device send/recovery | Durable ASR review, real send, newest message on current page and three focus/release cycles PASS | POST 1,476/1,992 ms in this workload; cold-load latency is not a guarantee |
| Memory | Minimum free heap 20,776 bytes in this run; 2.4 s queue retained | This is only the observed workload, not a universal lower bound |
| Unverified | Physical double-click feel, phone scanning of the new QR layout, first-time XiaoZhi pairing, long mixed load and real power loss | Existing device/account was not unpaired for this test |

Settings now contain Network, Feishu account, Draft management, XiaoZhi settings and Return. Feishu account owns the existing login/application-selection/authorization flow; the client-session login experiment was cancelled by the user.

Evidence: `build/device-checkpoint/ui-standard-native.json`, `build/ui-gallery-screenshots/result.json`, `build/feishu-ui-gallery-20261106.zip`. Ordinary firmware disables MCP and diagnostics.

## Menu focus and status layout refinement (2026-11-06)

Ordinary APP 2,026,288 bytes, SHA-256 `2ab38725e300620532e0fcd1b03fe7a311606b29da09d7d32b3757f962df6b05`, is installed. Build and the complete Host gate pass; 35 simulator frames pass footer-only red pairing-error, single menu-outline, draft-index, duplicate-heading and chrome/spacing checks. Native acceptance in this UI-only increment covers verified APP/font/table/identity, 64 pool headers, retained binding, normal boot and network connection without faults. It does not repeat the previous version's voice/send suite or establish physical display/button acceptance.

Account, draft and image action pages reuse the settings row component and yellow focus outline. Pairing expiry/missing-code errors appear only in the footer. Draft selection counts share the footer control line. Default text/menu content starts at y=46 below the y=34 rule; message rows retain y=38. Six conversations still fit using 34 px rows/36 px stride. Duplicate image/confirmation titles are removed.

Evidence: `build/device-checkpoint/ui-refined-native.json` and `build/ui-refined-gallery-screenshots/result.json`. The full 35-image package is `build/feishu-ui-refined-20261106.zip`; its QR/code data are synthetic. Existing bindings/data and mapped font are preserved.

## XiaoZhi setup guidance and draft hierarchy (2026-11-06)

Installed ordinary APP: 2,070,992 bytes; SHA-256 `db2eb8fb63b47be52d0c8e1d79d4c5184bcf8d55b0068f1d8b3271339537ab19`. Font: unchanged, 1,814,190 bytes. Settings order: Feishu account, XiaoZhi settings, Network, Draft management, Return.

Missing/expired pairing UI starts one background query, with a 30-second retry cooldown and cancellation on exit. The public console QR is a 43,808-byte RGB565 Flash asset, avoiding a runtime full-image canvas. Draft titles and metadata use distinct text colors.

Build/Host gate passed. Simulator passed 35 frames, six decoded console QR states, draft colors, settings order and the composer shortcut. Native tests passed cooldown, injected missing/expired UI states with real queries against the existing paired device, draft colors and QR cleanup. Ordinary firmware was restored with identity, partition, font, binding and application data preserved; no faults were recorded.

This increment did not rerun voice or send suites. First-time unpaired activation, phone scanning, physical appearance and long-running fault tests remain unverified. `lv_free=3592` refers to the fixed 24 KiB LVGL pool, not total device heap. Evidence: `build/device-checkpoint/pairing-qr-native.json`, `build/pairing-qr-gallery-screenshots/result.json`; screenshot archive: `build/feishu-pairing-qr-20261106.zip`.

## Network management and two-way focus switching (2026-11-06)

Installed ordinary APP: 2,079,168 bytes; SHA-256 `655ba653303567c43ebf0f64514410ccb5faad1a94b997bea330025a16ed2a5c`. Font remains 1,814,190 bytes. Identity, partition layout, binding and application data were retained.

| Layer | Verified result | Boundary |
| --- | --- | --- |
| Build / Host | Complete gate; ten-profile limit, edits preserving active selection, deletion/empty state, corruption rejection and re-add | Store contract runs against both file and actual FlashDB implementations; underlying failure/power-loss matrices remain covered |
| Simulator | 50 frames; visible 64-character password, fixed actions, default Add focus, two-way double-click and ordinary boundary navigation, no extra rule | Synthetic data, not physical photos |
| Device UI | Default Add, per-network actions/focus redraw, real scan after Add and transition to password page | Automated navigation; physical feel unverified |
| Device network | Cancel, disconnect/reconnect, rollback after a wrong saved password, wrong candidate rollback, forget-last/re-add, scan and real Feishu conversation read | Silent; no message sends or voice suite rerun |
| Installation | Ordinary firmware restored; identity/table/font checks pass, no faults, original data retained | Diagnostic disabled |
| Unverified | Phone provisioning end to end, real power loss, prolonged mixed load and physical controls | Automatic speech playback is not integrated |

Saved networks occupy the content list; Add and manual provisioning stay at the bottom with whitespace separation. Entry defaults to Add. A profile opens Connect, Disconnect, Forget and Set password. Editing saves without connecting or changing the active profile. Ordinary arrows cross the list/action boundary; double UP focuses content and double DOWN the composer/Add action. Page-owned editor state participates in repaint detection so cursor changes render immediately.

A driver config measures 184 bytes: ten plus a 16-byte header require 1,856 logical bytes before redundant copies and KV overhead. The transient book buffer is capped at 2 KiB; list/editor state exists only while network settings is open.

Separate silent probes of the existing XiaoZhi identity received no audio for two direct-TTS variants in six-second windows. The listen/detect route explicitly rejected long text as wake-word-only. The hello response advertised Opus/24 kHz. The current ASR integration therefore does not establish original-text playback support; a synthesis endpoint is needed before adding a working automatic-playback setting.

Evidence: `build/device-checkpoint/network-focus-r2-native.json`, `build/network-focus-r2-gallery-screenshots/result.json`, `build/xiao-tts-interface-report.json`, `build/xiao-tts-alert-report.json`; archive: `build/feishu-network-ui-20261106.zip`. The earlier navigation-entry timeout rolled back automatically; `network-actions-r2-native.json` remains failed evidence. The installed version fixes entry-page ownership and repaint detection for page-owned state.
