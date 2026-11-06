# Bounded memory diagnostic

[简体中文](README.zh_CN.md)

Explicitly enable `FEISHU_MEMORY_DIAGNOSTIC=1` for a diagnostic build. After
normal owner authentication, twenty cycles each isolate direct codec use,
short-lived microphone tasks, real message reads/cache publication, and settled
microphone plus message reads. No audio is uploaded or played; no message is sent.
The existing identity, bindings, drafts and pool remain in use.

Each cycle records free/largest/minimum heap, task count and heap integrity.
Optional standalone heap tracing with stack depth four records unreleased
allocations in the two audio phases. Tracing overhead and ordinary runtime
measurements must be distinguished. Stop on integrity failure or less than
60,000 bytes of settled heap. Decode program counters using the matching ELF.

Use a verified ordinary image as a mandatory restore target. Flash only the APP
at its verified offset; verify the unchanged partition table, identity window,
matching font and successful ordinary bound startup after the diagnostic.
