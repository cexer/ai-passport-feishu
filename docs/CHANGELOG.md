<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

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
