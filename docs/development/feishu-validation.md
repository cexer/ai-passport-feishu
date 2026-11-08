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
Wi-Fi QR page. APP: 2,188,352 bytes; merged BIN: 2,254,256 bytes. The APP is 335,200
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

## XiaoZhi assistant initial acceptance (2026-11-06)

Installed ordinary APP: 2,189,984 bytes, SHA-256 `8e11486e6f1866ea05c55f88a675a16a941a9af77e88ac9cda9e3877376514ef`. APP-only installation
retained binding, protected identity, partition table and the existing font.
The diagnostic was removed; ordinary startup loaded ten cached conversations
and successfully refreshed ten live conversations. Message announcements/Edge
TTS remain paused. The assistant uses XiaoZhi reply audio; ordinary dictation
still takes STT text and aborts the reply.

| Layer | Current evidence | Limit |
| --- | --- | --- |
| Build / Host | Complete gate PASS; typed/index argument checks, mutual exclusion of chat ID/index, paginated discovery, 2 MiB no-storage preview stream, 100000 packet resets and 1000 decoder-adapter lifecycles | Host codec is a fake; actual Opus evidence is device-only |
| Native protocol / audio | Seven scenes PASS on the real configured device: list chats, open settings, save/clean an owned draft, cancel, capture failure, fresh latest-message read after failures, open a chat and apply its message list | Synthetic input overlays real capture; output volume zero; no business message sent |
| Targets / data | Draft/read/open selected index zero; test draft object and UUID checked before cleanup; ordinary binding and protected regions compared | Current cached conversation page only; no full-directory search |
| Resources | Latest confirmed suite minimum heap 23,420 B; another run observed 20,212 B; worker minimum stack headroom 1044 B | The 20 KiB heap goal is not a universal guarantee; longer mixed pressure remains open |
| Physical / intent | Hardware key feel, real microphone/prefix and audible quality pending | Compound requests sometimes stopped after listing; single-intent routing is the current acceptance boundary |

Use Settings → XiaoZhi assistant. OK starts speech, OK finishes capture, long OK
cancels the turn. Message requests save a durable draft; the user confirms send
through the existing device UI. No direct send MCP tool exists.

The assistant preview API does not persist/compress a full message just to answer
a query. Wire JSON is allocated on demand and released per message/before tool
HTTP calls; MCP response memory is absent during audio. Earlier stack failures
and the first serial-summary gap remain in their reports, rather than being
relabeled as passes. Evidence: `assistant-r8b-native.json`,
`assistant-r8-installed.json`, `assistant-r8-final-gate-status.json` under `build/`;
design and reproducible fixtures: `docs/software-design/feishu-assistant.md`,
`tools/assistant_probe/README.md`.

## Continuous assistant acceptance (2026-11-06)

Ordinary APP 2,195,952 B, SHA-256 `01ebec828784eed9c35619b2bcc618f3c52de487d4d0d979a110ade029c2548d`. Full gate PASS; native R8 eight
scenes PASS, including a greeting, list/read/goodbye in one WSS visit, then
actual applied message-list loading. Synthetic input and muted diagnostic output;
real cloud tools and Opus/PCM. No business sends. Binding, Wi-Fi, existing
drafts, protected identity, partition table and font retained. Ordinary boot PASS.

Both input paths now send 16kHz PCM from the fixed 8kHz PCM6 queue; no full
recording allocation. Defaults: boot assistant off, silence three seconds.
Global double-OK enters; long OK exits. XiaoZhi settings separates account and
assistant settings. Physical speech, keys, sound quality, all setting choices
on hardware, real noise, extended sessions and power loss remain unverified.
Minimum heap in this suite: 24,796 B; worker stack minimum: 996 B.
Failures and the cancelled-load test ordering are retained in earlier reports.

## Disposable single-draft acceptance (2026-11-06)

APP 2,199,216 B; SHA-256 `0d9d3efa9b676b3c76ee0a136fbb167605a0355ea0c1c6398477df2fe66768c8`. Full Build/Host gate PASS. Actual FlashDB
replacement, failed publication, old-state recovery, clear/reuse, reader-protected
pressure and idle eviction/ghost cleanup PASS. Native injected draft-storage failure
retains a complete message in RAM, restores it through Draft management, and
rejects substitution after failed recipient lookup. No business POST in this test.
Ordinary APP-only installation and cached/live conversation startup PASS.
Actual target-group sending, physical UI and real power loss remain unverified.
Evidence: single-draft-ram-native.json, single-draft-installed.json, single-draft-gate-status.json.

## Global assistant and collaborative editing regression (2026-11-07)

Build: PASS; Host tests: PASS, complete R8 gate. Ordinary APP 2,228,416 B,
SHA-256 `de783b5a5856a32fdf27bc1e5b8a28af9079283f90d6a6f5ac8f224ac93b26ea`.
This candidate is now installed on COM7. APP-only hash verification and a 45-second boot check passed; cached/live conversations and account/Wi-Fi binding were retained. No rollback occurred.

Device tests: PASS for the two bounded, muted diagnostic suites. Global listening
retained one connection across background message/conversation loads, member
lookup, three synthetic speech turns and goodbye. Minimum free heap 21,924 B;
worker stack minimum 2,564 B. The previous R6 low-water mark was 5,256 B. These
are scenario measurements, not a universal reserve guarantee.

The real cloud called read_draft then edit_draft for an injected current-draft
utterance. Recipient preservation, stale revisions, undo, uncertain/oversized
content rejection, physical confirmation and RAM restoration after storage failure
passed. Minimum heap 21,108 B; worker stack minimum 2,340 B. Neither suite sent
a business message. Binding, identity, partition table and font were retained;
the original ordinary APP was restored after each suite.

Simulator: ten fixed-data scenes plus a second breathing-state capture passed
full-width header/footer, badge and text checks. Physical-device rendering and
acoustic quality are not established by these screenshots. Unverified: real
physical speech, actual structured @ delivery, post-send list echo, extended
sessions and power loss. Earlier ordinal-selection failure remains recorded;
recipient guards were not relaxed. Reports: global-assistant-r8-native.json,
collab-edit-r8-native.json, global-assistant-r8-gate-status.json and
assistant-gallery-r7-screenshots/result.json under build.

### Ordinal target-selection retest

Native integrated-compose-r8 passed: the real model prepared chat_index0 directly. Recipient guard, RAM restoration and revision/edit/undo checks passed; minimum heap25,660 B and stack2,340 B. No business sends. The earlier R6 failure remains retained, and the guard was not relaxed. Ordinary fallback restoration and binding verification passed.

## Conversation controls, volume and startup repair (2026-11-07)

Full R10 Build/Host gate PASS. Ordinary APP 2,230,176 B SHA-256
`60e9f0622505a5da80cb7cfab9a88a9b5088cb44561ab0c2bc6435a6e01f7fa7` is now installed,
not rolled back. Header badge moves to x158; conversation voice activity retains
long-OK Settings and labels silence-only submission. Ordinary business OK is no
longer intercepted by voice capture. New defaults use two seconds; explicit older
choices are preserved. Version-two four-byte assistant settings add 0..100 volume;
version one reads with volume60. Host bounds/reopen/legacy/corrupt tests pass.

Native reboot before repair found bound0/forced-WiFi1. An encrypted pool snapshot
showed valid BOT credentials and remembered Wi-Fi but an empty personal refresh
token; this is not evidence that the whole pool was lost. The original trigger
for token clearing remains unverified. Forced portal requests are now consumed
once, and OK/long-OK in the portal retries remembered Wi-Fi. The muted native
probe exercised the actual key handler: request consumed, portal retry and saved
network connection PASS. The ordinary candidate then booted with WiFi-setup0,
connected successfully and displayed personal authorization. Identity, table and
font remained unchanged; the user subsequently completed authorization. A separate ordinary reboot retained bound1/WiFi-setup0 and read ten live conversations successfully (r10-authorized-reboot.json).

Production startup screenshot PASS: bounded breathing title/progress dots,
no empty control-only page, a short success tone respecting the sound switch.
Twelve fixed-data gallery scenes plus breathing capture PASS; active-chat footer,
right-aligned badge, volume row/editing and defaults inspected. These are simulator
images. Actual listening, audible volume choices, startup sound quality and the
new candidate's authorized live-session regression remain pending. Evidence:
ui-onboarding-r10-installed.json; ui-onboarding-r10-gate-status.json;
assistant-gallery-r10-screenshots/result.json; startup-r10b-screenshots/result.json.

## Assistant activation/capture regression repair (2026-11-07)

Build: PASS; Host tests: PASS (full R14 gate). Installed ordinary APP 2,230,896 B,
SHA-256 `d9458d7bff3762e8ce203d44f58a2518ee08e0a36e192c51d640f1daed22f8e7`.
Actual R10 reproduction connected and greeted successfully, then failed with263:
quiet capture could fill its queue while the worker waited. R11 idle-only repair
failed the voiced regression. R12 profiling showed a198ms median processing loop
against20ms input; the invisible capture clock forced full list/font repaint and
per-frame UI mutex dependencies delayed audio. These failed runs are retained.

The producer now retains only six idle frames in the existing pool. On onset it
publishes that prefix and every spoken frame in order. Business paint ignores the
invisible clock; capture reads a four-byte atomic timing snapshot, feedback with
no pending sound avoids the UI lock, and work polling uses a nonblocking snapshot.
No audio queue expansion. Binary writes have a2s deadline within the2.4s capture
budget, and ambiguous failed frames are never replayed.

Device: PASS for five-second worker-stall pressure, live page work/member lookup,
three real-cloud synthetic turns (list/read/goodbye), cleanup and ordinary boot.
572 successful frame writes, median3ms, maximum53ms; peak ready queue6/120; no failed
writes or backpressure. Minimum heap20,948 B, worker stack minimum2,516 B. New
ordinary firmware remains installed, binding/identity/table/font preserved. No
business POST. Physical speech and extended adverse-network acceptance remain
unverified. Evidence: assistant-idle-r14-native.json and -metrics.json; earlier
R11/R12b voiced failures and R13 one-second write timeout remain separate. One
interrupted USB-transfer summary is explicitly reconstructed after a retry reused
its report filename; it is not represented as an original raw log.


R17: Build/Host full gate PASS; ordinary APP 2,232,256 bytes,
SHA256 `1c1fce6c99057a095dd14bc3707ea14edb42fc3274c18c01c2b1c970c330a35b`.
Simulator: sixteen frames have zero normalized glyph displacement; suffix covers
zero to three dots and four status/settings scenes pass. APP-only device install
and ordinary boot PASS (bound=1, wifi_setup=0); identity/table/font unchanged.
R16 native suite passed accelerated capture expiry/cleanup, five-second idle
pressure, three synthetic-input cloud turns and idle continuation after turn two.
No business messages sent. Original spontaneous exit was not reproduced;
physical speech/acoustics and long-duration network behavior remain unverified.


R19: full Build/Host gate PASS. Native fault injection PASS: reply preemption with decoder released, cached/network message load, one failed glyph allocation, continued GUI heartbeat and actual back handler, five-second idle stall, three cloud turns and goodbye cleanup. APP-only install/boot retained binding/WiFi; identity/table/font unchanged. R18 overall FAIL was retained: injected back cancelled the concurrent load probe; R19 separates the two steps. Physical buttons, real voice and long-term stability remain unverified. APP 2230752 bytes, SHA256 `689205243ae84f2579fda94fd32c16ef9307562816c4f0a46d82ff1f4e37f399`.


R20 Build/Host full gate PASS; APP 2232992 bytes, SHA256 `6e2a5caa65c77fb912f38b40661b58cc5bf2c2c01f0155614741a9de88ecf017`. Native tool/state tests: direct empty editor, empty/stale confirmation rejection, actual edits, 18-second confirmation wait with a real 15-second WSS ping, cancel then edit, dry send-owner handoff, five-second idle pressure and three cloud turns/cleanup PASS. No business POST. Ordinary APP-only install retained binding/WiFi, identity/table/font. Original reported disconnect not captured; natural-language tool selection, physical OK and real send remain user acceptance.


R25 final: Build/Host full gate PASS; ordinary APP 2237104 bytes, SHA256 `27c38652f0265edc9dde0aac907b2c60780cabca685557041f45c4f462e7fa16`. Native PASS: three fixed seeds, 30 forced-network reads, 30 cache reads, 144 mixed UI operations, 3 UI-task cancellations; memory-owner recovery, initial and capture-idle wire-fault recovery, 75-second quiet interval, full MCP-page registration before actual mic READY, business callbacks never READY, real cloud open_chat then ready/read/bye. Minimum free heap 23032 bytes since boot, worker stack 2452 bytes observed, no universal reserve claimed. No actual POST. Image/detail counters are branch attempts, not physical media/display acceptance. R21/R22 failed reports retained separately. Ordinary install preserved binding/WiFi/identity/table/font; volume0 and all three sound flags remain off until explicit restoration. Human speech/acoustics, physical keys and real delivery remain unverified.


R27: full Build/Host gate PASS; ordinary APP 2237600 bytes, SHA256 235e15a712d7bdccc78ef10acb83fa67a2e6ff1340edf7fde8097c1f7f40a4dc. Versus R25, APP +496 bytes and main UI state +8 bytes; no new persistent task/audio pool. Native PASS: actual LVGL sending title/recipient/body retained; same-activation owner recovery skips greeting; 30 cold/30 warm reads, 144 mixed actions, 3 cancellations; cloud open-chat then listen, 75-second quiet interval and wire recovery. Observed minimum heap 20940 bytes, worker stack 2452 bytes (not a universal reserve). Ordinary APP installed, binding/identity/table/font preserved, volume0/all sound flags off. No actual POST; human acoustics, physical keys and delivery remain unverified. Concise hint uses MCP initialize instructions; the client may ignore it, so strict length is unverified. Triple-press driver support confirmed; no new binding. Failed R26 evidence retained separately.


### Continuous standby qualification (R38)

- Continuous standby capture: ordinary R38 full Build/Host gate and full native pressure/fault regression PASS; earlier focused native evidence retained separately.
- Native PASS: same capture task/queue retained through reads; a real GET overlapping speech onset yields, then cloud recognition retains opening intent; battery sampling/30s successful network refresh do not restart capture; 75s quiet interval, wire recovery, three cloud turns and goodbye.
- Also 30 cold/30 warm reads, 144 mixed actions, 3 cancellations, collaborative editor and sending-page preservation.
- Ordinary APP 2238720 bytes, SHA256 226697d7e2adb5bab90f6422e79b8be529140a272c45d7d1fc0ec7a07b1e8caa, APP-only installed with binding/WiFi/identity/table/font retained.
- Reuses original120-frame (~2.4s) queue; no new audio pool/persistent task.
- Observed minimum heap 5928 and worker stack 2452 bytes, not universal reserves.
- Volume0/all sound flags off.
- No business-message POST.
- Human acoustics/physical keys/delivery, arbitrary network stalls and acoustic playback barge-in remain unverified.
- Earlier network, fixture-timeout and reply-preemption failures retained separately, never counted as passes..

## Task completion and collaborative editor R39

- Build and host: complete gate PASS.

- Native function and pressure: 30 cold reads, 30 warm reads, 144 mixed operations and three actual UI cancellations/recoveries PASS. Current-context empty editor, no recipient guessing, confirmation/cancel, memory/wire recovery and continuous capture PASS.

- Cloud ordering: actual open_chat data ready at 371980 ms, tool return at 371981 ms, elapsed 1234 ms; reply audio started at 373169 ms.

- Installed ordinary APP: 2240176 bytes; SHA256 `07e235725d5df6d4ed6127306c0b79e8ec8cfc0cbd8c03a749c77459ae24cc7e`. Binding, identity, table and font preserved.

- Memory boundary: stress minimum free heap 4412 bytes, not constant available RAM. No business-message POST.

- Human acceptance pending: voice-to-composer intent routing, visible stale-dictation takeover, acoustic quality and consistently short model acknowledgements.


## Voice controls and concurrent memory qualification (R53)

| Layer | Result and evidence |
| --- | --- |
| Build | Full gate PASS; ordinary APP 2246576 bytes, SHA256 `fc307a1acb8a079760625b335b76fc90a673c20d4efe67e9ff6f2c9f85c6b021`. Matched APP/ELF/MAP/merged/font/config/source bundles retained locally. |
| Host | Full logic/static gate PASS, including MCP argument validation, audio queue budgets, EAGAIN handling and opt-in HTTP TX lifetime/partial-header/body/POST/wipe/OOM cases. |
| Native controls | Seven persisted settings and delta, navigation, validated original-message reply targets, same-turn/stale/duplicate/full-queue rejection, later-turn one-send handoff and confirmation capture PASS. Business send queue was consumed by the diagnostic before POST. |
| Native pressure | 30 cold reads, 30 warm reads, 144 mixed actions, three actual UI cancellations/recoveries; 72 parser lifetimes each followed TLS/request TX release. No real TLS OOM, watchdog or unexpected replay/reconnect. |
| Native audio/cloud | Capture retained during actual HTTPS, speech preemption with complete opening intent, 80-to-120-slot promotion before upload, deferred read resume, background refresh/battery with capture live, confirmation keepalive, 75-second quiet standby, memory/initial-wire/idle-wire recovery and three cloud turns/goodbye PASS. |
| Independent audit | Actual open_chat data-ready before tool return, 589 ms; TX/parser ordering and frozen source integrity verified. |
| Installation | Ordinary candidate installed APP-only; binding/WiFi, identity, partition table and font unchanged. Probe inactive; volume zero and sound flags off retained. |

Observed minima are 2,824 bytes free heap since boot, 2,416 bytes business-worker
stack and 2,740 bytes LVGL stack; these are finite-run observations, not a universal
reserve. Built-in TJPGD is disabled and the original 20-row display buffer remains.
Active audio format/rate and 120-slot backlog are unchanged. UI interactions and
continuous standby capture were not reduced to avoid concurrent work.

Earlier R40-R52 failed qualification reports remain separate. In particular R52
failed a real 17,058-byte static RX allocation; R53 changes only parser lifetime
relative to that candidate and removes 6,984 bytes of handshake overlap. R48's
smaller display-buffer watchdog experiment was rejected and reverted. No shared
system SDK modification, global allocator replacement or permanent pool is used.

Unverified: human voice-to-tool confirmation intent, actual new quoted-message
POST/delivery, acoustic quality, physical key feel, arbitrary network churn and
long-term fragmentation. Automatic tests sent no business messages. The user's
single original reconnect has no contemporaneous trace and remains unattributed.

Evidence: `build/device-checkpoint/assistant-controls-r53-native.json`, matching
`.uart.log`, `ordering_audit`, `build/assistant-controls-r53-source-hashes.json`,
`build/assistant-controls-r53-product-verified` and paired design ownership table.

## Focused startup and QR UI checks (2026-11-08)

The ordinary R62 APP builds and its merged-image layout verifies (2,254,256 APP
bytes; SHA256 `32c09dd859cf0f3967c20f4c78c98fe76511bc122c8a7bbdcc3be6ef6f699a01`).
Only affected UI paths were checked; the unrelated full matrix was not repeated.

An isolated PassportSim fixture drives the production rendering functions without
network/audio: two full-screen startup frames change, no header/footer pixels remain,
the message shortcut uses the new wording, and BOT selection, BOT authorization and
three XiaoZhi account states show one copy of each scan instruction/status. Test QR
content decodes. The startup-to-QR transition explicitly restores top-left alignment.
These eight screenshots prove rendering only; the fixture is never installed on a
physical device and does not prove real authorization, capture or cloud behavior.
The separate storage/authentication candidate remains under device qualification.

The startup now draws an envelope logo rather than binary text. Focused host tests
verify the 575ms boot arpeggio, sample bounds, four silent separators, termination
and the success-sound switch. It generates into the existing 128-sample playback
block, without another task, permanent PCM array or temporary heap allocation.
Hardware audibility has not been checked during silent tests.

R61's separate native run did not qualify: its old cold-cache preparation used
a full visit with nested lookups and took about990 seconds. The measured root
read then took1,528ms; four committed text-page reads peaked at543ms, without
a watchdog fault, but subsequent HTTP requests failed before obtaining a status.
This is not an overall PASS. The app/identity/font/table were restored safely.
R63 replaces that diagnostic preparation with RAM locator/proof invalidation;
host verification observes zero Flash IO and unchanged revision counters, followed
by322 reads for the same cold fixture. Real authorization is now required before
its device run can continue. The R62 logo/chime ordinary firmware remains built
and UI-checked, rather than claimed as a fully accepted device release.

## R64 targeted device checks after renewed authorization (2026-11-08)

The user completed device authorization. Ordinary R62 then returned real chat and
message HTTP200 responses; the first observed message request was slow (about76s
between parser allocation and stream completion). Later requests were faster.
This cold-load performance issue is not declared resolved by the checks below.

R64 uses the R63 diagnostic with the current ordinary R62 as BOTH success/failure
fallback. Authorization waiting must not restore the earlier R53 UI. The fixture
skips the unrelated complete matrix and never transmits a business message.

| Selected device check | Observed result |
| --- | --- |
| Selected message context while loading, quoted-draft target and activity renewal | PASS, same validated message target and refreshed activity |
| Cold credential load, RAM-only cold preparation | 975ms; IDLE progressed, no watchdog fault |
| Actual expired-access response and normal refresh | HTTP400 code99991668; refresh and full body retrieval pass in7410ms (682-byte body) |
| Four random local pages of a12KiB fixture | 4/4 pass; maximum10ms |
| Named Blue bot in the actual current group | Unique verified result; no real mention POST performed |
| Deliberately cleared SDK fd_sets | Same connection rearmed after24ms |
| Three cloud assistant turns, speech preemption, deferred-read resumption, capture retention and injected recovery | Selected checks pass; no unexpected watchdog/panic |
| Final ordinary firmware | R62 SHA256 `32c09dd859cf0f3967c20f4c78c98fe76511bc122c8a7bbdcc3be6ef6f699a01`; binding, identity, partition and font retained; silent preferences retained |

The legacy collector's overall result remains NOT PASS: its one unmet term was
the pending-confirmation keepalive marker. That marker was not observed; no claim
is made that this path passed, nor that the absence proves a transport failure.
The separate targeted audit records the listed scope without overwriting that
legacy result. Physical startup-sound audibility, natural voice intent leading
to an actual quoted/mention send, and cold-load latency remain unverified or open.

## R69 refresh/startup regression (2026-11-08)

Build and merged-image layout verify. Ordinary APP:2,255,664 bytes, SHA256
`99cf22c445826586c0012a6bdaa2062fcd0eac0458570a179fd9800604b90fec`.
Only affected checks were run; no unrelated complete matrix is claimed.

- Host live-preview merge checks pass: duplicate/older/deleted rejection, bounded
  rows, follow-latest and preservation of a reader's selection.
- Isolated rendering checks pass through both startup owners, initialization-logo
  wait, cached home without the old startup footer, and existing QR transitions.
  These are synthetic UI fixtures, not physical screenshots or network proof.
- APP-only COM7 write/readback passes; MAC, full data-pool MD5, identity, font and
  partition were checked and preserved before reboot. No timeout rollback exists.
- Native logs show bound1/auth_request0 and cached conversations ready at7697ms
  after boot, ahead of draft indexing and network list fetch. Real chat/preview
  requests returnHTTP200; enrichment stores zero full text bodies.

Natural incoming-message latency under use and post-send assistant resumption
still need a human device check. The connection handoff remains intentional;
this iteration does not claim uninterrupted voice connectivity during sending.

## R72 BOT-authorization preparation (2026-11-08)

The reported sequence is supported by source: the settings action queued a durable
authorization request then restarted into the ordinary onboarding splash; it had
not cancelled the existing read first. R72 changes that UI reentry path.

- Build/image verification passes; ordinary APP2,256,560 bytes, SHA256
  `03e6680edb6a42055bb4fc356b6a44cdf6d856f6f60eae9730388be6d2f66fb0`.
- Host policy checks pass for warm-software entry, one-shot consumption, cold reset
  and corrupted tag/complement rejection.
- The R71 isolated gallery exercised preparation-to-decodable-QR and pre-restart
  settings preparation with no ASCII logo or old login wording. R72 additionally
  clears redundant footer text and captures that condition under the UI lock;
  this text-only follow-up was reviewed without repeating unrelated UI scenarios.
- R72 APP-only device write/readback passes; identity, entire pool, font and table
  remain unchanged by flashing. Native startup reports bound1/auth_request1 and
  reaches a real authorization QR. That request was already opened by the user.

The physical settings click through its software reset still needs a user check.
Polling HTTP400 alone is not treated as evidence that the existing grant was revoked.
No new consent was performed, no old credentials were cleared for this test, and
waiting does not roll back the ordinary firmware.

## R74 cold-list and conversation-boundary fix, qualified by R75 (2026-11-08)

Build: PASS, ordinary APP 2,257,616 bytes; SHA256
3f2eb22af6b677e041098d1906c3a47db11c1665167cf57822b76ea665806a27.
Only affected checks were run; no unrelated full matrix was repeated.

The prior unchanged-firmware capture compared eight rows/six text bodies/5,741
response bytes: foreground body storage took7,748ms to list readiness, while later
preview-only reconciliation took1,272ms. First body arrival was1,355/1,246ms.
Streaming-feed time includes network and storage callbacks, so those logs do not
isolate every millisecond of Flash cost.

| Check | Result |
| --- | --- |
| Host parser, normal/pressure/error | Preview pagination and cursor retention, valid/zero/malformed/duplicate revisions and unresolved mentions pass; existing2MiB preview and120KB full-store callback fixtures pass in a fixed7,000-byte parser context |
| Host cache and navigation | Exact-revision body reuse, changed/unknown/deleted/wrong-ID rejection; ordinary, final-row, final-page and empty-list boundaries pass; existing model/page tests pass |
| Native preview list | Two eight-row samples ready in1,317/1,322ms, stored0; total calls including optional snapshot work1,966/1,839ms |
| Native on-demand body and local read |297-byte body fetched/committed in2,036ms; initial cached page5ms |
| Native persistent body link | RAM view dropped, actual framed cache reloaded in40ms with the same body ID; subsequent preview retained the body reference and local page read8ms |
| Native next conversation page | Actual navigation handler, asynchronous cache owner and network-miss owner completed the next page; server_has_next1 |
| Native page failure recovery | A missing current-cache key in diagnostic RAM failed the page operation, cleared loading and preserved the list; navigation remained usable |
| Final device | Ordinary R74 written/read back; binding retained, identity/table/font preserved; no panic/watchdog; muted fixture and zero business POSTs |

The first R74 diagnostic fixture incorrectly waited only on the main work queue
and reported page failure before the cache worker could finish. That failed
record is preserved. R75 corrected the fixture to wait for both owners, without
changing the qualified production code; all selected checks then passed.

The probe used a temporary, hash-guarded source override for its second build.
Production sources were restored exactly before device execution. Generated
development artifacts were also restored to the frozen ordinary bundle. Native
measurements are samples, not a bound on every message size or network. Natural
button feel and active-assistant concurrency were not rerun as a full matrix.


### R77: offline errors preserve navigation and their actual cause

- Build: PASS, verified ordinary APP 2,258,480 bytes, SHA256 `e72887886ae9d8befe3070afa9ea320fdf3c2beb880368ce27b5b087cae7136e`, bundle `build/ui-core-r77-product-verified`.
- Host tests: PASS, failed-load navigation with retained/current versus stale/other chat rows, Down, double Down, Back; known/unknown Wi-Fi reasons, user-disconnect precedence, connection recovery and shutdown-event preservation. Repository and storage-boundary checks pass.
- Device tests: PASS for the affected offline paths only. The attached device actually reported repeated reason 201; the application then stopped the driver (reason 36). R77 retained reason 201, exercised the real failed-message worker and button handlers, and verified failed pagination releases loading and retains navigation. `build/device-checkpoint/offline-r77-native.json` has all seven checks, including `no_ap=1`; raw UART is local only.
- The preceding R76 diagnostic proved navigation but recorded `no_ap=0`: a local stop event erased the earlier reason. It is retained as partial evidence, not the final cause-preservation acceptance.
- Ordinary R77 restored with APP readback, identity/table/font and data-pool write preservation, bound=1/auth_request=0; no business POST or audible test. Diagnostic hooks and RAM-only fixture rows are absent from ordinary firmware.
- Unverified: online message fetching after the access point becomes available, natural button feel and the physical red-text presentation. No unrelated full matrix was repeated.


### R78: saved-network fallback and independent local listing

- Build and focused host tests pass. Ordinary APP 2,259,776 bytes, SHA256 `46205132874d8d5564ed367fbc519e740b46e6c93e23018a5d9f3335d2635d0e`, bundle `build/ui-core-r78-product-verified`.
- Saved names now use the cache-worker queue, independent of the business worker's connection waits; an unfinished read is labelled loading rather than an empty saved list.
- Automatic join begins with the active saved entry, caps attempts at 8 seconds and uses one caller deadline. A RAM cursor continues later entries across failed batches. Successful connection updates the existing active choice; explicit offline/cancellation bypasses automatic join. No additional password cache is retained.
- `build/device-checkpoint/wifi-r78-native.json` passes local listing, fallback, cancellation, explicit offline, recovery and saved-catalog preservation. The actual business worker waited while the cache worker read two names in 2ms; state was observed ready at 49ms. This is publication timing, not a measured physical LCD frame.
- Fault injection changed only the first candidate in RAM to a nonexistent SSID (actual reason 201). The second attempt reused the original active network credentials, without changing stored passwords. The real radio reconnected in 12,238ms total; this verifies retry control and radio recovery, not availability of every remembered AP.
- Identity/table/font and pool preservation across APP writes are checked; no business messages or audible tests. Ordinary firmware excludes injection and hooks.
- The reported slow opening of Doubao has not been reproduced in full. The read-only R78 live capture contains a 5-row preview refresh in 1,191ms, but began after the original opening. `MESSAGE_OPEN` and `MESSAGE_LOAD_BEGIN` markers were added for the next reproduction. Online opening latency and physical interaction remain pending.


### R79: About only, then an authorized clean reinstall

- Build: PASS, version `0.2.0-r79`, APP 2,260,032 bytes, SHA256 `7e58f7162cb265a38fab5c1fda223c997c3f9717f76f087b08654a77b4fdbb02`, bundle `build/ui-core-r79-product-verified`. `version.txt` drives both ESP-IDF metadata and the About display. The first build failed for the missing explicit `esp_app_format` dependency; the dependency was added and the build rerun successfully.
- Host test: About navigation only, entry/read-only actions/back focus/adjacent return item PASS. No unrelated functional matrix.
- Device test: About only PASS: actual Settings entry, title, author `cexer@qq.com`, exact metadata version, visible content within the header/footer bounds, and Back returning focus to About. Report `build/device-checkpoint/about-r79-clean-install.json`. This checks LVGL output/geometry; no physical photograph or font-quality judgement.
- After that pass, the user-authorized clean reinstall erased and verified blank the APP slot `0x10000+0x330000`, NVS `0x9000+0x6000`, legacy cache `0x360000+0x10000`, and shared pool `0x370000+0x100000`; it then wrote and readback-verified ordinary R79. Bootloader/table, PHY, board identity and font regions were preserved. Saved Wi-Fi, BOT credentials/tokens, drafts, caches and local preferences were intentionally discarded.
- The device is left in download mode without a final reset, to keep testing silent and leave the first clean boot to the user. No diagnostic code remains installed. First provisioning, BOT selection and authorization have not been exercised after this wipe. Reboot is required.


### R80 power policy: corrected device gate passed

- Build: PASS for corrected 0.2.1-r80, APP 2,280,912 bytes, SHA256 `c6e80a139e90099c83d320dcbe7c4a92673a8ae328e1b234f275c00538a87382`, bundle `build/ui-core-r80-product-verified`. PM, tickless idle and USB-connected sleep protection are enabled.
- Host tests: PASS, policy defaults/mode combinations/thresholds/audio-work guards/invalid records/prewarm wrap; real-manager wake consumption, 1,000 unchanged ticks without Wi-Fi churn, retry backoff and offline behavior; settings persistence/reopen/corruption and the neighboring About navigation.
- First device gate: FAIL. Actual UI/persistence passed; at 120,300ms screen off, application CPU saving and Wi-Fi MIN_MODEM passed; wake consumption and forced On/Off passed. Serial output then stopped during a saving-mode HTTPS test. The SDK configuration had USB-connected sleep protection disabled; ESP32-C3 USB Serial/JTAG cannot survive Light-sleep in that configuration. The bounded monitor timed out, and the attempted ordinary-APP restore failed with Windows device error 31. Exact failed bundles and UART are retained in `build/power-r80-usb-failure`.
- Corrective configuration: `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`. The SDK holds a No-Light-Sleep lock while attached to a USB host and releases it after disconnect, without a new application task. A scoped restart of this exact USB device was denied by Windows; no other device or hub was restarted.
- Corrected USB2 device gate: PASS. Actual four-row UI and persistence; real idle 120,301ms, first-key wake consumption, forced modes and saving-mode HTTPS; real prewarm expiry 60,002ms with audio protection and no passive rearm; page-entry cache/online refresh; 20 seconds foreground-only with zero background requests. No panic/watchdog was captured. Report: `build/device-checkpoint/power-focused-r80-usb2-native.json`, with matching UART log. Tests were silent and sent no business messages.
- The corrected ordinary R80 APP was installed and read-back verified; temporary probe is inactive. Bootloader, partition table, identity, font and data ranges were verified across APP writes; binding remained present. Left in download mode without reboot to keep testing silent; a physical restart is needed to run the ordinary build.
- Startup observation: the recovery controller deliberately resets after diagnostic flashing, so a second startup was expected during this recovery. The earlier user-observed frozen first start was not separately captured and cannot be declared crash-free. In the captured corrected boot, configuration check completed at 10,721ms and cached conversations became ready at 14,175ms. This bounds the local initialization/cache wait, not a measured animation-duration constant; its finer breakdown remains unverified.
- Unverified: electrical current, battery endurance, actual battery-mode Light-sleep residency and natural wake-key feel. USB-host protection intentionally prevents Light-sleep while attached; application CPU/Wi-Fi policy transitions are verified separately.
