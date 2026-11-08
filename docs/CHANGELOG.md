<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- Add versioned `release/` BIN archives and `tools/prepare_release.py` with layout, source and SHA256 checks.

- Add Power saving settings: 2–10 minute screen-off timeout (default 2), CPU and Wi-Fi On/Off/Auto modes (Auto after two idle minutes), and foreground/background/automatic refresh (foreground-only by default). Wake keys are consumed safely; voice/audio and active operations retain power locks. Microphone prewarming expires after one minute without being rearmed by passive refresh.

- Add Settings → About with author `cexer@qq.com` and the firmware version from ESP-IDF application metadata; version this build as `0.2.0-r79`.

- Read the saved Wi-Fi list on the local-cache worker and show loading explicitly. Automatic connection rotates through saved networks with an 8-second attempt budget and caller-wide deadline; later entries continue on the next batch, and intentional disconnect remains offline. Add message-open timing markers for diagnosis.

- Keep message-load failures on the navigable message page, retain same-conversation rows and discard stale cross-conversation rows. Show specific Wi-Fi failures (including missing access points) in message, conversation-page and initialization errors instead of a generic load failure.

- Show conversation message lists and older pages from bounded previews without synchronous body storage; fetch full text on open and retain valid body references in the framed page cache. Reuse cached bodies only for an exact known message revision. Short Down at the last conversation row now opens the next page, or reports the final page.

- Treat a settings-triggered BOT authorization restart as authorization preparation: cancel optional reads, omit duplicate loading text, and skip the cold-boot artwork/chime using a one-shot RAM hint. Normal power-on branding and durable credential checks remain intact.

- Keep the envelope startup screen through application initialization, show cached conversations before draft indexing, poll the visible message page with lightweight latest-message previews, and resume the enabled assistant before optional post-send history refresh. Sending still temporarily releases the voice connection to avoid a TLS memory peak.

- Replace the return-to-messages shortcut wording, remove repeated QR scan instructions, and use a green ASCII envelope logo with orbiting sparks and a 575ms electronic rising arpeggio, without header/footer chrome (controlled by the success-sound switch). Reset label alignment when leaving the animation so QR captions remain visible.

- Reduce simultaneous listening/read peaks with response-lifetime GET RX, deferred/released request scratch and body-time parser allocation; preserve active audio capacity and UI interactions. Full R53 native regression passes, without claiming indefinite stability.

- Recognize both SDK and errno EAGAIN results while polling HTTP response headers; preserve real errors and the same request without replay.

- Add later-turn voice send confirmation with revision/duplicate protection, true message reply targets in collaborative editing, and persisted assistant/sound settings tools plus settings-page navigation.

- Execute assistant page-opening tools before acknowledging their result; retain conversation content while loading, support the current conversation as the collaborative-editor recipient, and retire stale dictation pages during global assistant takeover.

- Keep the existing microphone task/queue across ordinary reads; prioritize detected speech over cancellable GET work, sample battery without capture teardown, and coalesce passive refresh to 30 seconds with capture retained.

- Keep the draft and recipient visible while sending; resume the same assistant activation without another greeting or timer reset. Request concise replies during protocol initialization and distinguish silent maintenance from an actual command.

- Fix assistant activation failure from idle audio backlog and UI repaint/lock contention without expanding the audio pool; retain onset preroll, isolate audio-backpressure errors, and allow a bounded two-second binary write without ambiguous replay.

- Repair a persistent provisioning trap with one-shot portal requests and remembered-network retry; preserve business OK during assistant listening, add persistent XiaoZhi volume and two-second defaults, move the badge beside battery, and show an animated startup screen with optional electronic feedback.

- Keep the assistant active across business pages, expose current-page/draft MCP tools and real structured mentions, add independent feedback switches, and unify full-width separators and the persistent breathing status badge. Reduce active background fetches to previews and release reply decoding before nested HTTPS.

- Add revision-checked collaborative draft editing and one volatile undo snapshot; preserve recipient/reply, keep assistant prose out of outgoing text, and require explicit send confirmation.

- Keep one global draft, allow complete short messages to send from volatile RAM after storage failure, show the immutable recipient in review, and expose explicit delete/clear actions.
### Continuous assistant controls

- Highlight assistant entry, double-OK shortcut, automatic greeting and continuous listening.
- Add XiaoZhi account/assistant subpages, boot preference, silence or OK submission.
- Preserve physical send confirmation and phase-separated audio memory.



- Add an experimental XiaoZhi assistant entry separate from ordinary dictation. Advertise bounded MCP tools for conversation lookup, latest-message previews, deferred navigation and device-confirmed drafts. Consume negotiated Opus reply audio with bounded packet/PCM buffers and release it after each voice turn. Message announcement work is paused; it is not part of this assistant path.

- Keep Add network and manual provisioning fixed below the Wi-Fi list with whitespace separation and default focus on Add. Add per-network Connect, Disconnect, Forget and Set password actions, explicit empty-list persistence, rollback using live credentials after a bad saved-password edit, and two-way double-click focus shortcuts alongside ordinary list/action boundary navigation. Include page-owned state in repaint detection so focus and editor changes render immediately.

- Add network settings with up to ten remembered Wi-Fi profiles, nearby-network selection, a clear-text password keyboard and a phone provisioning entry. Device-side switching commits only after connection succeeds and attempts the previous network on failure. Use BOT wording throughout the account selection/authorization screens.

- Fetch XiaoZhi pairing information on entry and renew missing/expired codes in the foreground, with one request at a time, a retry cooldown and cancellation on exit. Show a lazy, page-owned QR linking to the official console, and use distinct title/metadata colors in draft rows.

- Use the same yellow outlined focus rows for account, image and draft actions. Move draft selection counts beside footer controls, keep pairing errors only in the red status line, remove duplicated headings, and add content spacing below the header while preserving message-list spacing.

- Use one header/content/footer frame for setup and messenger pages, with battery status, separator rules and distinct blue/green setup QR codes. Add DOWN double-click to focus the composer and a XiaoZhi settings entry for bounded activation-code/status queries; unpaired voice attempts lead to pairing guidance without discarding existing drafts.

- Simplify settings to compact text-only rows, separate footer actions with measured pixel gaps, and keep recording status and elapsed time on one line. Check the shared layout with simulator setup, messaging, recording, draft and error states; simulator examples do not replace device acceptance.

- Use frame-scaled six-bit PCM for the transient XiaoZhi queue: save 5,688 bytes without shortening the 2.4-second backlog. Reject the smaller four-bit candidate after transcript regression; the final codec passes unchanged speech fixtures and native pressure/recovery checks.

- Pause the optional cache worker while the microphone is prewarmed, and resume it on focus loss, failure or recording completion. Add opt-in read-only device MCP and bounded memory diagnostics; the ordinary messenger does not expose MCP tools.

- Start recording with one composer confirmation and optionally prewarm the silent microphone on focus. Refresh the current conversation after successful sends; fit variable-height bubbles, redesign settings cards, soften text colors and use the same-size Medium font. Preserve matching APP/font rollback and existing bindings/drafts.

- Compare all matching Wi-Fi APs by signal when no BSSID is pinned, instead of stopping at the first SSID match on a saved channel. Device tests found a stronger same-SSID AP without moving the device; XiaoZhi voice cases and the persistent review/send path then passed.

- Erase each 16 KiB FlashDB sector's payload units before its header unit. A reset between physical 4 KiB erases now retains the GC recovery marker. Regression tests cut after every unit, reopen and verify retained data and subsequent writes.

- Add an on-demand XiaoZhi ASR candidate using authenticated TLS WebSocket and 20 ms PCM frames with a bounded local IMA4 queue. Keep speech review and explicit Feishu send confirmation; discard downstream speech playback and reject entirely zero capture. Pause the optional local-cache worker during audio allocation and restore it after audio/network teardown. Retain the Feishu backend as an explicit build option. Native qualification is required before delivery.

- Redesign the messenger and owned setup pages with a dark navy/teal palette and 70% backlight. Separate recognition text from status/actions, remove leading detail whitespace, use bounded 512-byte text windows, show six compact conversation rows and four message previews, and add a battery badge. Verify the 520 mAh fuel-gauge profile and update SOC without depending on an idle network queue.

- Pin LVGL 9.5.0 and preserve the 24 KiB UI pool. Firmware verification now rejects an actual ELF whose static LVGL pool silently expands to 64 KiB during dependency resolution.

- Overlap microphone settling with ASR connection preparation using the existing capture task; publish message lists before optional snapshot/read-state persistence, cancel obsolete message reads when switching conversations, warm cold snapshot chunk locators in a single bounded-table scan, and trace readiness latency. Retain the two-second microphone settling guard and the first retained frame before recording readiness.

- Reclaim bounded batches of unreachable chunks and coalesce small cold-object evictions into one atomic catalog update. Preserve active readers and protected drafts; use transient allocation with a one-key fallback rather than a resident cache.

- Reuse unpinned, unreferenced retired FlashDB catalog slots before reporting catalog exhaustion. Avoid whole-pool orphan scans for routine draft-state updates while retaining live/protected versions, active readers, atomic publication and non-repeating IDs.

- Keep voice capture in the preparing state during a bounded two-second microphone pre-read; show recording only after the first retained frame. Reuse the existing PCM buffer, support cancellation during preparation, and keep the settling interval out of the recording duration. Physical confirmation of the reported missing prefix remains required.

- Review and send ordinary short voice replies from bounded RAM without requiring a persistent draft or database slot. Reject oversized recognition explicitly, retain existing durable drafts, queue locally compressed audio and batch six decoded capture blocks per request, and release capture buffers before waiting for the server. Allocate optional burst buffers only while recording and keep the preparing indicator until capture starts.

- Refresh onboarding status within a page, distinguish Wi-Fi connection from Feishu service setup, and allow long-press cancellation back to password-free Wi-Fi setup. Accept 514-byte device codes with exact-sized poll forms and cancellable registration requests.

- Make the first-run device hotspot password-free and remove its password from the setup screen.

- Show the password-free device hotspot and portal address directly during first-run Wi-Fi setup; retain QR codes for Feishu app selection and user authorization. Use 14 px, 2 bpp grayscale glyphs with full CJK coverage and the existing line height.

- Flush pending SQLite journal bytes before truncation so later reads or close cannot regrow the truncated tail; propagate flush failures without proceeding to truncate.

- Bound local short-write/interruption/ENOSPC recovery across nested cache operations; reuse existing DB pages during draft streaming and release them before TLS setup and after transfer, preserving unconfirmed-send state.

- Acquire audio codec/I2S DMA only while recording and release it on completion or failure; release temporary conversation pages before message prefetch, defer stream parsing workspace, and skip unchanged UI redraws to reduce TLS memory peaks.

- Added on-device app selection/creation after Wi-Fi setup, separate same-app reauthorization and app/account switching, and candidate credentials committed only after user authorization. First-run Wi-Fi forms no longer require application secrets.

- Added USB first-run Wi-Fi setup and a host-only official SDK flow for selecting/creating the owner app, automatically transferring credentials and forwarding the user authorization link without reading the device screen.

- Added database-corruption isolation that retains failed DB/journal generations, rebuilds within the shared pool, preserves independent settings, rejects stale handles/draft updates, and explains unavailable old drafts.

- Added eight-row message-history pagination, compressed history-page caches and offline reading; invalid pagination aborts before body publication, and reconnect retries retain the active page.

- Added fixed-window conversation pagination with compressed page caches; invalid cursors and cancellation retain the current page, while cold pages remain reclaimable.

- Added a global protected-draft manager, immutable saved destinations during re-recording, and on-read mention normalization without storing a second full message body.

- Add persistent conversation/message caches, offline startup and compressed long-text paging with reading-position restoration; migrate configuration and Wi-Fi into the shared pool after verification, isolate tokens across bindings, and return cache file space to necessary updates through incremental vacuum.
- Preserve binding on permission, malformed-response and transient failures; reauthorize only after explicit refresh rejection, retaining the owner app. Retry failed startup and show message-page send feedback.
- Exclude unused product demo pages, optimize application/LVGL size, and move disposable caches outside the protected Passport identity window with validation coverage.

- Replaced publisher-built Feishu credentials with advanced owner provisioning: generic Web Serial firmware contains no private app, the same page can write an owner's App ID/Secret locally over USB, and the device then obtains and stores its own user grant.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `after-release`, `file-issues`, and experience-index documents.
- Fixed Feishu device-code polling to use the v2 OAuth token endpoint; pending authorization now keeps waiting, transient failures retry, and expired QR codes refresh automatically.
- Replaced the limited LVGL CJK subset with a Flash-resident 1-bit Source Han Sans device font covering U+4E00-U+9FFF and Chinese punctuation, so onboarding and dynamic Feishu message text render without missing-glyph squares.
- Added a direct-device Feishu messenger MVP with encrypted BLUFI provisioning, conversation unread markers, recent-message browsing, selected-message replies, streaming native ASR, review/re-record/cancel actions, and physical send confirmation.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.

- Distinguish app-selection and personal-authorization QR pages, separate bottom copy, and show bounded two-line message prefixes. Trial full-CJK 4 bpp immutable mapped font assets with integrity checks; request ASR permissions as tenant identity and display permission failures explicitly.

- Avoid repeated disk scans for a verified-empty outbox; invalidate empty knowledge on writes/scope/recovery and allow cancelling cold lookups. Split footer status/errors from page/key guidance on a coordinated blue-gray panel.

- Avoid redundant SQLite auto_vacuum setter and pin-reset transactions at startup; add source-level journal-failure and recovery tests.

- Rank SPIFFS garbage-collection candidates by net page recovery under severe pressure, accounting for extra index rewrites; retain normal-space wear-leveling and existing on-flash format.

- Use FlashDB in the shared 1 MiB application pool with fresh setup after an authorized reset; keep SQLite as an explicit build alternative. Required configuration, cached objects and draft states use the same serialized pool.

- Avoid duplicated rich-text message bodies when Feishu returns both canonical content and content_v2; ignore unsupported nested representations.

- Retry failed onboarding QR draws once per second, report failure visibly, and mark a URL rendered only after a successful draw.

- Restore persistent voice drafts and offline review retention; release capture resources before END and retain more runtime heap for TLS while bounding the compressed queue.
