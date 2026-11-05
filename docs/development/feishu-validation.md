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
